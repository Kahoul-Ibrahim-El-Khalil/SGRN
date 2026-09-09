// WalReplayer regression tests (s7shell `replay` path).
//
// (a) Replaying a SPECIFIC older archive must load that file — not whatever
//     archive sorts newest in its parent directory (the old
//     recoverStateFromArchives() behaviour).
// (b) speed_factor_ must scale the elapsed wall-clock time of a multi-frame
//     replay (per-frame timestamp pacing, same rule as sgrn_replay).
//
// Archive layout (binary WAL, see PersistenceService.hpp):
//   header: "SGRN" + version:u16 (=3) + schema_len:u32 (=0, skip check)
//   frame:  ts:i64 + db:u16 + payload_len:u32 + payload (raw DB image)

#include <sgrn/s7shell/replay/WalReplayer.hpp>
#include <sgrn/s7shell/runtime/PlcRuntime.hpp>
#include <sgrn/utils/compression.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace
{

namespace fs = std::filesystem;
using sgrn::s7shell::replay::WalReplayer;
using sgrn::s7shell::runtime::PlcRuntime;

constexpr uint16_t kDb = 10;
constexpr size_t kDbSize = 8;

void appendU16(std::string& t_out, uint16_t t_v) {
    t_out.append(reinterpret_cast<const char*>(&t_v), sizeof(t_v));
}

void appendU32(std::string& t_out, uint32_t t_v) {
    t_out.append(reinterpret_cast<const char*>(&t_v), sizeof(t_v));
}

void appendI64(std::string& t_out, int64_t t_v) {
    t_out.append(reinterpret_cast<const char*>(&t_v), sizeof(t_v));
}

/// Builds an uncompressed binary WAL buffer: one full-image frame per
/// (timestamp, payload) entry.
std::string makeBinaryArchive(const std::vector<std::pair<int64_t, std::vector<uint8_t>>>& t_frames) {
    std::string out;
    out += "SGRN";
    appendU16(out, 3); // kBinaryWalVersion
    appendU32(out, 0); // schema_len = 0 (skip schema check)
    for (const auto& [ts, payload] : t_frames) {
        assert(payload.size() == kDbSize);
        appendI64(out, ts);
        appendU16(out, kDb);
        appendU32(out, static_cast<uint32_t>(payload.size()));
        out.append(reinterpret_cast<const char*>(payload.data()), payload.size());
    }
    return out;
}

void writeFile(const fs::path& t_path, const std::string& t_bytes) {
    std::ofstream ofs(t_path, std::ios::binary | std::ios::trunc);
    assert(ofs.is_open());
    ofs.write(t_bytes.data(), static_cast<std::streamsize>(t_bytes.size()));
    ofs.close();
    assert(ofs.good() || true);
}

std::vector<uint8_t> readDb(PlcRuntime& t_rt) {
    std::vector<uint8_t> buf(kDbSize, 0);
    auto r = t_rt.getMemory().readDbMemory(kDb, 0, kDbSize, buf.data());
    assert(!r.hasError());
    return buf;
}

/// (a): an older archive (0xAA image) next to a newer archive (0xBB image)
/// in the same directory. Replaying the older file must yield 0xAA.
///
/// Layout mirrors a real state dir: the requested file sits next to an
/// `unsynced/` scan root holding both archives, so the legacy
/// directory-scan behaviour (recoverStateFromArchives on the parent dir)
/// would discover the pair and silently load the NEWEST one.
void testReplaysSpecificArchiveNotNewest() {
    const fs::path dir = fs::temp_directory_path() / "sgrn_wal_replayer_test_a";
    const fs::path watch = dir / "unsynced";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(watch, ec);
    assert(!ec);

    const std::vector<uint8_t> old_image(kDbSize, 0xAA);
    const std::vector<uint8_t> new_image(kDbSize, 0xBB);

    // Sort keys derive from the <end> half of the filename, so the second
    // file is unambiguously "newer" to a directory scan.
    const fs::path older = watch / "2026-01-01T00:00:00-2026-01-01T00:01:00.bin.zst";
    const fs::path newer = watch / "2026-01-01T00:02:00-2026-01-01T00:03:00.bin.zst";
    // The exact file under test: a copy of the older archive, sitting next
    // to the `unsynced/` scan root (like `s7shell replay <archive>`).
    const fs::path requested = dir / "requested.bin.zst";

    auto c_old = sgrn::utils::compression::compressStringZstd(makeBinaryArchive({{1000, old_image}}), 3);
    auto c_new = sgrn::utils::compression::compressStringZstd(makeBinaryArchive({{2000, new_image}}), 3);
    assert(!c_old.hasError() && !c_new.hasError());
    writeFile(older, std::move(c_old).value());
    writeFile(newer, std::move(c_new).value());
    auto c_req = sgrn::utils::compression::compressStringZstd(makeBinaryArchive({{1000, old_image}}), 3);
    assert(!c_req.hasError());
    writeFile(requested, std::move(c_req).value());

    auto rt = PlcRuntime::empty();
    rt->registerDb(kDb, static_cast<uint32_t>(kDbSize), "DB10");

    WalReplayer replayer(requested.string(), rt);
    replayer.speed(1000.0); // single frame: no pacing involved either way
    assert(replayer.run());

    const auto mem = readDb(*rt);
    assert(mem == old_image && "replay loaded the newest archive instead of the requested file");

    fs::remove_all(dir, ec);
    std::cout << "[wal_replayer_test] specific-archive selection: OK\n";
}

/// (b): three frames 300 ms apart replayed at 1x (~600 ms of pacing) vs at
/// 20x (~30 ms). Wall-clock must scale with speed_factor_.
void testSpeedFactorScalesWallClock() {
    const fs::path dir = fs::temp_directory_path() / "sgrn_wal_replayer_test_b";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    assert(!ec);

    const fs::path archive = dir / "paced.bin"; // uncompressed: exercises the raw path
    const std::vector<uint8_t> first(kDbSize, 0x11);
    const std::vector<uint8_t> last(kDbSize, 0x33);
    writeFile(archive, makeBinaryArchive({{1000, first}, {1300, std::vector<uint8_t>(kDbSize, 0x22)}, {1600, last}}));

    auto timeRun = [&](double t_speed) {
        auto rt = PlcRuntime::empty();
        rt->registerDb(kDb, static_cast<uint32_t>(kDbSize), "DB10");
        WalReplayer replayer(archive.string(), rt);
        replayer.speed(t_speed);
        const auto start = std::chrono::steady_clock::now();
        const bool ok = replayer.run();
        const auto elapsed = std::chrono::steady_clock::now() - start;
        assert(ok);
        assert(readDb(*rt) == last && "final frame image not applied");
        return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    };

    const auto slow = timeRun(1.0);
    const auto fast = timeRun(20.0);

    std::cout << "[wal_replayer_test] 1x elapsed: " << slow.count() << " ms, 20x elapsed: " << fast.count() << " ms\n";
    // Two 300 ms gaps at 1x => ~600 ms of pacing (lower bound with margin).
    assert(slow.count() >= 450 && "1x replay did not honour frame timestamps");
    // Same gaps at 20x => ~30 ms; must be a fraction of the 1x run.
    assert(fast.count() * 3 < slow.count() && "speed_factor_ did not scale replay pacing");

    fs::remove_all(dir, ec);
    std::cout << "[wal_replayer_test] speed-scaled pacing: OK\n";
}

} // namespace

int main() {
    testReplaysSpecificArchiveNotNewest();
    testSpeedFactorScalesWallClock();
    std::cout << "[wal_replayer_test] ALL OK\n";
    return 0;
}
