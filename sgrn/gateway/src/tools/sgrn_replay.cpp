#include <sgrn/gateway/tools/sgrn_replay.hpp>

#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/gateway/core/TelemetryBroker.hpp>
#include <sgrn/gateway/twin/utils.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/time.hpp>
#include <cctype>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

namespace sgrn::gateway
{

namespace
{

/// Scans forward for the next verifiable anchor frame. Returns its frame
/// start offset, or nullopt. CRC verification makes false hits ~2^-32.
std::optional<size_t> findNextAnchorFrame(const std::string& t_data, size_t t_from) {
    const size_t n = t_data.size();
    for (size_t p = t_from; p + 14 <= n; ++p) {
        uint16_t db = 0;
        std::memcpy(&db, t_data.data() + p + 8, sizeof(db));
        if (db != database::kAnchorFrameDbNum)
            continue;
        uint32_t len = 0;
        std::memcpy(&len, t_data.data() + p + 10, sizeof(len));
        if (len < 6 || p + 14 + len > n)
            continue;
        const uint8_t* pl = reinterpret_cast<const uint8_t*>(t_data.data() + p + 14);
        uint32_t crc = 0;
        std::memcpy(&crc, pl + 2, sizeof(crc));
        if (database::binaryWalCrc32(pl + 6, len - 6) == crc)
            return p;
    }
    return std::nullopt;
}

} // namespace

GatewayReplayer::GatewayReplayer(const ReplayConfig& t_config)
    : config_(t_config) {
}

GatewayReplayer::~GatewayReplayer() {
    stop();
}

Result<void, std::string> GatewayReplayer::initialize() {
    gateway_app_ = std::make_unique<GatewayApplication>();

    std::vector<std::string> args_vec = {"sgrn_replay", config_.gateway_config_path};
    if (!config_.scl_schema_path.empty()) {
        args_vec.push_back("--schema");
        args_vec.push_back(config_.scl_schema_path);
    }

    std::vector<char*> argv;
    for (auto& arg : args_vec) {
        argv.push_back(arg.data());
    }

    SGRN_RETURN_IF(auto r = gateway_app_->loadConfig(static_cast<int>(argv.size()), argv.data());
        r.hasError(), fmt::format(fg(fmt::color::red), "[sgrn_replay] Failed to load gateway config: {}\n", r.error()));

    // Passive mode from the start: initInfrastructure() must already know that
    // persistence and the cloud uploader stay off, and no southbound traffic
    // may happen — the twin is driven from the archive via writeDbMemory().
    gateway_app_->enablePassiveReplayMode();

    SGRN_RETURN_IF(auto r = gateway_app_->loadSchema();
        r.hasError(), fmt::format(fg(fmt::color::red), "[sgrn_replay] Failed to load schema: {}\n", r.error()));

    SGRN_RETURN_IF(auto r = gateway_app_->initSecurity();
        r.hasError(), fmt::format(fg(fmt::color::red), "[sgrn_replay] Security init failed: {}\n", r.error()));

    SGRN_RETURN_IF(auto r = gateway_app_->initTwin();
        r.hasError(), fmt::format(fg(fmt::color::red), "[sgrn_replay] Twin memory init failed: {}\n", r.error()));

    plc_memory_ = &gateway_app_->memory();
    schema_store_ = &gateway_app_->schema();

    SGRN_RETURN_IF(auto r = gateway_app_->initThreading();
        r.hasError(), fmt::format(fg(fmt::color::red), "[sgrn_replay] Threading init failed: {}\n", r.error()));

    SGRN_RETURN_IF(auto r = gateway_app_->wireTelemetry();
        r.hasError(), fmt::format(fg(fmt::color::red), "[sgrn_replay] Telemetry wiring failed: {}\n", r.error()));

    SGRN_RETURN_IF(auto r = gateway_app_->initInfrastructure();
        r.hasError(), fmt::format(fg(fmt::color::red), "[sgrn_replay] Infrastructure init failed: {}\n", r.error()));
    SGRN_RETURN_IF(auto r = gateway_app_->startAdapters();
        r.hasError(), fmt::format(fg(fmt::color::red), "[sgrn_replay] Adapter start failed: {}\n", r.error()));

    return {};
}

void GatewayReplayer::start() {
    if (running_.exchange(true))
        return;
    replay_thread_ = std::thread(&GatewayReplayer::replayLoop, this);
}

uint16_t GatewayReplayer::httpPort() const {
    return gateway_app_ ? gateway_app_->httpPort() : 0;
}

void GatewayReplayer::stop() {
    if (!running_.exchange(false))
        return;
    if (replay_thread_.joinable()) {
        replay_thread_.join();
    }
    if (gateway_app_) {
        gateway_app_->shutdown();
    }
}

bool GatewayReplayer::encodeLeaf(const std::string& t_full_path, const rapidjson::Value& t_val, std::deque<std::vector<uint8_t>>& t_storage,
    std::vector<twin::DbMemorySpan>& t_spans) {
    if (plc_memory_ == nullptr || plc_memory_->state() == nullptr || schema_store_ == nullptr)
        return false;

    const size_t dot = t_full_path.find('.');
    if (dot == std::string::npos)
        return false;
    const std::string db_name = t_full_path.substr(0, dot);
    const std::string field_path = t_full_path.substr(dot + 1);
    if (db_name.empty() || field_path.empty())
        return false;

    const auto* p_seg = plc_memory_->state()->findSegmentByName(db_name);
    if (p_seg == nullptr) {
        fmt::print(
            fg(fmt::color::yellow), "[sgrn_replay] Unknown DB '{}' for '{}' (schema drift?), skipping leaf.\n", db_name, t_full_path);
        return false;
    }
    const uint16_t db_num = static_cast<uint16_t>(p_seg->id);

    auto loc = schema_store_->findField(db_num, field_path);
    if (!loc.has_value() || loc->field == nullptr) {
        fmt::print(fg(fmt::color::yellow), "[sgrn_replay] Unknown field '{}' (schema drift?), skipping leaf.\n", t_full_path);
        return false;
    }
    const ::sgrn::scl::DbField& field = *loc->field;

    // Single-bit bools share packed bytes with neighbouring bools: a whole-
    // byte batch write would clear them. Stage for writeBit() instead — the
    // same split DbIOProvider::commitOneField uses for live writes.
    if (field.type == ::sgrn::scl::DataType::Bool && field.count <= 1) {
        bool bit = false;
        if (t_val.IsBool())
            bit = t_val.GetBool();
        else if (t_val.IsInt())
            bit = t_val.GetInt() != 0;
        else if (t_val.IsUint())
            bit = t_val.GetUint() != 0;
        else if (t_val.IsString()) {
            const std::string s = t_val.GetString();
            bit = (s == "true" || s == "1" || s == "TRUE");
        } else if (t_val.IsNull()) {
            bit = false;
        } else {
            fmt::print(fg(fmt::color::yellow), "[sgrn_replay] Unencodable bool '{}', skipping leaf.\n", t_full_path);
            return false;
        }
        pending_bits_.push_back(PendingBit{db_num, static_cast<size_t>(loc->abs_offset), field.bit_index, bit});
        return true;
    }

    const int field_size = twin::fieldSpanSize(field);
    if (field_size <= 0) {
        fmt::print(fg(fmt::color::yellow), "[sgrn_replay] Zero-size field '{}', skipping leaf.\n", t_full_path);
        return false;
    }
    const size_t size = static_cast<size_t>(field_size);

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    t_val.Accept(writer);

    // deque keeps existing element addresses stable across pushes, so spans
    // built earlier stay valid while later leaves append their buffers.
    t_storage.emplace_back(size, 0);
    auto res = twin::encodeFieldAt(field, sb.GetString(), t_storage.back().data(), size, /*bit_offset=*/0, field.endianness);
    if (res.hasError()) {
        t_storage.pop_back();
        fmt::print(fg(fmt::color::yellow), "[sgrn_replay] Encode failed for '{}': {}\n", t_full_path, ::sgrn::scl::toString(res.error()));
        return false;
    }
    t_spans.push_back(twin::DbMemorySpan{db_num, static_cast<size_t>(loc->abs_offset), size, t_storage.back().data()});
    return true;
}

bool GatewayReplayer::flushPendingBits() {
    bool ok = true;
    for (const auto& bit : pending_bits_) {
        if (plc_memory_ == nullptr) {
            ok = false;
            break;
        }
        if (auto r = plc_memory_->writeBit(bit.db, bit.byte_offset, bit.bit_index, bit.value); !r) {
            fmt::print(fg(fmt::color::yellow), "[sgrn_replay] writeBit failed for DB{}+{}#{}: {}\n", bit.db, bit.byte_offset, bit.bit_index,
                twin::toString(r.error()));
            ok = false;
        }
    }
    pending_bits_.clear();
    return ok;
}

void GatewayReplayer::replayLoop() {
    fmt::print(fg(fmt::color::cyan), "[sgrn_replay] Starting replay of history archive: {}\n", config_.archive_path);
    fmt::print(
        fg(fmt::color::cyan), "[sgrn_replay] Speed multiplier: {}x, Loop mode: {}\n", config_.replay_speed, config_.loop ? "ON" : "OFF");

    do {
        if (!processBinaryArchive(config_.archive_path)) {
            fmt::print(fg(fmt::color::yellow), "[sgrn_replay] Archive replay processing encountered error or reached EOF.\n");
            if (!config_.loop)
                break;
        }
    } while (running_.load() && config_.loop);

    fmt::print(fg(fmt::color::green), "[sgrn_replay] History archive replay finished.\n");
}

bool GatewayReplayer::processBinaryArchive(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        fmt::print(fg(fmt::color::red), "[sgrn_replay] Cannot open archive file: {}\n", path);
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    std::string decompressed;
    if (path.length() >= 4 && path.substr(path.length() - 4) == ".zst") {
        auto dec_res = sgrn::utils::compression::decompressStringZstd(content);
        if (dec_res.hasError()) {
            fmt::print(fg(fmt::color::red), "[sgrn_replay] Failed to decompress zstd archive: {}\n", dec_res.error());
            return false;
        }
        decompressed = std::move(dec_res).value();
    } else {
        decompressed = std::move(content);
    }

    const bool is_binary =
        (decompressed.size() >= 4 && decompressed[0] == 'S' && decompressed[1] == 'G' && decompressed[2] == 'R' && decompressed[3] == 'N');

    int64_t last_ts = -1;
    uint64_t replayed_frames = 0;
    std::unordered_map<uint16_t, uint64_t> skipped_frames;
    std::unordered_map<uint16_t, uint64_t> truncated_frames;
    if (is_binary) {
        database::BinaryWalHeader header;
        if (database::checkBinaryWalHeader(decompressed, header) != database::BinaryHeaderStatus::kOk) {
            uint16_t ver = 0;
            if (decompressed.size() >= 6)
                std::memcpy(&ver, decompressed.data() + 4, sizeof(ver));
            fmt::print(fg(fmt::color::red), "[sgrn_replay] Unsupported binary version {} in {}\n", ver, path);
            return false;
        }
        size_t pos = header.frames_start;

        // On stream corruption, jump forward to the next verifiable anchor
        // instead of abandoning the file; stop cleanly when none follows.
        auto resync_or_stop = [&](size_t t_frame_start) {
            auto found = database::findNextAnchorFrame(decompressed, t_frame_start);
            if (!found)
                return false;
            int64_t anchor_ts = 0;
            std::memcpy(&anchor_ts, decompressed.data() + *found, sizeof(anchor_ts));
            fmt::print(fg(fmt::color::yellow), "[sgrn_replay] Corruption at byte {} in {}, resumed at anchor ts={}\n", t_frame_start, path,
                anchor_ts);
            pos = *found;
            return true;
        };

        database::BinaryFrame fr;
        while (running_.load()) {
            const auto status = database::decodeBinaryFrame(decompressed, pos, fr);
            if (status == database::BinaryFrameStatus::kEnd)
                break;
            const int64_t ts = fr.ts;
            uint16_t db_num = fr.db;
            const uint32_t payload_len = fr.payload_len;
            const size_t frame_start = fr.header_start;
            const uint8_t* payload = fr.payload;

            if (status == database::BinaryFrameStatus::kTruncated) {
                if (resync_or_stop(frame_start))
                    continue;
                break;
            }

            // Control frames (dictionary / manifest / anchor / footer) carry
            // JSON, not raw DB memory — skip them before the memory write.
            if (db_num == database::kControlFrameDbNum) {
                continue;
            }

            // Handle real-time delay pacing
            if (!config_.no_delay && last_ts > 0 && ts > last_ts && config_.replay_speed > 0) {
                int64_t delay_ms = static_cast<int64_t>((ts - last_ts) / config_.replay_speed);
                if (delay_ms > 0 && delay_ms < 60000) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                }
            }
            last_ts = ts;

            // Anchor frames (v3+): db + crc32 + full image. Verify before
            // trusting a single byte; a mismatch seeks resync, never adopts.
            // Verified anchors fall through to the unified full-image write
            // below (same segment-size policy and counting as data frames).
            const uint8_t* image_ptr = payload;
            size_t image_len = payload_len;
            if (db_num == database::kAnchorFrameDbNum) {
                uint16_t anchor_db = 0;
                const uint8_t* anchor_image = nullptr;
                size_t anchor_len = 0;
                if (!database::verifyAnchorFrame(payload, payload_len, anchor_db, anchor_image, anchor_len)) {
                    fmt::print(fg(fmt::color::yellow), "[sgrn_replay] Anchor CRC mismatch, seeking resync\n");
                    if (resync_or_stop(frame_start))
                        continue;
                    break;
                }
                db_num = anchor_db;
                image_ptr = anchor_image;
                image_len = anchor_len;
            }

            // Delta frames (v2+): db + (offset,len,bytes) runs against the
            // live image. One atomic batch write, runs validated first.
            // Unknown DBs skip (exact stream position); corrupt runs resync.
            if (db_num == database::kDeltaFrameDbNum) {
                if (plc_memory_ != nullptr && plc_memory_->state() != nullptr && payload_len >= 2) {
                    uint16_t delta_db = 0;
                    std::memcpy(&delta_db, payload, sizeof(delta_db));
                    const auto* p_seg = plc_memory_->state()->findSegmentById(delta_db);
                    if (p_seg == nullptr) {
                        fmt::print(fg(fmt::color::yellow), "[sgrn_replay] Skipping delta for unknown DB{} (schema drift?)\n", delta_db);
                        ++skipped_frames[delta_db];
                        continue;
                    }
                    std::vector<database::BinaryDeltaRun> runs;
                    if (!database::parseDeltaRuns(payload, payload_len, p_seg->size, delta_db, runs)) {
                        fmt::print(fg(fmt::color::yellow), "[sgrn_replay] Corrupt delta frame, seeking resync\n");
                        if (resync_or_stop(frame_start))
                            continue;
                        break;
                    }
                    std::vector<twin::DbMemorySpan> spans;
                    spans.reserve(runs.size());
                    for (const auto& run : runs)
                        spans.push_back(twin::DbMemorySpan{delta_db, static_cast<size_t>(run.offset), static_cast<size_t>(run.len),
                            const_cast<uint8_t*>(payload + run.data_pos)});
                    const std::span<const twin::DbMemorySpan> batch(spans);
                    if (auto r = plc_memory_->writeDbMemory(batch); !r) {
                        fmt::print(
                            fg(fmt::color::yellow), "[sgrn_replay] Delta write failed for DB{}: {}\n", delta_db, twin::toString(r.error()));
                    } else {
                        ++replayed_frames;
                    }
                }
                continue;
            }

            // Write the frame straight into the twin. writeDbMemory() diffs,
            // bumps versions, marks dirty and signals — the dirty handler wired
            // up in wireTelemetry() turns this into LeafUpdate/DeltaSnapshot
            // events for northbound consumers. Full images land at offset 0;
            // anything else is schema drift: a larger image is truncated to
            // the live segment (existing prefix fields stay exact), a smaller
            // one cannot be placed and is skipped — both are logged.
            if (plc_memory_ != nullptr && plc_memory_->state() != nullptr) {
                const auto* p_seg = plc_memory_->state()->findSegmentById(db_num);
                if (p_seg == nullptr) {
                    fmt::print(fg(fmt::color::yellow), "[sgrn_replay] Skipping frame for unknown DB{} (schema drift?)\n", db_num);
                    ++skipped_frames[db_num];
                } else if (image_len > p_seg->size) {
                    fmt::print(fg(fmt::color::yellow), "[sgrn_replay] Truncating DB{} frame: {} bytes > live {} bytes\n", db_num, image_len,
                        p_seg->size);
                    (void)plc_memory_->writeDbMemory(db_num, /*offset=*/0, p_seg->size, reinterpret_cast<const uint8_t*>(image_ptr));
                    ++truncated_frames[db_num];
                    ++replayed_frames;
                } else if (image_len < p_seg->size) {
                    fmt::print(fg(fmt::color::yellow), "[sgrn_replay] Skipping short DB{} frame: {} bytes < live {} bytes\n", db_num,
                        image_len, p_seg->size);
                    ++skipped_frames[db_num];
                } else {
                    (void)plc_memory_->writeDbMemory(db_num, /*offset=*/0, image_len, reinterpret_cast<const uint8_t*>(image_ptr));
                    ++replayed_frames;
                }
            }
        }
    } else {
        // Line-based JSONL archive: anchor/delta WAL lines. Archived leaves
        // resolve via the file-local LeafDictionary ("dictionary" line) to
        // schema fields, encode to raw bytes, and commit as ONE batched
        // writeDbMemory — the same granularity live adapters use, matching
        // the atomicity of the merge window the data was captured with.
        std::vector<std::string> path_by_id;

        std::istringstream stream(decompressed);
        std::string line;
        while (std::getline(stream, line) && running_.load()) {
            if (line.empty())
                continue;
            rapidjson::Document doc;
            if (doc.Parse(line.c_str()).HasParseError())
                continue;

            if (doc.HasMember("type") && doc["type"].IsString() && std::string_view(doc["type"].GetString()) == "dictionary") {
                if (doc.HasMember("leaves") && doc["leaves"].IsArray()) {
                    for (const auto& item : doc["leaves"].GetArray()) {
                        if (item.IsObject() && item.HasMember("id") && item["id"].IsUint() && item.HasMember("path") &&
                            item["path"].IsString()) {
                            const auto id = item["id"].GetUint();
                            if (id >= path_by_id.size())
                                path_by_id.resize(static_cast<size_t>(id) + 1);
                            path_by_id[id] = item["path"].GetString();
                        }
                    }
                }
                continue;
            }

            if (!doc.HasMember("type") || !doc["type"].IsString())
                continue;
            const std::string_view line_type = doc["type"].GetString();
            if (line_type != "anchor" && line_type != "delta")
                continue;

            const int64_t ts = (doc.HasMember("ts") && doc["ts"].IsInt64()) ? doc["ts"].GetInt64() : sgrn::utils::time::nowMilliseconds();

            if (!config_.no_delay && last_ts > 0 && ts > last_ts && config_.replay_speed > 0) {
                int64_t delay_ms = static_cast<int64_t>((ts - last_ts) / config_.replay_speed);
                if (delay_ms > 0 && delay_ms < 60000) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                }
            }
            last_ts = ts;

            const rapidjson::Value* payload = nullptr;
            if (doc.HasMember("changes") && doc["changes"].IsObject())
                payload = &doc["changes"];
            else if (doc.HasMember("data") && doc["data"].IsObject())
                payload = &doc["data"];
            if (payload == nullptr)
                continue;

            // deque keeps span backing buffers stable while later leaves
            // append theirs (see encodeLeaf).
            std::deque<std::vector<uint8_t>> storage;
            std::vector<twin::DbMemorySpan> spans;
            for (auto it = payload->MemberBegin(); it != payload->MemberEnd(); ++it) {
                std::string key = it->name.GetString();
                if (!path_by_id.empty() && std::all_of(key.begin(), key.end(), [](unsigned char c) { return std::isdigit(c) != 0; })) {
                    const size_t id = std::stoul(key);
                    if (id >= path_by_id.size() || path_by_id[id].empty())
                        continue;
                    key = path_by_id[id];
                }
                if (it->value.IsObject()) {
                    fmt::print(fg(fmt::color::cyan), "[sgrn_replay] Skipping nested member '{}' (legacy anchor shape).\n", key);
                    continue;
                }
                (void)encodeLeaf(key, it->value, storage, spans);
            }

            if (spans.empty() && pending_bits_.empty())
                continue;

            bool line_ok = true;
            if (!spans.empty() && plc_memory_ != nullptr) {
                const std::span<const twin::DbMemorySpan> batch(spans);
                if (auto r = plc_memory_->writeDbMemory(batch); !r) {
                    fmt::print(fg(fmt::color::yellow), "[sgrn_replay] Batched write failed: {}\n", twin::toString(r.error()));
                    line_ok = false;
                }
            }
            if (!flushPendingBits())
                line_ok = false;
            if (line_ok)
                ++replayed_frames;
        }
    }

    fmt::print(fg(fmt::color::green), "[sgrn_replay] Replayed {} frames from archive.\n", replayed_frames);
    for (const auto& [db, count] : skipped_frames) {
        const uint64_t truncated = truncated_frames.count(db) ? truncated_frames.at(db) : 0;
        fmt::print(fg(fmt::color::yellow), "[sgrn_replay] DB{}: {} frames skipped, {} truncated (schema drift?)\n", db, count, truncated);
    }
    for (const auto& [db, count] : truncated_frames) {
        if (skipped_frames.count(db) != 0)
            continue;
        fmt::print(fg(fmt::color::yellow), "[sgrn_replay] DB{}: 0 frames skipped, {} truncated (schema drift?)\n", db, count);
    }
    return true;
}

} // namespace sgrn::gateway
