// =============================================================================
// WalReplayer.cpp — Rate-controlled replay of binary/JSONL WAL archives
// =============================================================================
//
// Replays ONE specific archive file (archive_path_) frame-by-frame into the
// bound PlcRuntime, honouring per-frame timestamps with real-time pacing
// scaled by speed_factor_. This mirrors GatewayReplayer::processBinaryArchive()
// in sgrn/gateway/src/tools/sgrn_replay.cpp: same shared database:: decode
// helpers (decodeBinaryFrame, verifyAnchorFrame, parseDeltaRuns,
// findNextAnchorFrame), same pacing rule (skip delay if delta <= 0 or
// >= 60s), same write path (PlcMemory::writeDbMemory / writeBit).
//
// Deliberately NOT based on recoverStateFromArchives(): that routine is a
// one-shot "jump to final state" helper for boot-time crash recovery
// (GatewayApplication/PersistenceService) — it rescans a whole state
// directory, picks the newest archive and applies everything in a tight
// loop with no pacing. The s7shell `replay` subcommand needs the opposite:
// the exact file the user passed, replayed in real time.

#include <sgrn/s7shell/replay/WalReplayer.hpp>

#include <sgrn/gateway/database/PersistenceService.hpp> // shared database:: WAL decode helpers
#include <sgrn/gateway/twin/DbMemorySpan.hpp>
#include <sgrn/gateway/twin/encoding.hpp> // twin::encodeFieldAt (JSONL path)
#include <sgrn/gateway/twin/path.hpp>     // twin::fieldSpanSize (JSONL path)
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/time.hpp>

#include <fmt/color.h>
#include <fmt/format.h>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cctype>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sgrn::s7shell::replay
{

namespace fs = std::filesystem;
namespace database = ::sgrn::gateway::database;
namespace twin = ::sgrn::gateway::twin;

namespace
{

/// Delays larger than this are treated as capture gaps, not replayable
/// silence (same 60 s cap as GatewayReplayer::processBinaryArchive()).
inline constexpr int64_t kMaxPaceDelayMs = 60000;

/// Sleeps for (t_ts - t_last_ts) / t_speed, updating t_last_ts to t_ts.
/// Skips the delay when there is no previous timestamp, the delta is not
/// positive, the speed factor is not positive, or the delay reaches the
/// 60 s capture-gap cap.
void paceFrame(int64_t& t_last_ts, int64_t t_ts, double t_speed) {
    if (t_last_ts > 0 && t_ts > t_last_ts && t_speed > 0) {
        const int64_t delay_ms = static_cast<int64_t>((t_ts - t_last_ts) / t_speed);
        if (delay_ms > 0 && delay_ms < kMaxPaceDelayMs) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
    }
    t_last_ts = t_ts;
}

/// Single-bit bools staged for writeBit() (same split as DbIOProvider and
/// GatewayReplayer::encodeLeaf: co-packed neighbours must survive).
struct PendingBit {
    uint16_t db{0};
    size_t byte_offset{0};
    int bit_index{0};
    bool value{false};
};

/// Encodes one archived JSONL leaf (dotted "DbName.field.path" + JSON
/// scalar) into raw twin bytes, mirroring GatewayReplayer::encodeLeaf().
/// Appends the span to t_spans (backed by t_storage, which must outlive the
/// spans); single-bit bools are staged into t_pending for writeBit().
/// Returns false on schema drift or encode failure (caller logs + skips).
bool encodeLeaf(runtime::PlcRuntime& t_runtime, const std::string& t_full_path, const rapidjson::Value& t_val,
    std::deque<std::vector<uint8_t>>& t_storage, std::vector<twin::DbMemorySpan>& t_spans, std::vector<PendingBit>& t_pending) {
    auto* p_state = &t_runtime.getState();
    const auto& schema = t_runtime.getSchema();

    const size_t dot = t_full_path.find('.');
    if (dot == std::string::npos)
        return false;
    const std::string db_name = t_full_path.substr(0, dot);
    const std::string field_path = t_full_path.substr(dot + 1);
    if (db_name.empty() || field_path.empty())
        return false;

    const auto* p_seg = p_state->findSegmentByName(db_name);
    if (p_seg == nullptr) {
        fmt::print(
            fg(fmt::color::yellow), "[WalReplayer] Unknown DB '{}' for '{}' (schema drift?), skipping leaf.\n", db_name, t_full_path);
        return false;
    }
    const uint16_t db_num = static_cast<uint16_t>(p_seg->id);

    auto loc = schema.findField(db_num, field_path);
    if (!loc.has_value() || loc->field == nullptr) {
        fmt::print(fg(fmt::color::yellow), "[WalReplayer] Unknown field '{}' (schema drift?), skipping leaf.\n", t_full_path);
        return false;
    }
    const ::sgrn::scl::DbField& field = *loc->field;

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
            fmt::print(fg(fmt::color::yellow), "[WalReplayer] Unencodable bool '{}', skipping leaf.\n", t_full_path);
            return false;
        }
        t_pending.push_back(PendingBit{db_num, static_cast<size_t>(loc->abs_offset), field.bit_index, bit});
        return true;
    }

    const int field_size = twin::fieldSpanSize(field);
    if (field_size <= 0) {
        fmt::print(fg(fmt::color::yellow), "[WalReplayer] Zero-size field '{}', skipping leaf.\n", t_full_path);
        return false;
    }
    const size_t size = static_cast<size_t>(field_size);

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> writer(sb);
    t_val.Accept(writer);

    // deque keeps existing element addresses stable across pushes, so spans
    // built earlier stay valid while later leaves append their buffers.
    t_storage.emplace_back(size, 0);
    auto res = twin::encodeFieldAt(field, sb.GetString(), t_storage.back().data(), size, /*t_depth=*/0, field.endianness);
    if (res.hasError()) {
        t_storage.pop_back();
        fmt::print(fg(fmt::color::yellow), "[WalReplayer] Encode failed for '{}': {}\n", t_full_path, ::sgrn::scl::toString(res.error()));
        return false;
    }
    t_spans.push_back(twin::DbMemorySpan{db_num, static_cast<size_t>(loc->abs_offset), size, t_storage.back().data()});
    return true;
}

/// Commits bits staged by encodeLeaf() via PlcMemory::writeBit().
/// Clears the staging list. Returns false if any write failed.
bool flushPendingBits(runtime::PlcRuntime& t_runtime, std::vector<PendingBit>& t_pending) {
    bool ok = true;
    for (const auto& bit : t_pending) {
        if (auto r = t_runtime.getMemory().writeBit(bit.db, bit.byte_offset, bit.bit_index, bit.value); !r) {
            fmt::print(fg(fmt::color::yellow), "[WalReplayer] writeBit failed for DB{}+{}#{}: {}\n", bit.db, bit.byte_offset, bit.bit_index,
                twin::toString(r.error()));
            ok = false;
        }
    }
    t_pending.clear();
    return ok;
}

} // namespace

WalReplayer::WalReplayer(std::string archive_path, std::shared_ptr<runtime::PlcRuntime> runtime)
    : archive_path_(std::move(archive_path))
    , runtime_(std::move(runtime)) {
    if (!runtime_) {
        runtime_ = std::make_shared<runtime::PlcRuntime>();
    }
}

bool WalReplayer::replayBinaryArchive(const std::string& t_decompressed, uint64_t& t_replayed_frames) {
    auto& state = runtime_->getState();
    auto& memory = runtime_->getMemory();

    database::BinaryWalHeader header;
    if (database::checkBinaryWalHeader(t_decompressed, header) != database::BinaryHeaderStatus::kOk) {
        uint16_t ver = 0;
        if (t_decompressed.size() >= 6)
            std::memcpy(&ver, t_decompressed.data() + 4, sizeof(ver));
        fmt::print(stderr, fg(fmt::color::red), "[WalReplayer] Failed to restore state: unsupported binary version {} in {}\n", ver,
            archive_path_);
        return false;
    }
    size_t pos = header.frames_start;

    // On stream corruption, jump forward to the next verifiable anchor
    // instead of abandoning the file; stop cleanly when none follows.
    auto resync_or_stop = [&](size_t t_frame_start) {
        auto found = database::findNextAnchorFrame(t_decompressed, t_frame_start);
        if (!found)
            return false;
        int64_t anchor_ts = 0;
        std::memcpy(&anchor_ts, t_decompressed.data() + *found, sizeof(anchor_ts));
        fmt::print(fg(fmt::color::yellow), "[WalReplayer] Corruption at byte {} in {}, resumed at anchor ts={}\n", t_frame_start,
            archive_path_, anchor_ts);
        pos = *found;
        return true;
    };

    int64_t last_ts = -1;
    std::unordered_map<uint16_t, uint64_t> skipped_frames;
    std::unordered_map<uint16_t, uint64_t> truncated_frames;

    database::BinaryFrame fr;
    while (true) {
        const auto status = database::decodeBinaryFrame(t_decompressed, pos, fr);
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

        // Handle real-time delay pacing (same rule as sgrn_replay).
        paceFrame(last_ts, ts, speed_factor_);

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
                fmt::print(fg(fmt::color::yellow), "[WalReplayer] Anchor CRC mismatch, seeking resync\n");
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
            if (payload_len >= 2) {
                uint16_t delta_db = 0;
                std::memcpy(&delta_db, payload, sizeof(delta_db));
                const auto* p_seg = state.findSegmentById(delta_db);
                if (p_seg == nullptr) {
                    fmt::print(fg(fmt::color::yellow), "[WalReplayer] Skipping delta for unknown DB{} (schema drift?)\n", delta_db);
                    ++skipped_frames[delta_db];
                    continue;
                }
                std::vector<database::BinaryDeltaRun> runs;
                if (!database::parseDeltaRuns(payload, payload_len, p_seg->size, delta_db, runs)) {
                    fmt::print(fg(fmt::color::yellow), "[WalReplayer] Corrupt delta frame, seeking resync\n");
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
                if (auto r = memory.writeDbMemory(batch); !r) {
                    fmt::print(
                        fg(fmt::color::yellow), "[WalReplayer] Delta write failed for DB{}: {}\n", delta_db, twin::toString(r.error()));
                } else {
                    ++t_replayed_frames;
                    if (callback_)
                        callback_(delta_db, static_cast<uint64_t>(ts));
                }
            }
            continue;
        }

        // Full-image frames land at offset 0; anything else is schema
        // drift: a larger image is truncated to the live segment (existing
        // prefix fields stay exact), a smaller one cannot be placed and is
        // skipped — both are logged.
        const auto* p_seg = state.findSegmentById(db_num);
        if (p_seg == nullptr) {
            fmt::print(fg(fmt::color::yellow), "[WalReplayer] Skipping frame for unknown DB{} (schema drift?)\n", db_num);
            ++skipped_frames[db_num];
        } else if (image_len > p_seg->size) {
            fmt::print(
                fg(fmt::color::yellow), "[WalReplayer] Truncating DB{} frame: {} bytes > live {} bytes\n", db_num, image_len, p_seg->size);
            (void)memory.writeDbMemory(db_num, /*t_offset=*/0, p_seg->size, reinterpret_cast<const uint8_t*>(image_ptr));
            ++truncated_frames[db_num];
            ++t_replayed_frames;
            if (callback_)
                callback_(db_num, static_cast<uint64_t>(ts));
        } else if (image_len < p_seg->size) {
            fmt::print(fg(fmt::color::yellow), "[WalReplayer] Skipping short DB{} frame: {} bytes < live {} bytes\n", db_num, image_len,
                p_seg->size);
            ++skipped_frames[db_num];
        } else {
            (void)memory.writeDbMemory(db_num, /*t_offset=*/0, image_len, reinterpret_cast<const uint8_t*>(image_ptr));
            ++t_replayed_frames;
            if (callback_)
                callback_(db_num, static_cast<uint64_t>(ts));
        }
    }

    for (const auto& [db, count] : skipped_frames) {
        const uint64_t truncated = truncated_frames.count(db) ? truncated_frames.at(db) : 0;
        fmt::print(fg(fmt::color::yellow), "[WalReplayer] DB{}: {} frames skipped, {} truncated (schema drift?)\n", db, count, truncated);
    }
    for (const auto& [db, count] : truncated_frames) {
        if (skipped_frames.count(db) != 0)
            continue;
        fmt::print(fg(fmt::color::yellow), "[WalReplayer] DB{}: 0 frames skipped, {} truncated (schema drift?)\n", db, count);
    }
    return true;
}

bool WalReplayer::replayJsonlArchive(const std::string& t_decompressed, uint64_t& t_replayed_frames) {
    // Line-based JSONL archive: anchor/delta WAL lines. Archived leaves
    // resolve via the file-local LeafDictionary ("dictionary" line) to
    // schema fields, encode to raw bytes, and commit as ONE batched
    // writeDbMemory — the same granularity live adapters use, matching
    // the atomicity of the merge window the data was captured with.
    std::vector<std::string> path_by_id;
    std::vector<PendingBit> pending_bits;

    int64_t last_ts = -1;

    std::istringstream stream(t_decompressed);
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty())
            continue;
        rapidjson::Document doc;
        if (doc.Parse(line.c_str()).HasParseError() || !doc.IsObject())
            continue;

        if (database::isDictionaryRecord(doc)) {
            database::parseDictionaryLine(doc, path_by_id);
            continue;
        }

        if (!doc.HasMember("type") || !doc["type"].IsString())
            continue;
        const std::string_view line_type = doc["type"].GetString();
        if (line_type != "anchor" && line_type != "delta")
            continue;

        const int64_t ts = (doc.HasMember("ts") && doc["ts"].IsInt64()) ? doc["ts"].GetInt64() : ::sgrn::utils::time::nowMilliseconds();

        paceFrame(last_ts, ts, speed_factor_);

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
                fmt::print(fg(fmt::color::cyan), "[WalReplayer] Skipping nested member '{}' (legacy anchor shape).\n", key);
                continue;
            }
            (void)encodeLeaf(*runtime_, key, it->value, storage, spans, pending_bits);
        }

        if (spans.empty() && pending_bits.empty())
            continue;

        bool line_ok = true;
        if (!spans.empty()) {
            const std::span<const twin::DbMemorySpan> batch(spans);
            if (auto r = runtime_->getMemory().writeDbMemory(batch); !r) {
                fmt::print(fg(fmt::color::yellow), "[WalReplayer] Batched write failed: {}\n", twin::toString(r.error()));
                line_ok = false;
            }
        }
        if (!flushPendingBits(*runtime_, pending_bits))
            line_ok = false;
        if (line_ok) {
            ++t_replayed_frames;
            if (callback_ && !spans.empty())
                callback_(spans.front().db, static_cast<uint64_t>(ts));
        }
    }
    return true;
}

bool WalReplayer::run() {
    fmt::print(fg(fmt::color::cyan), "[WalReplayer] Replaying archive {} at {:.1f}x speed...\n", archive_path_, speed_factor_);

    // Load exactly the file the user passed — never a directory scan.
    std::ifstream file(archive_path_, std::ios::binary);
    if (!file.is_open()) {
        fmt::print(stderr, fg(fmt::color::red), "[WalReplayer] Failed to restore state: cannot open archive file: {}\n", archive_path_);
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    std::string decompressed;
    if (archive_path_.length() >= 4 && (archive_path_.ends_with(".zst") || archive_path_.ends_with(".zst.tmp"))) {
        auto dec_res = ::sgrn::utils::compression::decompressStringZstd(content);
        if (dec_res.hasError()) {
            fmt::print(stderr, fg(fmt::color::red), "[WalReplayer] Failed to restore state: failed to decompress zstd archive: {}\n",
                dec_res.error());
            return false;
        }
        decompressed = std::move(dec_res).value();
    } else {
        decompressed = std::move(content);
    }

    const bool is_binary =
        (decompressed.size() >= 4 && decompressed[0] == 'S' && decompressed[1] == 'G' && decompressed[2] == 'R' && decompressed[3] == 'N');

    uint64_t replayed_frames = 0;
    const bool ok = is_binary ? replayBinaryArchive(decompressed, replayed_frames) : replayJsonlArchive(decompressed, replayed_frames);
    if (!ok) {
        return false;
    }

    fmt::print(fg(fmt::color::green), "[WalReplayer] Replayed {} frames from archive.\n", replayed_frames);
    fmt::print(fg(fmt::color::green), "[WalReplayer] Replay complete. Archive used: {}\n", archive_path_);
    return true;
}

} // namespace sgrn::s7shell::replay
