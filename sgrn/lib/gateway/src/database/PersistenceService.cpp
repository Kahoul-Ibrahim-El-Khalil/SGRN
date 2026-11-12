#include <sgrn/utils/json-dom.hpp>
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
#include <stdexcept>
#include <string>
#include <string_view>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>

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

sgrn::json::dom::StringBuffer& threadLocalLeafJsonBuffer() {
    thread_local sgrn::json::dom::StringBuffer buffer;
    return buffer;
}

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
    schema_store_ = t_schema_store;

    // Render the binary schema payload once, up front: archives open lazily
    // (long after configure returns), and the header needs the bytes then.
    if (t_schema_store) {
        if (auto bin_res = t_schema_store->toBinary(); !bin_res.hasError()) {
            schema_binary_ = std::move(bin_res).value();
            has_binary_schema_ = true;
        } else {
            fmt::print(fg(fmt::color::yellow), "[persist] Binary schema encode failed — falling back to JSON header.\n");
        }
    }

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
    if (!active_.exchange(false, std::memory_order_acq_rel))
        return;
    if (broker_sub_id_ != 0) {
        TelemetryBroker::instance().unsubscribe(broker_sub_id_);
        broker_sub_id_ = 0;
    }
    // Close any archive still open so the WAL on disk ends with a valid footer.
    if (current_archive_) {
        finalizeArchive(sgrn::utils::time::nowMilliseconds());
    }
    // Drain pending tasks so background pool threads don't touch this object after destruction
    while (pending_tasks_.load(std::memory_order_acquire) > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
        // DBs to snapshot for this event: the single-DB leaf path (`db`)
        // plus the multi-DB DeltaSnapshot batch (`dirty_dbs`, populated by
        // GatewayApplication::wireTelemetry from the same dirty batch the
        // JSON snapshot was built from). A DeltaSnapshot leaves `db` at 0,
        // so without dirty_dbs the per-DB branch below never runs for it.
        std::vector<uint16_t> dbs;
        if (t_event.db > 0)
            dbs.push_back(t_event.db);
        for (uint16_t db : t_event.dirty_dbs) {
            if (db == 0)
                continue;
            if (std::find(dbs.begin(), dbs.end(), db) == dbs.end())
                dbs.push_back(db);
        }
        if (!dbs.empty()) {
            const uint64_t ts = t_event.timestamp > 0 ? t_event.timestamp : static_cast<uint64_t>(t_now);
            if (read_db_fn_) {
                for (uint16_t db : dbs) {
                    if (!current_archive_) {
                        (void)openNewArchive(t_now);
                    }
                    if (current_archive_) {
                        writeBinaryDbImage(db, ts, t_now);
                    }
                }
            } else if (current_archive_ && dbs.size() == 1 && t_event.typed_leaf.bytes && !t_event.typed_leaf.bytes->empty()) {
                // Degraded fallback (no full-DB reader wired): anchors the
                // per-leaf slice as-is, CRC'd over the stored bytes. Every
                // data frame self-identifies as delta or anchor; this one is
                // verifiable but only exact for offset-0 leaves (slices carry
                // no offset), and it bypasses dedup/keyframe tracking.
                writeBinaryAnchor(dbs.front(), *t_event.typed_leaf.bytes, static_cast<int64_t>(ts));
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
    // PersistenceService must:    //   1. Parse the JSON (Glaze JSON DOM DOM construction)
    //   2. Extract individual field paths and values
    //   3. Apply namespace filtering per field
    //   4. Re-serialize each field individually as a JSONL delta line
    //
    // The actual zstd compression now happens incrementally inside the writer
    // (bounded by ZSTD_CStreamOutSize()), so a full-batch compress + envelope
    // pass is gone: per-event cost is O(line size) in both CPU and memory.
    // ─────────────────────────────────────────────────────────────────────────

    // For the other two modes we apply namespace filtering and atomic merging.
    // Shared lazy DOM: at most one parse per event across all subscribers
    // (WebSocket filtering shares it); null mirrors the old parse failure.
    const sgrn::json::dom::Document* p_doc = t_event.parsedJson();
    if (!p_doc)
        return;
    const sgrn::json::dom::Document& doc = *p_doc;

    // Iterate top-level keys (DB names) → fields recursively.
    // Delta payloads arrive as nested, flat numeric-ID, or legacy dotted JSON.
    // Keep one mutable path buffer for the recursive tree walk and resolve
    // numeric IDs directly instead of converting them back to paths and
    // hashing those paths a second time.
    auto processLeafId = [&](twin::LeafId t_id, const sgrn::json::dom::Value& t_value) {
        if (!passesFilter(t_id))
            return;

        auto& buffer = threadLocalLeafJsonBuffer();
        const auto encoded = sgrn::json::writeTo(buffer.mutableString(), t_value.rawValue());
        if (encoded.hasError())
            return;

        mergeOrFlushEvent(t_id, std::string_view{buffer.getString(), buffer.size()}, t_now);
    };

    auto processLeafPath = [&](const std::string& t_path, const sgrn::json::dom::Value& t_value) {
        const auto id_it = dict_.path_to_id.find(t_path);
        if (id_it != dict_.path_to_id.end())
            processLeafId(id_it->second, t_value);
    };

    // getDeltaSnapshot() may contain nested leaves (e.g. ReactorCore.rods.position_pct),
    // so walk the JSON tree with one path string rather than allocating a new
    // concatenated string at every recursion level.
    std::string path;
    path.reserve(128);
    auto walkFields = [&](auto&& t_self, const sgrn::json::dom::Value& t_obj) -> void {
        for (auto f_it = t_obj.memberBegin(); f_it != t_obj.memberEnd(); ++f_it) {
            const size_t previous_size = path.size();
            if (!path.empty())
                path.push_back('.');
            path.append(f_it->name.getString(), f_it->name.getStringLength());

            if (f_it->value.is_object()) {
                t_self(t_self, f_it->value);
            } else {
                processLeafPath(path, f_it->value);
            }
            path.resize(previous_size);
        }
    };

    auto isDigits = [](std::string_view t_s) {
        return !t_s.empty() && t_s.size() <= 10 &&
               std::all_of(t_s.begin(), t_s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    };

    for (auto db_it = doc.memberBegin(); db_it != doc.memberEnd(); ++db_it) {
        if (db_it->value.is_object()) {
            path.assign(db_it->name.getString(), db_it->name.getStringLength());
            walkFields(walkFields, db_it->value);
            path.clear();
            continue;
        }

        // Flat leaf straight at top level: numeric ID or dotted path.
        const std::string_view key{db_it->name.getString(), db_it->name.getStringLength()};
        if (isDigits(key)) {
            twin::LeafId id{};
            const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), id);
            if (error == std::errc{} && end == key.data() + key.size() && id < dict_.path_by_id.size() && !dict_.path_by_id[id].empty()) {
                processLeafId(id, db_it->value);
            }
        } else {
            path.assign(key);
            processLeafPath(path, db_it->value);
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

    const bool anchor_is_flat = !t_flat_tree_json.empty();
    const std::string& anchor_json = anchor_is_flat ? t_flat_tree_json : t_full_tree_json;
    if (auto res = writeAnchorLine(anchor_json, t_now, anchor_is_flat); res.hasError()) {
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
        // #TRANSIENT leaves never persist (JSONL deltas and anchors share
        // this filter; binary WAL keeps full-DB images for resync).
        // Runs at rebuild time only — the hot path stays a bool lookup.
        if (passes && schema_store_) {
            const size_t dot = path.find('.');
            if (dot != std::string::npos) {
                if (auto loc = schema_store_->findField(
                        std::string_view(path.data(), dot), std::string_view(path.data() + dot + 1, path.size() - dot - 1));
                    loc.has_value() && loc->field && loc->field->is_transient) {
                    passes = false;
                }
            }
        }
        allowed_by_id_[id] = passes;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Atomic merge window
// ─────────────────────────────────────────────────────────────────────────────

void PersistenceService::mergeOrFlushEvent(twin::LeafId t_id, std::string_view t_value_json, int64_t t_now) {
    const uint32_t window = cfg_.atomic_window_ms;

    if (window == 0) {
        // t_value_json was produced by the Glaze serializer above. Splice the
        // trusted fragment directly instead of parsing and serializing it again.
        const std::string id = std::to_string(t_id);
        std::string record;
        record.reserve(t_value_json.size() + id.size() + 64);
        record.append(R"({"type":"delta","ts":)");
        record.append(std::to_string(t_now));
        record.append(R"(,"changes":{")");
        record.append(id);
        record.append(R"(":)");
        record.append(t_value_json);
        record.append("}}");
        (void)writeDeltaLine(record);
        return;
    }

    // If the merge buffer is open and within the atomic window, accumulate.
    if (merge_buf_.open_ts != 0 && (t_now - merge_buf_.open_ts) <= static_cast<int64_t>(window)) {
        merge_buf_.fields[t_id] = std::string(t_value_json);
        return;
    }

    // Commit whatever was in the buffer before opening a new window.
    if (merge_buf_.open_ts != 0)
        commitMergeBuffer();

    merge_buf_.open_ts = t_now;
    merge_buf_.fields[t_id] = std::string(t_value_json);
}

void PersistenceService::commitMergeBuffer() {
    if (merge_buf_.open_ts == 0 || merge_buf_.fields.empty())
        return;

    // Values in merge_buf_ are already compact, valid JSON fragments from
    // the Glaze serializer. Avoid reparsing every leaf through Writer::RawValue.
    size_t reserve_size = 64;
    constexpr size_t kLeafIdMaxDigits = std::numeric_limits<twin::LeafId>::digits10 + 1;
    for (const auto& [id, value] : merge_buf_.fields) {
        (void)id;
        reserve_size += kLeafIdMaxDigits + value.size() + 5;
    }

    std::string record;
    record.reserve(reserve_size);
    record.append(R"({"type":"delta","ts":)");
    record.append(std::to_string(merge_buf_.open_ts));
    record.append(R"(,"changes":{)");
    bool first = true;
    for (const auto& [id, value] : merge_buf_.fields) {
        if (!first)
            record.push_back(',');
        first = false;
        record.push_back('"');
        record.append(std::to_string(id));
        record.append(R"(":)");
        record.append(value);
    }
    record.append("}}");

    (void)writeDeltaLine(record);

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
        // Binary Header Frame: "SGRN" (4B) + Version (2B) + Schema Length (4B) + Schema payload.
        // Version 4+ carries the binary schema encoding (SchemaSerializer);
        // version 3 is the legacy JSON-text schema, still written when no
        // schema store was provided at configure time. Readers accept both.
        const bool use_binary_schema = has_binary_schema_;
        const uint16_t ver = (use_binary_schema || schema_json_.empty()) ? kBinaryWalVersion : 3;
        const std::string& schema_payload = use_binary_schema ? schema_binary_ : schema_json_;
        char magic[4] = {'S', 'G', 'R', 'N'};
        uint32_t schema_len = static_cast<uint32_t>(schema_payload.size());

        (void)current_archive_->writeRaw(magic, 4);
        (void)current_archive_->writeRaw(&ver, sizeof(ver));
        (void)current_archive_->writeRaw(&schema_len, sizeof(schema_len));
        if (schema_len > 0) {
            (void)current_archive_->writeRaw(schema_payload.data(), schema_len);
        }
        ++current_line_counter_;
    } else {
        // ── Line 1: schema ───────────────────────────────────────────────────────
        sgrn::json::dom::StringBuffer sb;
        sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> w(sb);
        w.startObject();
        w.key("type");
        w.string("schema");
        w.key("schema");
        if (schema_json_.empty()) {
            w.nullValue();
        } else {
            w.rawJson(schema_json_.c_str(), schema_json_.size(), sgrn::json::dom::Type::Object);
        }
        w.endObject();

        auto schema_res = current_archive_->writeLine(sb.getString());
        if (schema_res.hasError()) {
            current_archive_.reset();
            current_archive_path_.clear();
            return fmt::format("PersistenceService: schema line write failed: {}", schema_res.error());
        }
        ++current_line_counter_;
    }

    // ── Line 2: dictionary ───────────────────────────────────────────────────
    sgrn::json::dom::StringBuffer dsb;
    sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> dw(dsb);
    dw.startObject();
    dw.key("type");
    dw.string("dictionary");
    dw.key("leaves");
    dw.startArray();
    for (size_t id = 0; id < dict_.path_by_id.size(); ++id) {
        const auto& path = dict_.path_by_id[id];
        dw.startObject();
        dw.key("id");
        dw.uint32(static_cast<uint32_t>(id));
        dw.key("path");
        dw.string(path.c_str(), static_cast<sgrn::json::dom::SizeType>(path.size()));
        dw.endObject();
    }
    dw.endArray();
    dw.endObject();

    auto dict_res = [&]() -> Result<void> {
        if (is_binary) {
            return writeBinaryControlLine(dsb.getString(), t_now);
        }
        return current_archive_->writeLine(dsb.getString());
    }();
    if (dict_res.hasError()) {
        current_archive_.reset();
        current_archive_path_.clear();
        return fmt::format("PersistenceService: dictionary line write failed: {}", dict_res.error());
    }
    ++current_line_counter_;

    // ── Line 3: manifest ─────────────────────────────────────────────────────
    sgrn::json::dom::StringBuffer msb;
    sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> mw(msb);
    mw.startObject();
    mw.key("type");
    mw.string("manifest");
    mw.key("start_time");
    const std::string ts_str = sgrn::utils::time::iso8601Timestamp(t_now);
    mw.string(ts_str.c_str(), static_cast<sgrn::json::dom::SizeType>(ts_str.size()));
    mw.key("mode");
    mw.string(cfg_.mode.c_str(), static_cast<sgrn::json::dom::SizeType>(cfg_.mode.size()));
    mw.key("namespaces");
    mw.startArray();
    for (const auto& ns : cfg_.namespaces) {
        mw.string(ns.c_str(), static_cast<sgrn::json::dom::SizeType>(ns.size()));
    }
    mw.endArray();
    // Encoding policy for this file, so readers know the restart-point
    // cadence without consulting gateway.json. Binary keyframes follow
    // anchor_interval_s when set (see configure()), JSONL anchors follow
    // the mode's own anchor triggers.
    if (is_binary) {
        mw.key("keyframe_interval_s");
        mw.int64(binary_keyframe_interval_ms_ / 1000);
        mw.key("delta_ratio");
        mw.float64(kBinaryDeltaMaxRatio);
    } else {
        mw.key("anchor_interval_s");
        mw.uint32(cfg_.anchor_interval_s);
        mw.key("anchor_change_count");
        mw.uint32(cfg_.anchor_change_count);
    }
    mw.endObject();

    auto manifest_res = [&]() -> Result<void> {
        if (is_binary) {
            return writeBinaryControlLine(msb.getString(), t_now);
        }
        return current_archive_->writeLine(msb.getString());
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
    PROPAGATE_ERROR_IF(current_archive_->writeRaw(&ts, sizeof(ts)));
    PROPAGATE_ERROR_IF(current_archive_->writeRaw(&db_num, sizeof(db_num)));
    PROPAGATE_ERROR_IF(current_archive_->writeRaw(&len, sizeof(len)));
    if (len > 0) {
        PROPAGATE_ERROR_IF(current_archive_->writeRaw(t_json.data(), len));
    }
    return {};
}

void PersistenceService::writeBinaryDbImage(uint16_t t_db, uint64_t t_ts, int64_t t_now) {
    RETURN_IF(!current_archive_ || !read_db_fn_, ;);

    auto full_res = read_db_fn_(t_db);
    RETURN_IF(full_res.hasError() || full_res.value().empty(), ;);

    auto& last = last_db_bytes_[t_db];
    RETURN_IF(last == full_res.value(), ;);

    const auto& full = full_res.value();
    const auto key_it = last_keyframe_ts_.find(t_db);
    const bool need_keyframe =
        last.empty() || key_it == last_keyframe_ts_.end() || (t_now - key_it->second) >= binary_keyframe_interval_ms_;

    bool wrote_delta = false;
    if (!need_keyframe && last.size() == full.size()) {
        const auto runs = diffRuns(last, full);
        uint64_t delta_bytes = 2; // embedded db_num
        for (const auto& run : runs)
            delta_bytes += 8 + run.len;
        if (!runs.empty() && delta_bytes <= 0xFFFFFFFFu && delta_bytes < static_cast<double>(full.size()) * kBinaryDeltaMaxRatio) {
            const uint16_t marker = kDeltaFrameDbNum;
            const uint32_t total = static_cast<uint32_t>(delta_bytes);
            (void)current_archive_->writeRaw(&t_ts, sizeof(t_ts));
            (void)current_archive_->writeRaw(&marker, sizeof(marker));
            (void)current_archive_->writeRaw(&total, sizeof(total));
            (void)current_archive_->writeRaw(&t_db, sizeof(t_db));
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
        writeBinaryAnchor(t_db, full, static_cast<int64_t>(t_ts));
        // Wall clock on both sides: the frame carries the event timestamp,
        // but keyframe ageing is measured in wall time (event clocks may lag).
        last_keyframe_ts_[t_db] = t_now;
    }
    last = full;
    ++current_line_counter_;
    maybeFlushBatch(t_now);
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

Result<void> PersistenceService::writeAnchorLine(const std::string& t_json, int64_t t_ts, bool t_data_is_flat) {
    if (!current_archive_) {
        if (auto res = openNewArchive(sgrn::utils::time::nowMilliseconds()); res.hasError())
            return res.error();
    }

    std::string_view data_json{t_json};
    sgrn::json::dom::StringBuffer flattened_buffer;

    // Snapshots supplied through the explicit flat argument need no parse.
    // For legacy callers that pass a flat object as the first argument, retain
    // the old first-key check while avoiding the second parse in RawValue.
    if (!t_data_is_flat) {
        sgrn::json::dom::Document doc;
        doc.parse(t_json.data(), t_json.size());
        if (doc.hasParseError() || !doc.is_object()) {
            return fmt::format("Failed to parse nested anchor JSON: {}", doc.errorMessage());
        }

        bool already_flat = false;
        if (doc.memberCount() > 0) {
            const auto first = doc.memberBegin();
            const std::string_view first_key{first->name.getString(), first->name.getStringLength()};
            already_flat = !first_key.empty() &&
                           std::all_of(first_key.begin(), first_key.end(), [](unsigned char t_char) { return std::isdigit(t_char) != 0; });
        }

        if (!already_flat && !dict_.path_to_id.empty()) {
            sgrn::json::dom::Document flat_doc;
            if (!twin::flattenNestedTreeFiltered(doc, dict_.path_to_id, allowed_by_id_, flat_doc.allocator(), flat_doc).hasError()) {
                sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> flat_writer(flattened_buffer);
                if (!flat_doc.accept(flat_writer))
                    return "Failed to serialize flattened anchor JSON";
                data_json = std::string_view{flattened_buffer.getString(), flattened_buffer.size()};
            }
            // Preserve the previous fallback: if flattening fails, archive the
            // original nested tree rather than dropping the anchor.
        }
    }

    // The data fragment is produced internally by Glaze. Compose the fixed
    // envelope around it directly; Writer::RawValue would parse it again.
    std::string record;
    record.reserve(data_json.size() + 64);
    record.append(R"({"type":"anchor","ts":)");
    record.append(std::to_string(t_ts));
    record.append(R"(,"data":)");
    record.append(data_json);
    record.push_back('}');

    // In binary mode a raw JSON line would corrupt the frame stream, so the
    // anchor is wrapped in a control frame (db_num 0xFFFF) instead. The
    // JSON control is a transcode shim and doesn't count as an anchor line.
    Result<void> res;
    if (cfg_.format == "binary" || cfg_.format == "bin.zst") {
        res = writeBinaryControlLine(record, t_ts);
    } else {
        res = current_archive_->writeLine(record);
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
    sgrn::json::dom::StringBuffer sb;
    sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> w(sb);
    w.startObject();
    w.key("type");
    w.string("footer");
    w.key("last_anchor_line");
    w.int64(last_anchor_line_index_);
    w.key("record_count");
    w.uint64(static_cast<uint64_t>(current_line_counter_));
    // Restart-point inventory, counted per format so units never mix:
    // binary files report frames, JSONL files report lines.
    if (cfg_.format == "binary" || cfg_.format == "bin.zst") {
        w.key("anchor_count");
        w.uint64(anchor_frames_);
        w.key("delta_count");
        w.uint64(delta_frames_);
    } else {
        w.key("anchor_count");
        w.uint64(anchor_lines_);
        w.key("delta_count");
        w.uint64(delta_lines_);
    }
    w.endObject();

    auto footer_res = [&]() -> Result<void> {
        if (cfg_.format == "binary" || cfg_.format == "bin.zst") {
            return writeBinaryControlLine(sb.getString(), t_ts_end);
        }
        return current_archive_->writeLine(sb.getString());
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

    auto do_finalize = [db = db_, provisional_path, final_path, t_ts_start, t_ts_end]() {
        try {
            std::error_code ec;
            std::filesystem::create_directories(std::filesystem::path(final_path).parent_path(), ec);
            std::filesystem::rename(provisional_path, final_path, ec);
            if (ec) {
                // If rename failed because destination exists, attempt replace
                std::filesystem::remove(final_path, ec);
                ec.clear();
                std::filesystem::rename(provisional_path, final_path, ec);
            }
            if (ec) {
                fmt::print(fg(fmt::color::red), "[persist] Rename {} -> {} failed: {}\n", provisional_path, final_path, ec.message());
            } else {
                fmt::print(fg(fmt::color::green), "[persist] Wrote {}\n", final_path);
            }
        } catch (const std::exception& e) {
            fmt::print(fg(fmt::color::red), "[persist] Rename {} -> {} failed: {}\n", provisional_path, final_path, e.what());
        }
        if (db) {
            (void)db->recordPendingBatch(final_path, t_ts_start, t_ts_end);
        }
    };

    if (heavy_pool_ && pending_tasks_.load(std::memory_order_relaxed) < max_pending_) {
        ++pending_tasks_;
        asio::post(*heavy_pool_, [this, do_finalize]() {
            do_finalize();
            --pending_tasks_;
        });
    } else {
        do_finalize();
    }
}

void PersistenceService::maybeFlushBatch(int64_t t_now) {
    RETURN_IF(!current_archive_, ;);

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

    RETURN_IF(cfg_.mode != "full_tree_with_anchor", false);
    RETURN_IF(needs_first_anchor_, false); // we're already waiting for the first anchor

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
