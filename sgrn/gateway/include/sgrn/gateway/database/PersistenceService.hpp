#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/gateway/config/persistence.hpp>
#include <sgrn/gateway/core/TelemetryBroker.hpp>
#include <sgrn/gateway/database/GatewayDatabase.hpp>
#include <sgrn/gateway/twin/LeafDictionary.hpp>
#include <sgrn/utils/compression.hpp>
#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rapidjson/document.h>

namespace sgrn::gateway::database
{

/**
 * @brief Reserved DB number marking a binary-WAL control frame.
 *
 * Binary archives (.bin.zst) are an uninterrupted run of
 * (ts:8)(db_num:2)(payload_len:4)(payload) frames after the 10-byte header.
 * JSON control records (dictionary / manifest / anchor / footer) are wrapped
 * in the same envelope with this sentinel db_num so readers can distinguish
 * them from raw memory-write frames. 0xFFFF can never be a real DB number:
 * live DB numbers are small (declared_db_numbers) and the arena allocator
 * starts synthetic IDs at 1000.
 */
inline constexpr uint16_t kControlFrameDbNum = 0xFFFF;

/**
 * @brief DB number marking a binary-WAL delta frame (version 2+).
 *
 * A delta payload is `db_num:u16` (the REAL target DB) followed by a run
 * list: repeated `(offset:u32, len:u32, bytes...)` against the last image
 * written for that DB (keyframe or previous delta). Self-delimiting within
 * payload_len; runs never overlap. Readers apply runs onto their cached
 * image; a delta for an unseen DB (no keyframe yet) must be skipped.
 */
inline constexpr uint16_t kDeltaFrameDbNum = 0xFFFE;

/**
 * @brief DB number marking a binary-WAL anchor frame (version 3+).
 *
 * An anchor payload is `db_num:u16` (REAL target) + `crc32:u32` + the full
 * DB image. The CRC covers the image bytes and lets readers VERIFY a
 * decode; anchors are also resync points (see below). The writer emits one
 * for the first frame of each DB per file, periodically, and for large
 * changes — every full-image emission is an anchor, never a bare frame.
 */
inline constexpr uint16_t kAnchorFrameDbNum = 0xFFFD;

/**
 * @brief CRC32 (IEEE, zlib-compatible) over raw bytes.
 *
 * Used for binary anchor-frame integrity. Small, table-free implementation:
 * fine at archive rates for DB-sized images. Readers recompute over the
 * decoded image and compare against the stored value.
 */
inline uint32_t binaryWalCrc32(const uint8_t* t_data, size_t t_size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < t_size; ++i) {
        crc ^= static_cast<uint32_t>(t_data[i]);
        for (int k = 0; k < 8; ++k)
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
    return ~crc;
}

/**
 * @brief Current binary WAL version stamped in the archive header.
 *
 * v1 = full-image data frames only. v2 = v1 + delta frames. v3 = v2 +
 * anchor frames (verifiable full images). Readers refuse versions above
 * this; writers always stamp this.
 */
inline constexpr uint16_t kBinaryWalVersion = 3;

// ── Shared binary-WAL decode primitives ──────────────────────────────────
// One implementation for every reader (sgrn_dataset, sgrn_replay,
// RecoveryEngine) so framing, validation and resync stay identical.
// These are pure and policy-free: corruption *reporting* (warn vs count vs
// abort) stays with the caller.

/// Validated archive header.
struct BinaryWalHeader {
    uint16_t version = 0;
    uint32_t schema_len = 0;
    size_t frames_start = 10; ///< first byte after header + schema JSON
};

/// Header validation outcome (callers map these to their own diagnostics).
enum class BinaryHeaderStatus {
    kOk,
    kTooShort,   ///< fewer than 10 bytes or schema overruns the buffer
    kBadMagic,   ///< missing SGRN magic
    kBadVersion, ///< ver < 1 (v0 never shipped) — readers handle 1..kBinaryWalVersion
};

/// Validates magic + version + header bounds. No logging; see above.
inline BinaryHeaderStatus checkBinaryWalHeader(const std::string& t_data, BinaryWalHeader& t_header) {
    if (t_data.size() < 10)
        return BinaryHeaderStatus::kTooShort;
    if (t_data[0] != 'S' || t_data[1] != 'G' || t_data[2] != 'R' || t_data[3] != 'N')
        return BinaryHeaderStatus::kBadMagic;
    uint16_t ver = 0;
    uint32_t schema_len = 0;
    std::memcpy(&ver, t_data.data() + 4, sizeof(ver));
    std::memcpy(&schema_len, t_data.data() + 6, sizeof(schema_len));
    if (10 + schema_len > t_data.size())
        return BinaryHeaderStatus::kTooShort;
    if (ver < 1 || ver > kBinaryWalVersion)
        return BinaryHeaderStatus::kBadVersion;
    t_header.version = ver;
    t_header.schema_len = schema_len;
    t_header.frames_start = 10 + schema_len;
    return BinaryHeaderStatus::kOk;
}

/// One decoded frame header. `payload` points into the source buffer.
struct BinaryFrame {
    int64_t ts = 0;
    uint16_t db = 0;
    uint32_t payload_len = 0;
    size_t header_start = 0; ///< offset of the 14-byte header (resync base)
    const uint8_t* payload = nullptr;
};

/// Frame decode outcome.
enum class BinaryFrameStatus {
    kOk,       ///< t_pos advanced past the payload; t_frame filled
    kEnd,      ///< fewer than 14 bytes remain (clean EOF); t_pos untouched
    kTruncated ///< header parsed but payload overruns (t_pos left at header)
};

/// Decodes the frame at t_pos. Never reads out of bounds.
inline BinaryFrameStatus decodeBinaryFrame(const std::string& t_data, size_t& t_pos, BinaryFrame& t_frame) {
    if (t_pos + 14 > t_data.size())
        return BinaryFrameStatus::kEnd;
    int64_t ts = 0;
    uint16_t db = 0;
    uint32_t len = 0;
    std::memcpy(&ts, t_data.data() + t_pos, sizeof(ts));
    std::memcpy(&db, t_data.data() + t_pos + 8, sizeof(db));
    std::memcpy(&len, t_data.data() + t_pos + 10, sizeof(len));
    if (t_pos + 14 + len > t_data.size())
        return BinaryFrameStatus::kTruncated;
    t_frame.ts = ts;
    t_frame.db = db;
    t_frame.payload_len = len;
    t_frame.header_start = t_pos;
    t_frame.payload = reinterpret_cast<const uint8_t*>(t_data.data()) + t_pos + 14;
    t_pos += 14 + len;
    return BinaryFrameStatus::kOk;
}

/// A validated delta run: [offset, offset+len) within the image, with the
/// run's bytes at data_pos within the frame payload.
struct BinaryDeltaRun {
    uint32_t offset = 0;
    uint32_t len = 0;
    uint32_t data_pos = 0;
};

/// Validates a delta payload (`db:u16` + runs) against an image size limit
/// (cached image size, or live segment size for direct writers). Extracts the
/// target DB and the run list. Pure: applying runs is the caller's job.
inline bool parseDeltaRuns(const uint8_t* t_payload, uint32_t t_len, size_t t_limit, uint16_t& t_db, std::vector<BinaryDeltaRun>& t_runs) {
    t_runs.clear();
    if (t_len < 2)
        return false;
    std::memcpy(&t_db, t_payload, sizeof(t_db));
    size_t run_pos = 2;
    while (run_pos + 8 <= t_len) {
        uint32_t run_off = 0;
        uint32_t run_len = 0;
        std::memcpy(&run_off, t_payload + run_pos, sizeof(run_off));
        std::memcpy(&run_len, t_payload + run_pos + sizeof(run_off), sizeof(run_len));
        run_pos += 8;
        if (run_pos + run_len > t_len || static_cast<size_t>(run_off) + run_len > t_limit)
            return false;
        t_runs.push_back(BinaryDeltaRun{run_off, run_len, static_cast<uint32_t>(run_pos)});
        run_pos += run_len;
    }
    return run_pos == t_len && !t_runs.empty();
}

/// Verifies an anchor payload (`db:u16` + `crc32:u32` + image). Extracts the
/// target DB and a pointer/len view of the image (into the source buffer).
inline bool verifyAnchorFrame(const uint8_t* t_payload, uint32_t t_len, uint16_t& t_db, const uint8_t*& t_image, size_t& t_image_len) {
    if (t_len < 6)
        return false;
    uint32_t crc = 0;
    std::memcpy(&t_db, t_payload, sizeof(t_db));
    std::memcpy(&crc, t_payload + sizeof(t_db), sizeof(crc));
    t_image = t_payload + 6;
    t_image_len = t_len - 6;
    return binaryWalCrc32(t_image, t_image_len) == crc;
}

/// Scans forward for the next verifiable anchor frame. Returns its frame
/// start offset, or nullopt. CRC verification makes false hits ~2^-32, so a
/// hit is a trustworthy resync point after stream corruption.
inline std::optional<size_t> findNextAnchorFrame(const std::string& t_data, size_t t_from) {
    const size_t n = t_data.size();
    for (size_t p = t_from; p + 14 <= n; ++p) {
        uint16_t db = 0;
        std::memcpy(&db, t_data.data() + p + 8, sizeof(db));
        if (db != kAnchorFrameDbNum)
            continue;
        uint32_t len = 0;
        std::memcpy(&len, t_data.data() + p + 10, sizeof(len));
        if (len < 6 || p + 14 + len > n)
            continue;
        const uint8_t* pl = reinterpret_cast<const uint8_t*>(t_data.data()) + p + 14;
        uint16_t adb = 0;
        uint32_t crc = 0;
        std::memcpy(&adb, pl, sizeof(adb));
        (void)adb;
        std::memcpy(&crc, pl + sizeof(adb), sizeof(crc));
        if (binaryWalCrc32(pl + 6, len - 6) == crc)
            return p;
    }
    return std::nullopt;
}

/// Fills id-indexed paths from a {"type":"dictionary","leaves":[...]} line.
inline void parseDictionaryLine(const rapidjson::Document& t_doc, std::vector<std::string>& t_paths) {
    if (!t_doc.HasMember("leaves") || !t_doc["leaves"].IsArray())
        return;
    for (const auto& item : t_doc["leaves"].GetArray()) {
        if (item.IsObject() && item.HasMember("id") && item["id"].IsUint() && item.HasMember("path") && item["path"].IsString()) {
            const auto id = item["id"].GetUint();
            if (id >= t_paths.size())
                t_paths.resize(static_cast<size_t>(id) + 1);
            t_paths[id] = item["path"].GetString();
        }
    }
}

/// True for a dictionary line (caller already validated IsObject).
inline bool isDictionaryRecord(const rapidjson::Document& t_doc) {
    return t_doc.HasMember("type") && t_doc["type"].IsString() && std::string_view(t_doc["type"].GetString()) == "dictionary";
}

/**
 * @brief Reads one full DB image (whole segment, offset 0).
 *
 * Binary WAL frames are full-DB snapshots: readers decode fields at schema
 * offsets (sgrn_dataset) and replay writes buffers at offset 0 (sgrn_replay).
 * TelemetryEvent typed payloads are per-leaf slices WITHOUT an offset, so
 * the writer cannot use them directly — it pulls the full image through this
 * callback instead (wired to PlcMemory by GatewayApplication).
 */
using FullDbReadFn = std::function<sgrn::Result<std::vector<uint8_t>>(uint16_t)>;

/**
 * @brief Independent local historian / WAL archiver service.
 *
 * ARCHITECTURAL ROLE
 * ------------------
 * PersistenceService is the *local persistence layer* in the gateway: it is the
 * single writer of the write-ahead log (WAL) that doubles as both the historian
 * and the boot-recovery source of the digital twin.
 *
 *   - PersistenceService subscribes to TelemetryBroker and writes
 *     WAL files to `state/unsynced/` on disk.
 *
 *   - DatastoreBridge is a dumb uploader: it polls `pending_batches` in
 *     GatewayDatabase and uploads the files when connectivity is available,
 *     then moves them into `state/synced/`.
 *
 *   - At boot, RecoveryEngine scans unsynced+synced for the newest archive and
 *     rebuilds the twin from its last anchor + deltas. PersistenceService does
 *     NOT read anything itself.
 *
 * CANONICAL FORMAT
 * ----------------
 * The binary WAL (`.bin.zst`) is the canonical on-disk format: framed
 * full-image snapshots, sparse delta frames, and CRC-verified anchor frames
 * (see `documentation/gateway/binary-format.md`). The JSONL WAL
 * (`.jsonl.zst`, see `documentation/gateway/jsonl-format.md`) is the legacy
 * format: still the default and still the only one RecoveryEngine reads at
 * boot, but new archival features target binary first.
 *
 * ARCHIVE MODES  (configured via PersistenceConfig::mode)
 * -------------------------------------------------------
 *  "full_tree"
 *     Every delta is written as a full snapshot.
 *     High storage footprint; easy to consume downstream.
 *
 *  "full_tree_with_anchor"
 *     A full snapshot is written on startup and then again when:
 *       * anchor_interval_s seconds have elapsed  (time-based, 0 = disabled)
 *       * anchor_change_count records have accumulated (count-based, 0 = disabled)
 *     Between anchors only changed fields are archived.
 *
 *  "changes_with_timestamp"
 *     Only changed fields are archived. Multiple DeltaSnapshot events that
 *     arrive within atomic_window_ms of each other are merged into a single
 *     record, preserving PLC-scan-cycle atomicity. In binary mode the merge
 *     window only decides *when* a snapshot is taken; every stored frame is
 *     still a full image or a diff against one.
 *
 * The whole file is a single Zstd frame written incrementally via
 * ZstdLineWriter, so peak resident memory stays bounded by the zstd output
 * buffer size regardless of twin size or batch_size.
 */
class PersistenceService {
public:
    explicit PersistenceService(asio::thread_pool* tp_heavy_pool);
    ~PersistenceService();

    sgrn::Result<void> configure(const sgrn::gateway::config::PersistenceConfig& t_cfg, const std::string& t_state_dir,
        std::shared_ptr<GatewayDatabase> tsp_db, const std::string& t_schema_json = std::string{},
        const scl::PlcSchemaStore* t_schema_store = nullptr, FullDbReadFn t_read_db = {});

    /// Inject a pre-built LeafDictionary (from GatewayApplication). When set,
    /// PersistenceService skips building its own and reuses the shared instance.
    void setLeafDictionary(const twin::LeafDictionary& t_dict) {
        dict_ = t_dict;
        rebuildAllowedByIndex();
    }

    void stop();

    /// True if the service is active (configured and not stopped).
    bool isActive() const {
        return active_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Inject a full-tree JSON string directly (e.g. from the S7 poller).
     *
     * Writes an "anchor" line into the current WAL archive.
     */
    void ingestFullTree(const std::string& t_full_tree_json, const std::string& t_flat_tree_json = "");

private:
    // -- Internal event handler --------------------------------------------
    void onTelemetryEvent(const sgrn::gateway::core::TelemetryEvent& t_event);

    // -- Namespace filter ---------------------------------------------------
    bool passesFilter(twin::LeafId t_id) const;
    void rebuildAllowedByIndex();

    // -- Atomic merge buffer ------------------------------------------------
    struct MergeBuffer {
        int64_t open_ts{0};                                   ///< timestamp of first event in window
        std::unordered_map<twin::LeafId, std::string> fields; ///< path -> JSON value string
    };

    void mergeOrFlushEvent(twin::LeafId t_id, const std::string& t_value_json, int64_t t_now);
    void commitMergeBuffer();

    // -- WAL archive lifecycle ---------------------------------------------

    /// Lazily opens a fresh WAL archive (schema + manifest lines) if none is open.
    /// Sets current_line_counter_ = 2.
    sgrn::Result<void, std::string> openNewArchive(int64_t t_now);

    /// Writes one "delta" line into the open archive.
    sgrn::Result<void> writeDeltaLine(const std::string& t_record_json);

    /// Writes one "anchor" line into the open archive and records its line index.
    sgrn::Result<void> writeAnchorLine(const std::string& t_full_tree_json, int64_t t_ts);

    /// Wraps a JSON control record (dictionary / manifest / anchor / footer) in
    /// the binary frame envelope (ts:8)(0xFFFF:2)(len:4)(json) when the archive
    /// is binary. Callers must still bump current_line_counter_.
    sgrn::Result<void> writeBinaryControlLine(const std::string& t_json, int64_t t_ts);

    /// Writes one verifiable binary anchor frame: (ts)(0xFFFD)(len)(db:crc:image).
    /// Requires an open archive and a non-empty image; caller owns
    /// last_db_bytes_, last_keyframe_ts_, the line counter, flush.
    void writeBinaryAnchor(uint16_t t_db, const std::vector<uint8_t>& t_image, int64_t t_ts);

    /// Closes the archive (footer + stream end + rename + DB registration).
    void finalizeArchive(int64_t t_ts_end);

    void maybeFlushBatch(int64_t t_now);

    // -- Anchor management ---------------------------------------------------
    bool anchorDue(int64_t t_now) const;
    void resetAnchor(int64_t t_now);

    // -- Members -------------------------------------------------------------
    asio::thread_pool* heavy_pool_{nullptr};
    sgrn::gateway::config::PersistenceConfig cfg_;
    std::string state_dir_;
    std::string unsynced_dir_;
    std::shared_ptr<GatewayDatabase> db_;
    std::string schema_json_; ///< serialized PlcSchemaStore, written as WAL line 1

    twin::LeafDictionary dict_;
    std::vector<bool> allowed_by_id_;

    sgrn::gateway::core::TelemetryBroker::SubscriberId broker_sub_id_{0};
    std::atomic<bool> active_{false};

    // WAL state (all access on the io_context strand -- no extra lock needed).
    std::unique_ptr<sgrn::utils::compression::ZstdLineWriter> current_archive_;
    std::string current_archive_path_; ///< provisional "*.jsonl.zst.tmp" path; renamed on finalize
    int64_t current_archive_start_ts_{0};
    size_t current_line_counter_{0};    ///< 1-indexed line count written to current_archive_
    int64_t last_anchor_line_index_{0}; ///< 0 = no anchor line written yet in this archive

    // Atomic merge buffer
    MergeBuffer merge_buf_;

    // Binary-mode full-DB snapshot support (see FullDbReadFn). last_db_bytes_
    // dedupes consecutive identical images; it is reset per archive so every
    // file starts with a state-establishing frame.
    FullDbReadFn read_db_fn_;
    std::unordered_map<uint16_t, std::vector<uint8_t>> last_db_bytes_;
    // Last keyframe (full-image) timestamp per DB, for the delta policy.
    std::unordered_map<uint16_t, int64_t> last_keyframe_ts_;
    // Effective binary keyframe interval (ms), derived from configuration
    // (see configure()).
    int64_t binary_keyframe_interval_ms_{60000};
    // Default when anchoring is disabled in config: binary deltas still need
    // restart points for resync, so keyframes never go away entirely.
    static constexpr int64_t kDefaultBinaryKeyframeIntervalMs = 60000;
    // Per-archive record counts for the footer. Frames (binary) and lines
    // (JSONL) are counted separately so a footer never mixes units.
    uint64_t anchor_frames_{0};
    uint64_t delta_frames_{0};
    uint64_t anchor_lines_{0};
    uint64_t delta_lines_{0};

    // Anchor state
    int64_t last_anchor_ts_{0};
    uint32_t changes_since_anchor_{0};
    bool needs_first_anchor_{true};

    // Pending finalize task guard
    const int max_pending_ = 200;
    std::atomic<int> pending_tasks_{0};
};

} // namespace sgrn::gateway::database
