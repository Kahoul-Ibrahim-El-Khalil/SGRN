// ─────────────────────────────────────────────────────────────────────────────
// PersistenceService.cpp
//
// Independent local historian / WAL archiver service.
// See PersistenceService.hpp for full architectural documentation.
//
// JSONL WAL LINE CONTRACT (one JSON object per line inside a single Zstd
// frame; see documentation/gateway/jsonl-format.md):
//   Line 1      {"type":"schema","schema":{<serialized PlcSchemaStore>}|null}
//   Line 2      {"type":"dictionary","leaves":[{"id":<uint>,"path":"Db.field"}]}
//   Line 3      {"type":"manifest","start_time":"<iso8601>","mode":"...",
//                 "namespaces":[...], ...}
//   Lines 4..N-1 {"type":"anchor","ts":<ms>,"data":{"<leaf-id>":<value>}}
//                {"type":"delta","ts":<ms>,"changes":{"<leaf-id>":<value>}}
//   Line N      {"type":"footer","last_anchor_line":<line>,"record_count":<line>,
//                 "anchor_count":<n>,"delta_count":<n>}
//
// Anchor data and delta changes are flat LeafDictionary-ID-keyed objects,
// never nested trees. RecoveryEngine reads this file at boot: the footer's
// last_anchor_line points at the most recent anchor, so recovery never
// re-reads the whole file.
//
// BINARY WAL CONTRACT (canonical format; see
// documentation/gateway/binary-format.md): after the 10-byte header
// ("SGRN"+version+schema_len+schema JSON), an uninterrupted run of
// (ts:8)(db_num:2)(payload_len:4)(payload) frames, where db_num selects the
// kind: real DB id = full image (v1 legacy), 0xFFFE = delta runs (v2+),
// 0xFFFD = CRC-verified anchor image (v3+), 0xFFFF = JSON control record.
// ─────────────────────────────────────────────────────────────────────────────

#include <sgrn/gateway/core/GlobalContext.hpp>
#include <sgrn/gateway/database/PersistenceService.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/json.hpp>
#include <sgrn/utils/time.hpp>

#include <fmt/color.h>
#include <fmt/core.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>

namespace sgrn::gateway::database
{
using namespace sgrn::gateway::core;
using namespace sgrn::gateway::config;

using sgrn::Result;

namespace
{

/// Binary delta policy: emit a delta frame when its runs are cheaper than
/// this fraction of a full image. The keyframe interval itself comes from
/// configuration (anchor_interval_s, else the default) — see configure().
constexpr double kBinaryDeltaMaxRatio = 0.5;

struct DeltaRun {
    uint32_t offset;
    uint32_t len;
};

/// Maximal changed-byte runs of t_new versus t_old (same size).
std::vector<DeltaRun> diffRuns(const std::vector<uint8_t>& t_old, const std::vector<uint8_t>& t_new) {
    std::vector<DeltaRun> runs;
    size_t i = 0;
    const size_t n = t_new.size();
    while (i < n) {
        if (t_old[i] == t_new[i]) {
            ++i;
            continue;
        }
        size_t start = i;
        while (i < n && t_old[i] != t_new[i])
            ++i;
        runs.push_back(DeltaRun{static_cast<uint32_t>(start), static_cast<uint32_t>(i - start)});
    }
    return runs;
}

} // namespace

PersistenceService::PersistenceService(asio::thread_pool* tp_heavy_pool)
    : heavy_pool_(tp_heavy_pool) {
}

PersistenceService::~PersistenceService() {
    stop();
}

Result<void> PersistenceService::configure(const PersistenceConfig& t_cfg, const std::string& t_state_dir,
    std::shared_ptr<GatewayDatabase> tsp_db, const std::string& t_schema_json, const scl::PlcSchemaStore* t_schema_store,
    FullDbReadFn t_read_db) {
    if (!t_cfg.enabled) {
        fmt::print(fg(fmt::color::yellow), "[persist] Persistence disabled — skipping.\n");
        return {};
    }

    cfg_ = t_cfg;
    if (t_schema_store) {
        // Expand legacy "DBx" namespaces to actual DB names if present in schema.
        std::vector<std::string> expanded_namespaces;
        for (const auto& ns : cfg_.namespaces) {
            if (ns.rfind("DB", 0) == 0 && ns.size() > 2 && std::all_of(ns.begin() + 2, ns.end(), ::isdigit)) {
                uint16_t db_num = std::stoi(ns.substr(2));
                auto it = t_schema_store->dbs().find(db_num);
                if (it != t_schema_store->dbs().end() && !it->second.db_name.empty()) {
                    expanded_namespaces.push_back(it->second.db_name);
                } else {
                    expanded_namespaces.push_back(ns);
                }
            } else {
                expanded_namespaces.push_back(ns);
            }
        }
        cfg_.namespaces = expanded_namespaces;
    }

    state_dir_ = t_state_dir;
    unsynced_dir_ = t_state_dir + "/unsynced";
    db_ = std::move(tsp_db);
    schema_json_ = t_schema_json;
    read_db_fn_ = std::move(t_read_db);

    // One anchor cadence for both formats: binary keyframes follow
    // anchor_interval_s when set, otherwise the default. (Binary deltas
    // require restart points for resync, so unlike JSONL anchors this never
    // disables entirely.)
    binary_keyframe_interval_ms_ =
        cfg_.anchor_interval_s > 0 ? static_cast<int64_t>(cfg_.anchor_interval_s) * 1000 : kDefaultBinaryKeyframeIntervalMs;

    try {
        std::filesystem::create_directories(unsynced_dir_);
    } catch (const std::exception& e) {
        return fmt::format("PersistenceService: cannot create directories: {}", e.what());
    }

    const int64_t t_now = sgrn::utils::time::nowMilliseconds();
    last_anchor_ts_ = t_now;

    // Subscribe to the global TelemetryBroker.
    // All callbacks arrive on the io_context, so WAL state is single-threaded.
    broker_sub_id_ = TelemetryBroker::instance().subscribe([this](const TelemetryEvent& t_ev) { onTelemetryEvent(t_ev); });

    if (t_schema_store && dict_.path_by_id.empty()) {
        dict_ = twin::LeafDictionary::buildFrom(*t_schema_store);
    }

    rebuildAllowedByIndex();

    active_.store(true, std::memory_order_relaxed);

    fmt::print(fg(fmt::color::cyan), "[persist] Active. mode={} atomic_window={}ms batch_size={} batch_interval={}s\n", cfg_.mode,
        cfg_.atomic_window_ms, cfg_.batch_size, cfg_.batch_interval_s);

    return {};
}

void PersistenceService::stop() {
    if (!active_.load(std::memory_order_relaxed))
        return;
    active_.store(false, std::memory_order_relaxed);
    if (broker_sub_id_ != 0) {
        TelemetryBroker::instance().unsubscribe(broker_sub_id_);
        broker_sub_id_ = 0;
    }
    // Close any archive still open so the WAL on disk ends with a valid footer.
    if (current_archive_) {
        finalizeArchive(sgrn::utils::time::nowMilliseconds());
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// TelemetryBroker callback
// ─────────────────────────────────────────────────────────────────────────────

void PersistenceService::onTelemetryEvent(const TelemetryEvent& t_event) {
    if (!active_.load(std::memory_order_relaxed))
        return;

    if (pending_tasks_.load(std::memory_order_relaxed) >= max_pending_)
        return;

    const int64_t t_now = sgrn::utils::time::nowMilliseconds();

    // ── FullSnapshot: write an anchor line regardless of mode ────────────────
    if (t_event.type == EventType::FullSnapshot && t_event.json_value) {
        const std::string& json = *t_event.json_value;
        if (!json.empty() && json != "{}") {
            ingestFullTree(json);
        }
        return;
    }

    // ── Binary Mode: Full DB Raw Memory Snapshot Writing ───────────────────
    // Frames are full-DB images: readers decode fields at schema offsets and
    // replay writes buffers at offset 0. TelemetryEvent typed payloads are
    // per-leaf slices with no offset, so the full image is pulled through
    // read_db_fn_ (PlcMemory) instead of using the event bytes directly.
    const bool is_binary = (cfg_.format == "binary" || cfg_.format == "bin.zst");
    if (is_binary) {
        if (!current_archive_) {
            (void)openNewArchive(t_now);
        }
        if (current_archive_ && t_event.db > 0) {
            const uint64_t ts = t_event.timestamp > 0 ? t_event.timestamp : static_cast<uint64_t>(t_now);
            if (read_db_fn_) {
                auto full_res = read_db_fn_(t_event.db);
                if (full_res.hasValue() && !full_res.value().empty()) {
                    auto& last = last_db_bytes_[t_event.db];
                    if (last != full_res.value()) {
                        const auto& full = full_res.value();
                        const uint16_t db_num = t_event.db;
                        const auto key_it = last_keyframe_ts_.find(t_event.db);
                        const bool need_keyframe =
                            last.empty() || key_it == last_keyframe_ts_.end() || (t_now - key_it->second) >= binary_keyframe_interval_ms_;

                        bool wrote_delta = false;
                        if (!need_keyframe && last.size() == full.size()) {
                            const auto runs = diffRuns(last, full);
                            uint64_t delta_bytes = 2; // embedded db_num
                            for (const auto& run : runs)
                                delta_bytes += 8 + run.len;
                            if (!runs.empty() && delta_bytes <= 0xFFFFFFFFu &&
                                delta_bytes < static_cast<double>(full.size()) * kBinaryDeltaMaxRatio) {
                                const uint16_t marker = kDeltaFrameDbNum;
                                const uint32_t total = static_cast<uint32_t>(delta_bytes);
                                (void)current_archive_->writeRaw(&ts, sizeof(ts));
                                (void)current_archive_->writeRaw(&marker, sizeof(marker));
                                (void)current_archive_->writeRaw(&total, sizeof(total));
                                (void)current_archive_->writeRaw(&db_num, sizeof(db_num));
                                for (const auto& run : runs) {
                                    (void)current_archive_->writeRaw(&run.offset, sizeof(run.offset));
                                    (void)current_archive_->writeRaw(&run.len, sizeof(run.len));
                                    (void)current_archive_->writeRaw(full.data() + run.offset, run.len);
                                }
                                wrote_delta = true;
                                ++delta_frames_;
                            }
                        }
                        if (!wrote_delta) {
                            writeBinaryAnchor(t_event.db, full, static_cast<int64_t>(ts));
                            // Wall clock on both sides: the frame carries the
                            // event timestamp, but keyframe ageing is measured
                            // in wall time (event clocks may lag).
                            last_keyframe_ts_[t_event.db] = t_now;
                        }
                        last = full;
                        ++current_line_counter_;
                        maybeFlushBatch(t_now);
                    }
                }
            } else if (t_event.typed_leaf.bytes && !t_event.typed_leaf.bytes->empty()) {
                // Degraded fallback (no full-DB reader wired): anchors the
                // per-leaf slice as-is, CRC'd over the stored bytes. Every
                // data frame self-identifies as delta or anchor; this one is
                // verifiable but only exact for offset-0 leaves (slices carry
                // no offset), and it bypasses dedup/keyframe tracking.
                writeBinaryAnchor(t_event.db, *t_event.typed_leaf.bytes, static_cast<int64_t>(ts));
                ++current_line_counter_;
                maybeFlushBatch(t_now);
            }
        }
        return;
    }

    // LeafUpdate events carry typed payloads with no JSON attached — the
    // DeltaSnapshot event from the same dirty batch carries the JSON snapshot
    // the WAL path below consumes. Dereferencing json_value here segfaults.
    if (t_event.type != EventType::DeltaSnapshot)
        return;
    if (!t_event.json_value || t_event.json_value->empty())
        return;

    // ── PERFORMANCE NOTE: JSON Parsing and Filtering ─────────────────────────
    // The TelemetryBroker broadcasts the SAME shared_ptr<string> to all subscribers.
    // This JSON was already serialized once by PlcState::getDeltaSnapshot().
    // PersistenceService must:    //   1. Parse the JSON (RapidJSON DOM construction)
    //   2. Extract individual field paths and values
    //   3. Apply namespace filtering per field
    //   4. Re-serialize each field individually as a JSONL delta line
    //
    // The actual zstd compression now happens incrementally inside the writer
    // (bounded by ZSTD_CStreamOutSize()), so a full-batch compress + envelope
    // pass is gone: per-event cost is O(line size) in both CPU and memory.
    // ─────────────────────────────────────────────────────────────────────────

    // For the other two modes we apply namespace filtering and atomic merging.
    rapidjson::Document doc;
    doc.Parse(t_event.json_value->c_str());
    if (doc.HasParseError() || !doc.IsObject())
        return;

    // Iterate top-level keys (DB names) → fields recursively.
    // Delta payloads come in three shapes, all of which must land in the
    // merge buffer keyed by LeafId:
    //   nested       {"DbName": {"field": v, "struct": {"sub": v}}}
    //   flat numeric {"<leaf_id>": v}  (dictionary mode — the common case)
    //   flat dotted  {"DbName.field": v} (legacy)
    auto processLeaf = [&](const std::string& t_path, const rapidjson::Value& t_value) {
        auto id_it = dict_.path_to_id.find(t_path);
        if (id_it == dict_.path_to_id.end()) {
            return;
        }
        twin::LeafId id = id_it->second;
        if (!passesFilter(id))
            return;

        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        t_value.Accept(w);

        mergeOrFlushEvent(id, sb.GetString(), t_now);
    };

    // getDeltaSnapshot() may contain nested leaves (e.g. ReactorCore.rods.position_pct),
    // so we walk the full JSON tree and emit one delta per leaf.
    std::function<void(const std::string&, const rapidjson::Value&)> walkFields;
    walkFields = [&](const std::string& t_prefix, const rapidjson::Value& t_obj) {
        for (auto f_it = t_obj.MemberBegin(); f_it != t_obj.MemberEnd(); ++f_it) {
            const std::string t_path = t_prefix.empty() ? f_it->name.GetString() : t_prefix + "." + f_it->name.GetString();
            if (f_it->value.IsObject()) {
                walkFields(t_path, f_it->value);
                continue;
            }
            processLeaf(t_path, f_it->value);
        }
    };

    auto isDigits = [](const std::string& t_s) {
        return !t_s.empty() && t_s.size() <= 10 &&
               std::all_of(t_s.begin(), t_s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    };

    for (auto db_it = doc.MemberBegin(); db_it != doc.MemberEnd(); ++db_it) {
        if (db_it->value.IsObject()) {
            const std::string db_name = db_it->name.GetString();
            walkFields(db_name, db_it->value);
            continue;
        }
        // Flat leaf straight at top level: numeric id or dotted path.
        const std::string key = db_it->name.GetString();
        if (isDigits(key)) {
            const size_t id = std::stoul(key);
            if (id < dict_.path_by_id.size() && !dict_.path_by_id[id].empty())
                processLeaf(dict_.path_by_id[id], db_it->value);
        } else {
            processLeaf(key, db_it->value);
        }
    }

    maybeFlushBatch(t_now);
}

// ─────────────────────────────────────────────────────────────────────────────
// Public ingestion of full-tree snapshots
// ─────────────────────────────────────────────────────────────────────────────

void PersistenceService::ingestFullTree(const std::string& t_full_tree_json, const std::string& t_flat_tree_json) {
    const int64_t t_now = sgrn::utils::time::nowMilliseconds();

    if (auto res = openNewArchive(t_now); res.hasError()) {
        fmt::print(fg(fmt::color::red), "[persist] Cannot open WAL archive: {}\n", res.error());
        return;
    }

    // Commit any open atomic merge window so archive ordering is consistent.
    commitMergeBuffer();

    const std::string& anchor_json = t_flat_tree_json.empty() ? t_full_tree_json : t_flat_tree_json;
    if (auto res = writeAnchorLine(anchor_json, t_now); res.hasError()) {
        fmt::print(fg(fmt::color::red), "[persist] Anchor line write failed: {}\n", res.error());
        return;
    }

    // Binary archives additionally anchor every known DB as a verifiable
    // image frame, so a snapshot always leaves verifiable baselines behind
    // (the JSON control above is kept for transcode compatibility).
    if ((cfg_.format == "binary" || cfg_.format == "bin.zst") && read_db_fn_ && current_archive_) {
        for (auto& [db_num, last] : last_db_bytes_) {
            auto full_res = read_db_fn_(db_num);
            if (full_res.hasError() || full_res.value().empty())
                continue;
            writeBinaryAnchor(db_num, full_res.value(), t_now);
            last = full_res.value();
            last_keyframe_ts_[db_num] = t_now;
            ++current_line_counter_;
        }
        maybeFlushBatch(t_now);
    }

    if (cfg_.mode == "full_tree_with_anchor")
        resetAnchor(t_now);
}

// ─────────────────────────────────────────────────────────────────────────────
// Namespace filter
// ─────────────────────────────────────────────────────────────────────────────

bool PersistenceService::passesFilter(twin::LeafId t_id) const {
    if (allowed_by_id_.empty())
        return true;
    if (t_id >= allowed_by_id_.size())
        return false;
    return allowed_by_id_[t_id];
}

void PersistenceService::rebuildAllowedByIndex() {
    allowed_by_id_.clear();
    if (dict_.path_by_id.empty())
        return;
    allowed_by_id_.resize(dict_.path_by_id.size(), false);

    bool match_all = cfg_.namespaces.empty();
    for (const auto& ns : cfg_.namespaces) {
        if (ns == "*" || ns == "all") {
            match_all = true;
            break;
        }
    }

    for (size_t id = 0; id < dict_.path_by_id.size(); ++id) {
        const std::string& path = dict_.path_by_id[id];
        bool passes = match_all;
        if (!match_all) {
            for (const auto& ns : cfg_.namespaces) {
                if (path.rfind(ns, 0) == 0) {
                    passes = true;
                    break;
                }
            }
        }
        allowed_by_id_[id] = passes;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Atomic merge window
// ─────────────────────────────────────────────────────────────────────────────

void PersistenceService::mergeOrFlushEvent(twin::LeafId t_id, const std::string& t_value_json, int64_t t_now) {
    const uint32_t window = cfg_.atomic_window_ms;

    if (window == 0) {
        // Merging disabled — emit a standalone delta line immediately.
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        w.StartObject();
        w.Key("type");
        w.String("delta");
        w.Key("ts");
        w.Int64(t_now);
        w.Key("changes");
        w.StartObject();
        std::string id_str = std::to_string(t_id);
        w.Key(id_str.c_str(), static_cast<rapidjson::SizeType>(id_str.size()));
        w.RawValue(t_value_json.c_str(), t_value_json.size(), rapidjson::kObjectType);
        w.EndObject();
        w.EndObject();
        (void)writeDeltaLine(sb.GetString());
        return;
    }

    // If the merge buffer is open and within the atomic window, accumulate.
    if (merge_buf_.open_ts != 0 && (t_now - merge_buf_.open_ts) <= static_cast<int64_t>(window)) {
        merge_buf_.fields[t_id] = t_value_json;
        return;
    }

    // Commit whatever was in the buffer before opening a new window.
    if (merge_buf_.open_ts != 0)
        commitMergeBuffer();

    merge_buf_.open_ts = t_now;
    merge_buf_.fields[t_id] = t_value_json;
}

void PersistenceService::commitMergeBuffer() {
    if (merge_buf_.open_ts == 0 || merge_buf_.fields.empty())
        return;

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("type");
    w.String("delta");
    w.Key("ts");
    w.Int64(merge_buf_.open_ts);
    w.Key("changes");
    w.StartObject();
    for (const auto& [t_id, val] : merge_buf_.fields) {
        std::string id_str = std::to_string(t_id);
        w.Key(id_str.c_str(), static_cast<rapidjson::SizeType>(id_str.size()));
        w.RawValue(val.c_str(), val.size(), rapidjson::kObjectType);
    }
    w.EndObject();
    w.EndObject();

    (void)writeDeltaLine(sb.GetString());

    merge_buf_.open_ts = 0;
    merge_buf_.fields.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// WAL archive lifecycle
// ─────────────────────────────────────────────────────────────────────────────

Result<void, std::string> PersistenceService::openNewArchive(int64_t t_now) {
    if (current_archive_)
        return {};

    const std::string date_dir = sgrn::utils::time::datePath(t_now);
    const std::string start_file = sgrn::utils::time::timePath(t_now);
    const std::string target_dir = unsynced_dir_ + "/" + date_dir;
    try {
        std::filesystem::create_directories(target_dir);
    } catch (const std::exception& e) {
        return fmt::format("PersistenceService: cannot create archive directory: {}", e.what());
    }

    const bool is_binary = (cfg_.format == "binary" || cfg_.format == "bin.zst");
    const std::string ext = is_binary ? ".bin.zst" : ".jsonl.zst";
    const std::string tmp_path = target_dir + "/" + start_file + ext + ".tmp";

    current_archive_ = std::make_unique<sgrn::utils::compression::ZstdLineWriter>(tmp_path, cfg_.zstd_level);
    current_archive_path_ = tmp_path;
    current_archive_start_ts_ = t_now;
    current_line_counter_ = 0;
    last_anchor_line_index_ = 0;
    // A new file must re-establish state from its first frame, even if the
    // image is identical to the previous file's last frame.
    last_db_bytes_.clear();
    last_keyframe_ts_.clear();
    anchor_frames_ = 0;
    delta_frames_ = 0;
    anchor_lines_ = 0;
    delta_lines_ = 0;

    if (is_binary) {
        // Binary Header Frame: "SGRN" (4B) + Version (2B) + Schema Length (4B) + Schema JSON
        char magic[4] = {'S', 'G', 'R', 'N'};
        uint16_t ver = kBinaryWalVersion;
        uint32_t schema_len = static_cast<uint32_t>(schema_json_.size());

        (void)current_archive_->writeRaw(magic, 4);
        (void)current_archive_->writeRaw(&ver, sizeof(ver));
        (void)current_archive_->writeRaw(&schema_len, sizeof(schema_len));
        if (schema_len > 0) {
            (void)current_archive_->writeRaw(schema_json_.data(), schema_len);
        }
        ++current_line_counter_;
    } else {
        // ── Line 1: schema ───────────────────────────────────────────────────────
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        w.StartObject();
        w.Key("type");
        w.String("schema");
        w.Key("schema");
        if (schema_json_.empty()) {
            w.Null();
        } else {
            w.RawValue(schema_json_.c_str(), schema_json_.size(), rapidjson::kObjectType);
        }
        w.EndObject();

        auto schema_res = current_archive_->writeLine(sb.GetString());
        if (schema_res.hasError()) {
            current_archive_.reset();
            current_archive_path_.clear();
            return fmt::format("PersistenceService: schema line write failed: {}", schema_res.error());
        }
        ++current_line_counter_;
    }

    // ── Line 2: dictionary ───────────────────────────────────────────────────
    rapidjson::StringBuffer dsb;
    rapidjson::Writer<rapidjson::StringBuffer> dw(dsb);
    dw.StartObject();
    dw.Key("type");
    dw.String("dictionary");
    dw.Key("leaves");
    dw.StartArray();
    for (size_t id = 0; id < dict_.path_by_id.size(); ++id) {
        const auto& path = dict_.path_by_id[id];
        dw.StartObject();
        dw.Key("id");
        dw.Uint(static_cast<uint32_t>(id));
        dw.Key("path");
        dw.String(path.c_str(), static_cast<rapidjson::SizeType>(path.size()));
        dw.EndObject();
    }
    dw.EndArray();
    dw.EndObject();

    auto dict_res = [&]() -> Result<void> {
        if (is_binary) {
            return writeBinaryControlLine(dsb.GetString(), t_now);
        }
        return current_archive_->writeLine(dsb.GetString());
    }();
    if (dict_res.hasError()) {
        current_archive_.reset();
        current_archive_path_.clear();
        return fmt::format("PersistenceService: dictionary line write failed: {}", dict_res.error());
    }
    ++current_line_counter_;

    // ── Line 3: manifest ─────────────────────────────────────────────────────
    rapidjson::StringBuffer msb;
    rapidjson::Writer<rapidjson::StringBuffer> mw(msb);
    mw.StartObject();
    mw.Key("type");
    mw.String("manifest");
    mw.Key("start_time");
    const std::string ts_str = sgrn::utils::time::iso8601Timestamp(t_now);
    mw.String(ts_str.c_str(), static_cast<rapidjson::SizeType>(ts_str.size()));
    mw.Key("mode");
    mw.String(cfg_.mode.c_str(), static_cast<rapidjson::SizeType>(cfg_.mode.size()));
    mw.Key("namespaces");
    mw.StartArray();
    for (const auto& ns : cfg_.namespaces) {
        mw.String(ns.c_str(), static_cast<rapidjson::SizeType>(ns.size()));
    }
    mw.EndArray();
    // Encoding policy for this file, so readers know the restart-point
    // cadence without consulting gateway.json. Binary keyframes follow
    // anchor_interval_s when set (see configure()), JSONL anchors follow
    // the mode's own anchor triggers.
    if (is_binary) {
        mw.Key("keyframe_interval_s");
        mw.Int64(binary_keyframe_interval_ms_ / 1000);
        mw.Key("delta_ratio");
        mw.Double(kBinaryDeltaMaxRatio);
    } else {
        mw.Key("anchor_interval_s");
        mw.Uint(cfg_.anchor_interval_s);
        mw.Key("anchor_change_count");
        mw.Uint(cfg_.anchor_change_count);
    }
    mw.EndObject();

    auto manifest_res = [&]() -> Result<void> {
        if (is_binary) {
            return writeBinaryControlLine(msb.GetString(), t_now);
        }
        return current_archive_->writeLine(msb.GetString());
    }();
    if (manifest_res.hasError()) {
        current_archive_.reset();
        current_archive_path_.clear();
        return fmt::format("PersistenceService: manifest line write failed: {}", manifest_res.error());
    }
    ++current_line_counter_;

    return {};
}

Result<void> PersistenceService::writeDeltaLine(const std::string& t_record_json) {
    if (!current_archive_) {
        if (auto res = openNewArchive(sgrn::utils::time::nowMilliseconds()); res.hasError())
            return res.error();
    }

    Result<void> res;
    if (cfg_.format == "binary" || cfg_.format == "bin.zst") {
        // Defensive: the binary onTelemetryEvent() path never fills the merge
        // buffer, so this should be unreachable — but a raw JSON line would
        // corrupt the frame stream, so frame it as a control record anyway.
        res = writeBinaryControlLine(t_record_json, sgrn::utils::time::nowMilliseconds());
    } else {
        res = current_archive_->writeLine(t_record_json);
        ++delta_lines_;
    }
    if (res.hasError())
        return res;
    ++current_line_counter_;
    ++changes_since_anchor_;
    return {};
}

Result<void> PersistenceService::writeBinaryControlLine(const std::string& t_json, int64_t t_ts) {
    uint64_t ts = static_cast<uint64_t>(t_ts);
    uint16_t db_num = kControlFrameDbNum;
    uint32_t len = static_cast<uint32_t>(t_json.size());
    SGRN_IF_ERROR_PROPAGATE(current_archive_->writeRaw(&ts, sizeof(ts)));
    SGRN_IF_ERROR_PROPAGATE(current_archive_->writeRaw(&db_num, sizeof(db_num)));
    SGRN_IF_ERROR_PROPAGATE(current_archive_->writeRaw(&len, sizeof(len)));
    if (len > 0) {
        SGRN_IF_ERROR_PROPAGATE(current_archive_->writeRaw(t_json.data(), len));
    }
    return {};
}

void PersistenceService::writeBinaryAnchor(uint16_t t_db, const std::vector<uint8_t>& t_image, int64_t t_ts) {
    if (!current_archive_ || t_image.empty() || t_image.size() > static_cast<size_t>(0xFFFFFFFFu) - 6)
        return;
    const uint64_t ts = static_cast<uint64_t>(t_ts);
    const uint16_t marker = kAnchorFrameDbNum;
    const uint32_t total = static_cast<uint32_t>(2 + 4 + t_image.size());
    const uint32_t crc = binaryWalCrc32(t_image.data(), t_image.size());
    (void)current_archive_->writeRaw(&ts, sizeof(ts));
    (void)current_archive_->writeRaw(&marker, sizeof(marker));
    (void)current_archive_->writeRaw(&total, sizeof(total));
    (void)current_archive_->writeRaw(&t_db, sizeof(t_db));
    (void)current_archive_->writeRaw(&crc, sizeof(crc));
    (void)current_archive_->writeRaw(t_image.data(), t_image.size());
    ++anchor_frames_;
}

Result<void> PersistenceService::writeAnchorLine(const std::string& t_json, int64_t t_ts) {
    if (!current_archive_) {
        if (auto res = openNewArchive(sgrn::utils::time::nowMilliseconds()); res.hasError())
            return res.error();
    }

    // Parse the JSON. If it's already flat (numeric keys), we just write it.
    // Otherwise, flatten it.
    rapidjson::Document doc;
    doc.Parse(t_json.c_str());

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("type");
    w.String("anchor");
    w.Key("ts");
    w.Int64(t_ts);
    w.Key("data");

    bool is_flat = false;
    if (!doc.HasParseError() && doc.IsObject() && doc.MemberCount() > 0) {
        const char* first_key = doc.MemberBegin()->name.GetString();
        is_flat = std::all_of(first_key, first_key + strlen(first_key), ::isdigit);
    }

    if (is_flat) {
        w.RawValue(t_json.c_str(), t_json.size(), rapidjson::kObjectType);
    } else if (!doc.HasParseError() && doc.IsObject() && !dict_.path_to_id.empty()) {
        rapidjson::Document flat_doc;
        if (twin::flattenNestedTreeFiltered(doc, dict_.path_to_id, allowed_by_id_, flat_doc.GetAllocator(), flat_doc).hasError()) {
            // Fallback: write the raw nested tree if flattening fails.
            w.RawValue(t_json.c_str(), t_json.size(), rapidjson::kObjectType);
        } else {
            rapidjson::StringBuffer flat_sb;
            rapidjson::Writer<rapidjson::StringBuffer> flat_w(flat_sb);
            flat_doc.Accept(flat_w);
            w.RawValue(flat_sb.GetString(), flat_sb.GetSize(), rapidjson::kObjectType);
        }
    } else {
        w.RawValue(t_json.c_str(), t_json.size(), rapidjson::kObjectType);
    }

    w.EndObject();

    // In binary mode a raw JSON line would corrupt the frame stream, so the
    // anchor is wrapped in a control frame (db_num 0xFFFF) instead. The
    // JSON control is a transcode shim and doesn't count as an anchor line.
    Result<void> res;
    if (cfg_.format == "binary" || cfg_.format == "bin.zst") {
        res = writeBinaryControlLine(sb.GetString(), t_ts);
    } else {
        res = current_archive_->writeLine(sb.GetString());
        ++anchor_lines_;
    }
    if (res.hasError())
        return res;
    ++current_line_counter_;

    // This is the line index the footer must advertise so recovery can seek
    // straight to the most recent anchor.
    last_anchor_line_index_ = static_cast<int64_t>(current_line_counter_);
    return {};
}

void PersistenceService::finalizeArchive(int64_t t_ts_end) {
    if (!current_archive_)
        return;

    // Commit any open atomic merge window so no buffered change is lost.
    commitMergeBuffer();

    // ── Footer line ──────────────────────────────────────────────────────────
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("type");
    w.String("footer");
    w.Key("last_anchor_line");
    w.Int64(last_anchor_line_index_);
    w.Key("record_count");
    w.Uint64(static_cast<uint64_t>(current_line_counter_));
    // Restart-point inventory, counted per format so units never mix:
    // binary files report frames, JSONL files report lines.
    if (cfg_.format == "binary" || cfg_.format == "bin.zst") {
        w.Key("anchor_count");
        w.Uint64(anchor_frames_);
        w.Key("delta_count");
        w.Uint64(delta_frames_);
    } else {
        w.Key("anchor_count");
        w.Uint64(anchor_lines_);
        w.Key("delta_count");
        w.Uint64(delta_lines_);
    }
    w.EndObject();

    auto footer_res = [&]() -> Result<void> {
        if (cfg_.format == "binary" || cfg_.format == "bin.zst") {
            return writeBinaryControlLine(sb.GetString(), t_ts_end);
        }
        return current_archive_->writeLine(sb.GetString());
    }();
    if (footer_res.hasError())
        fmt::print(fg(fmt::color::red), "[persist] Footer write failed: {}\n", footer_res.error());

    auto close_res = current_archive_->close();
    if (close_res.hasError())
        fmt::print(fg(fmt::color::red), "[persist] Archive close failed: {}\n", close_res.error());

    const std::string provisional_path = current_archive_path_;
    const int64_t t_ts_start = current_archive_start_ts_;
    current_archive_.reset();
    current_archive_path_.clear();
    current_line_counter_ = 0;
    last_anchor_line_index_ = 0;

    const std::string start_file = sgrn::utils::time::timePath(t_ts_start);
    const std::string end_file = sgrn::utils::time::timePath(t_ts_end);
    const bool is_binary = (cfg_.format == "binary" || cfg_.format == "bin.zst");
    const std::string ext = is_binary ? ".bin.zst" : ".jsonl.zst";
    const std::string final_path =
        unsynced_dir_ + "/" + sgrn::utils::time::datePath(t_ts_start) + "/" + fmt::format("{}-{}{}", start_file, end_file, ext);

    // The stream is already closed (bounded flush). The remaining work — rename
    // into place + DB bookkeeping — is the only part that needs the heavy pool.
    if (pending_tasks_.load(std::memory_order_relaxed) < max_pending_) {
        ++pending_tasks_;
        asio::post(*heavy_pool_, [this, provisional_path, final_path, t_ts_start, t_ts_end]() {
            try {
                std::filesystem::rename(provisional_path, final_path);
            } catch (const std::exception& e) {
                fmt::print(fg(fmt::color::red), "[persist] Rename {} -> {} failed: {}\n", provisional_path, final_path, e.what());
            }
            if (db_) {
                (void)db_->recordPendingBatch(final_path, t_ts_start, t_ts_end);
            }
            fmt::print(fg(fmt::color::green), "[persist] Wrote {}\n", final_path);
            --pending_tasks_;
        });
    } else {
        // Queue saturated — the closed archive is still on disk under its
        // provisional name; RecoveryEngine scans *.jsonl.zst.tmp too.
        fmt::print(fg(fmt::color::red), "[persist] Finalize queue saturated — archive left at {}\n", provisional_path);
    }
}

void PersistenceService::maybeFlushBatch(int64_t t_now) {
    SGRN_RETURN_IF(!current_archive_, ;);

    const bool size_triggered = current_line_counter_ >= 2 + static_cast<size_t>(cfg_.batch_size);
    const bool time_triggered = (t_now - current_archive_start_ts_) >= static_cast<int64_t>(cfg_.batch_interval_s) * 1000;

    if (size_triggered || time_triggered) {
        finalizeArchive(t_now);
        return;
    }

    // For full_tree_with_anchor: check if a new anchor is due.
    if (cfg_.mode == "full_tree_with_anchor" && anchorDue(t_now)) {
        // Roll the archive so the fresh anchor starts a clean file. Do NOT call
        // resetAnchor() here — that would prematurely clear the counters.
        // resetAnchor() is called inside ingestFullTree() when the new anchor
        // line is actually written.
        finalizeArchive(t_now);
        needs_first_anchor_ = true;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Anchor management
// ─────────────────────────────────────────────────────────────────────────────

bool PersistenceService::anchorDue(int64_t t_now) const {

    SGRN_RETURN_IF(cfg_.mode != "full_tree_with_anchor", false);
    SGRN_RETURN_IF(needs_first_anchor_, false); // we're already waiting for the first anchor

    const bool time_due = cfg_.anchor_interval_s > 0 && (t_now - last_anchor_ts_) >= static_cast<int64_t>(cfg_.anchor_interval_s) * 1000;

    const bool count_due = cfg_.anchor_change_count > 0 && changes_since_anchor_ >= cfg_.anchor_change_count;

    return time_due || count_due;
}

void PersistenceService::resetAnchor(int64_t t_now) {
    last_anchor_ts_ = t_now;
    changes_since_anchor_ = 0;
    needs_first_anchor_ = false;
}

} // namespace sgrn::gateway::database
