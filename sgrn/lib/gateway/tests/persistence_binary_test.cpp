// Binary WAL regression test for the DeltaSnapshot -> PersistenceService path.
//
// A multi-DB DeltaSnapshot event leaves TelemetryEvent::db at 0 and carries
// the dirty DB numbers in TelemetryEvent::dirty_dbs (populated by
// GatewayApplication::wireTelemetry). PersistenceService must snapshot each
// listed DB through the binary (anchor/delta) frame path. Before the fix it
// gated that path on `t_event.db > 0`, so no DeltaSnapshot ever produced a
// binary data frame.
//
// The test configures a PersistenceService in isolation (format="binary"),
// publishes three DeltaSnapshot events for one DB, then asserts on the
// archive bytes:
//   1. first frame for the DB is an anchor (0xFFFD),
//   2. second frame is a delta (0xFFFE) when the diff is < 50% of the image,
//   3. a keyframe (anchor) is re-emitted after binary_keyframe_interval_ms_.
// Finally the archive is run through recoverStateFromArchives() and the
// recovered twin image is asserted byte-equal to the last write.
//
// Scenario 2 configures the service with a real PlcSchemaStore: the v4
// header must then carry the binary schema encoding ("SGRS"), recovery with
// the matching store must succeed, and recovery with a different store must
// be rejected as a schema mismatch.

#include <sgrn/gateway/core/GlobalContext.hpp>
#include <sgrn/gateway/core/RecoveryEngine.hpp>
#include <sgrn/gateway/core/TelemetryBroker.hpp>
#include <sgrn/gateway/database/PersistenceService.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/PlcState.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/schema/SchemaSerializer.hpp>
#include <sgrn/scl/types/DbField.hpp>
#include <sgrn/scl/types/DbSchema.hpp>
#include <sgrn/utils/compression.hpp>

#include <asio.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

int g_failures = 0;

#define CHECK(cond)                                                                                                                        \
    do {                                                                                                                                   \
        if (!(cond)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                                    \
        }                                                                                                                                  \
    } while (0)

namespace fs = std::filesystem;
using sgrn::gateway::core::EventType;
using sgrn::gateway::core::TelemetryBroker;
using sgrn::gateway::core::TelemetryEvent;
using sgrn::gateway::database::PersistenceService;
using sgrn::gateway::twin::PlcMemory;
using sgrn::gateway::twin::PlcState;

constexpr uint16_t kDb = 7;
constexpr size_t kImageSize = 64;

void publishDeltaSnapshot() {
    TelemetryEvent ev;
    ev.type = EventType::DeltaSnapshot;
    ev.db = 0; // multi-DB event: the DB list travels in dirty_dbs
    ev.dirty_dbs = {kDb};
    ev.json_value = std::make_shared<std::string>("{}");
    ev.timestamp = 0; // wall clock applies
    TelemetryBroker::instance().publish(std::move(ev));
}

std::string readFile(const fs::path& t_path) {
    std::ifstream file(t_path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    using sgrn::gateway::config::PersistenceConfig;

    const fs::path state_dir = fs::temp_directory_path() / ("sgrn_persistence_binary_test_" + std::to_string(::getpid()));
    fs::remove_all(state_dir);

    // The twin image under test. Mutated on the main thread between publishes
    // (with quiesce waits after each publish, so the broker strand never
    // races the mutation); read back through read_db_fn_ on the strand.
    std::vector<uint8_t> image(kImageSize, 0);
    std::mutex image_mutex;
    auto read_db = [&](uint16_t t_db) -> sgrn::Result<std::vector<uint8_t>> {
        if (t_db != kDb)
            return sgrn::Result<std::vector<uint8_t>>::Error("unknown DB");
        std::lock_guard<std::mutex> lk(image_mutex);
        return image;
    };

    sgrn::gateway::core::GlobalContext::instance().run(2, 2);
    asio::thread_pool heavy_pool(2);

    PersistenceConfig cfg;
    cfg.enabled = true;
    cfg.format = "binary";
    cfg.mode = "changes_with_timestamp";
    cfg.batch_size = 1000000;    // never rotate mid-test
    cfg.batch_interval_s = 3600; // never rotate mid-test
    cfg.anchor_interval_s = 1;   // binary keyframe every 1s
    cfg.zstd_level = 1;

    PersistenceService svc(&heavy_pool);
    {
        auto res = svc.configure(cfg, state_dir.string(), /*db=*/nullptr, /*schema_json=*/"", /*schema_store=*/nullptr, read_db);
        CHECK(!res.hasError());
        if (g_failures != 0)
            return 1;
    }

    auto set_image = [&](uint8_t t_b0, uint8_t t_b1) {
        std::lock_guard<std::mutex> lk(image_mutex);
        image[0] = t_b0;
        image[1] = t_b1;
    };
    auto quiesce = []() { std::this_thread::sleep_for(std::chrono::milliseconds(300)); };

    // Frame 1 for DB7: first sight -> anchor (0xFFFD).
    set_image(1, 0);
    publishDeltaSnapshot();
    quiesce();

    // Frame 2: one byte differs (11-byte delta << 50% of the 64-byte image)
    // and no keyframe is due -> delta (0xFFFE).
    set_image(2, 0);
    publishDeltaSnapshot();
    quiesce();

    // Frame 3: past the 1s keyframe interval -> anchor again.
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    set_image(2, 3);
    publishDeltaSnapshot();
    quiesce();

    std::vector<uint8_t> expected_v3;
    {
        std::lock_guard<std::mutex> lk(image_mutex);
        expected_v3 = image;
    }

    svc.stop();
    heavy_pool.join(); // finalizeArchive()'s rename lands here

    // Locate the single finalized archive.
    std::vector<fs::path> archives;
    for (auto it = fs::recursive_directory_iterator(state_dir); it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_regular_file() && it->path().filename().string().ends_with(".bin.zst"))
            archives.push_back(it->path());
    }
    CHECK(archives.size() == 1);
    if (g_failures != 0)
        return 1;

    auto dec_res = sgrn::utils::compression::decompressStringZstd(readFile(archives.front()));
    CHECK(!dec_res.hasError());
    if (g_failures != 0)
        return 1;
    const std::string raw = std::move(dec_res).value();

    sgrn::gateway::database::BinaryWalHeader header;
    CHECK(sgrn::gateway::database::checkBinaryWalHeader(raw, header) == sgrn::gateway::database::BinaryHeaderStatus::kOk);

    // Collect the data-frame marker sequence and validate each payload.
    std::vector<uint16_t> data_markers;
    std::vector<std::vector<uint8_t>> anchor_images;
    size_t pos = header.frames_start;
    sgrn::gateway::database::BinaryFrame frame;
    while (true) {
        const auto status = sgrn::gateway::database::decodeBinaryFrame(raw, pos, frame);
        if (status == sgrn::gateway::database::BinaryFrameStatus::kEnd)
            break;
        CHECK(status == sgrn::gateway::database::BinaryFrameStatus::kOk);
        if (frame.db == sgrn::gateway::database::kControlFrameDbNum)
            continue; // dictionary / manifest / footer carry no twin data
        data_markers.push_back(frame.db);
        if (frame.db == sgrn::gateway::database::kAnchorFrameDbNum) {
            uint16_t anchor_db = 0;
            const uint8_t* anchor_image = nullptr;
            size_t anchor_len = 0;
            CHECK(sgrn::gateway::database::verifyAnchorFrame(frame.payload, frame.payload_len, anchor_db, anchor_image, anchor_len));
            CHECK(anchor_db == kDb);
            CHECK(anchor_len == kImageSize);
            anchor_images.emplace_back(anchor_image, anchor_image + anchor_len);
        } else if (frame.db == sgrn::gateway::database::kDeltaFrameDbNum) {
            uint16_t delta_db = 0;
            std::vector<sgrn::gateway::database::BinaryDeltaRun> runs;
            CHECK(sgrn::gateway::database::parseDeltaRuns(frame.payload, frame.payload_len, kImageSize, delta_db, runs));
            CHECK(delta_db == kDb);
            CHECK(!runs.empty());
        } else {
            std::printf("FAIL unexpected data frame db=0x%04x\n", frame.db);
            ++g_failures;
        }
    }

    // anchor, delta, anchor(keyframe) — in that order.
    CHECK(data_markers.size() == 3);
    if (data_markers.size() == 3) {
        CHECK(data_markers[0] == sgrn::gateway::database::kAnchorFrameDbNum);
        CHECK(data_markers[1] == sgrn::gateway::database::kDeltaFrameDbNum);
        CHECK(data_markers[2] == sgrn::gateway::database::kAnchorFrameDbNum);
    }
    // First anchor carries v1, keyframe anchor carries v3.
    CHECK(anchor_images.size() == 2);
    if (anchor_images.size() == 2) {
        CHECK(anchor_images[0][0] == 1 && anchor_images[0][1] == 0);
        CHECK(anchor_images[1] == expected_v3);
    }

    // Boot-recovery round-trip: the recovered twin image must equal v3.
    {
        PlcState fresh_state;
        PlcMemory fresh_mem;
        fresh_mem.attachState(fresh_state);
        CHECK(!fresh_mem.registerDb(kDb, kImageSize).hasError());
        sgrn::scl::PlcSchemaStore schema_store; // empty embedded schema accepts anything
        auto rec_res = sgrn::gateway::core::recoverStateFromArchives(state_dir.string(), fresh_state, schema_store);
        CHECK(!rec_res.hasError());
        if (!rec_res.hasError()) {
            std::vector<uint8_t> recovered(kImageSize, 0);
            CHECK(!fresh_mem.readDbMemory(kDb, 0, kImageSize, recovered.data()).hasError());
            CHECK(recovered == expected_v3);
        }
    }

    fs::remove_all(state_dir);

    // --- Scenario 2: store-backed service stamps a v4 binary schema -------
    {
        sgrn::scl::DbSchema db_schema;
        db_schema.db_number = kDb;
        db_schema.db_name = "DB7";
        db_schema.size_bytes = static_cast<int>(kImageSize);
        sgrn::scl::DbField counter;
        counter.name = "counter";
        counter.type = sgrn::scl::DataType::Int;
        counter.offset = 0;
        counter.count = 1;
        db_schema.fields.push_back(counter);

        sgrn::scl::PlcSchemaStore live_store;
        CHECK(!live_store.addDb(std::move(db_schema)).hasError());
        if (g_failures != 0)
            return 1;

        const fs::path state_dir2 = fs::temp_directory_path() / ("sgrn_persistence_binary_v4_" + std::to_string(::getpid()));
        fs::remove_all(state_dir2);

        std::vector<uint8_t> image2(kImageSize, 0);
        image2[0] = 9;
        std::mutex image2_mutex;
        auto read_db2 = [&](uint16_t t_db) -> sgrn::Result<std::vector<uint8_t>> {
            if (t_db != kDb)
                return sgrn::Result<std::vector<uint8_t>>::Error("unknown DB");
            std::lock_guard<std::mutex> lk(image2_mutex);
            return image2;
        };

        asio::thread_pool heavy_pool2(2);
        PersistenceService svc2(&heavy_pool2);
        {
            auto res = svc2.configure(cfg, state_dir2.string(), /*db=*/nullptr, live_store.toJson(), &live_store, read_db2);
            CHECK(!res.hasError());
            if (g_failures != 0)
                return 1;
        }
        publishDeltaSnapshot();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        svc2.stop();
        heavy_pool2.join();

        // Header must be version 4 with a binary ("SGRS") schema payload.
        std::vector<fs::path> archives2;
        for (auto it = fs::recursive_directory_iterator(state_dir2); it != fs::recursive_directory_iterator(); ++it) {
            if (it->is_regular_file() && it->path().filename().string().ends_with(".bin.zst"))
                archives2.push_back(it->path());
        }
        CHECK(archives2.size() == 1);
        if (archives2.size() == 1) {
            auto dec2 = sgrn::utils::compression::decompressStringZstd(readFile(archives2.front()));
            CHECK(!dec2.hasError());
            if (!dec2.hasError()) {
                const std::string raw2 = std::move(dec2).value();
                uint16_t ver = 0;
                uint32_t schema_len = 0;
                std::memcpy(&ver, raw2.data() + 4, sizeof(ver));
                std::memcpy(&schema_len, raw2.data() + 6, sizeof(schema_len));
                CHECK(ver == sgrn::gateway::database::kBinaryWalVersion);
                CHECK(schema_len > 0);
                CHECK(sgrn::scl::isBinarySchemaPayload(std::string_view(raw2.data() + 10, schema_len)));
            }
        }

        // Matching store recovers the image; a different store is rejected.
        {
            PlcState fresh_state;
            PlcMemory fresh_mem;
            fresh_mem.attachState(fresh_state);
            CHECK(!fresh_mem.registerDb(kDb, kImageSize).hasError());
            auto rec_res = sgrn::gateway::core::recoverStateFromArchives(state_dir2.string(), fresh_state, live_store);
            CHECK(!rec_res.hasError());
            if (!rec_res.hasError()) {
                std::vector<uint8_t> recovered(kImageSize, 0);
                CHECK(!fresh_mem.readDbMemory(kDb, 0, kImageSize, recovered.data()).hasError());
                CHECK(recovered == image2);
            }
        }
        {
            PlcState fresh_state;
            PlcMemory fresh_mem;
            fresh_mem.attachState(fresh_state);
            CHECK(!fresh_mem.registerDb(kDb, kImageSize).hasError());
            sgrn::scl::PlcSchemaStore other_store; // schema mismatch
            CHECK(sgrn::gateway::core::recoverStateFromArchives(state_dir2.string(), fresh_state, other_store).hasError());
        }

        fs::remove_all(state_dir2);
    }

    // --- Scenario 3: no store + JSON text falls back to a v3 header -------
    // Legacy writers (and readers) only know the JSON schema encoding; this
    // pins both the fallback writer path and the v3 reader branch.
    {
        sgrn::scl::DbSchema db_schema;
        db_schema.db_number = kDb;
        db_schema.db_name = "DB7";
        db_schema.size_bytes = static_cast<int>(kImageSize);
        sgrn::scl::PlcSchemaStore live_store;
        CHECK(!live_store.addDb(std::move(db_schema)).hasError());
        if (g_failures != 0)
            return 1;

        const fs::path state_dir3 = fs::temp_directory_path() / ("sgrn_persistence_binary_v3_" + std::to_string(::getpid()));
        fs::remove_all(state_dir3);

        std::vector<uint8_t> image3(kImageSize, 0);
        image3[0] = 5;
        auto read_db3 = [&](uint16_t t_db) -> sgrn::Result<std::vector<uint8_t>> {
            if (t_db != kDb)
                return sgrn::Result<std::vector<uint8_t>>::Error("unknown DB");
            return image3;
        };

        asio::thread_pool heavy_pool3(2);
        PersistenceService svc3(&heavy_pool3);
        // No store, but a JSON schema text: v3 header with the JSON payload.
        {
            auto res = svc3.configure(cfg, state_dir3.string(), /*db=*/nullptr, live_store.toJson(),
                /*schema_store=*/nullptr, read_db3);
            CHECK(!res.hasError());
            if (g_failures != 0)
                return 1;
        }
        publishDeltaSnapshot();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        svc3.stop();
        heavy_pool3.join();

        std::vector<fs::path> archives3;
        for (auto it = fs::recursive_directory_iterator(state_dir3); it != fs::recursive_directory_iterator(); ++it) {
            if (it->is_regular_file() && it->path().filename().string().ends_with(".bin.zst"))
                archives3.push_back(it->path());
        }
        CHECK(archives3.size() == 1);
        if (archives3.size() == 1) {
            auto dec3 = sgrn::utils::compression::decompressStringZstd(readFile(archives3.front()));
            CHECK(!dec3.hasError());
            if (!dec3.hasError()) {
                const std::string raw3 = std::move(dec3).value();
                uint16_t ver = 0;
                uint32_t schema_len = 0;
                std::memcpy(&ver, raw3.data() + 4, sizeof(ver));
                std::memcpy(&schema_len, raw3.data() + 6, sizeof(schema_len));
                CHECK(ver == 3);
                CHECK(schema_len > 0);
                CHECK(!sgrn::scl::isBinarySchemaPayload(std::string_view(raw3.data() + 10, schema_len)));
            }
        }

        PlcState fresh_state;
        PlcMemory fresh_mem;
        fresh_mem.attachState(fresh_state);
        CHECK(!fresh_mem.registerDb(kDb, kImageSize).hasError());
        auto rec_res = sgrn::gateway::core::recoverStateFromArchives(state_dir3.string(), fresh_state, live_store);
        CHECK(!rec_res.hasError());
        if (!rec_res.hasError()) {
            std::vector<uint8_t> recovered(kImageSize, 0);
            CHECK(!fresh_mem.readDbMemory(kDb, 0, kImageSize, recovered.data()).hasError());
            CHECK(recovered == image3);
        }

        fs::remove_all(state_dir3);
    }

    // Tear down the broker strand before exit (its threads are joinable).
    sgrn::gateway::core::GlobalContext::instance().stop();

    if (g_failures == 0)
        std::printf("persistence_binary_test: ALL CHECKS PASSED\n");
    else
        std::printf("persistence_binary_test: %d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
