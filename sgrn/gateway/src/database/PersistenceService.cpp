// ─────────────────────────────────────────────────────────────────────────────
// PersistenceService.cpp
//
// Independent local historian / archiver service.
// See PersistenceService.hpp for full architectural documentation.
// ─────────────────────────────────────────────────────────────────────────────

#include <sgrn/gateway/core/GlobalContext.hpp>
#include <sgrn/gateway/database/PersistenceService.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/json.hpp>
#include <sgrn/utils/time.hpp>

#include <fmt/color.h>
#include <fmt/core.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace sgrn::gateway::database
{

using namespace sgrn::gateway::core;
using namespace sgrn::gateway::config;

// ─────────────────────────────────────────────────────────────────────────────
// Construction / configuration
// ─────────────────────────────────────────────────────────────────────────────

PersistenceService::PersistenceService(asio::thread_pool* tp_heavy_pool)
    : heavy_pool_(tp_heavy_pool) {
}

PersistenceService::~PersistenceService() {
    stop();
}

sgrn::Result<void> PersistenceService::configure(
    const PersistenceConfig& t_cfg, const std::string& t_state_dir, std::shared_ptr<GatewayDatabase> tsp_db) {
    if (!t_cfg.enabled) {
        fmt::print(fg(fmt::color::yellow), "[persist] Persistence disabled — skipping.\n");
        return {};
    }

    cfg_ = t_cfg;
    state_dir_ = t_state_dir;
    unsynced_dir_ = t_state_dir + "/unsynced";
    db_ = std::move(tsp_db);

    try {
        std::filesystem::create_directories(unsynced_dir_);
    } catch (const std::exception& e) {
        return fmt::format("PersistenceService: cannot create directories: {}", e.what());
    }

    const int64_t t_now = sgrn::utils::time::nowMilliseconds();
    last_flush_ts_ = t_now;
    last_anchor_ts_ = t_now;

    // Subscribe to the global TelemetryBroker.
    // All callbacks arrive on the io_context, so batch state is single-threaded.
    broker_sub_id_ = TelemetryBroker::instance().subscribe([this](const TelemetryEvent& t_ev) { onTelemetryEvent(t_ev); });

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
}

// ─────────────────────────────────────────────────────────────────────────────
// TelemetryBroker callback
// ─────────────────────────────────────────────────────────────────────────────

void PersistenceService::onTelemetryEvent(const TelemetryEvent& t_event) {
    if (!active_.load(std::memory_order_relaxed))
        return;
    if (pending_tasks_.load() >= max_pending_)
        return;

    const int64_t t_now = sgrn::utils::time::nowMilliseconds();

    // ── FullSnapshot: always write an anchor regardless of mode ──────────────
    if (t_event.type == EventType::FullSnapshot && t_event.json_value) {
        const std::string& json = *t_event.json_value;
        if (!json.empty() && json != "{}") {
            ingestFullTree(json);
        }
        return;
    }

    // ── DeltaSnapshot ────────────────────────────────────────────────────────
    if (t_event.type != EventType::DeltaSnapshot)
        return;
    if (!t_event.json_value || t_event.json_value->empty() || *t_event.json_value == "{}")
        return;

    // In full_tree mode every delta is treated as a full tree snapshot.
    if (cfg_.mode == "full_tree") {
        ingestFullTree(*t_event.json_value);
        return;
    }

    // ── PERFORMANCE NOTE: JSON Parsing and Filtering ─────────────────────────
    // The TelemetryBroker broadcasts the SAME shared_ptr<string> to all subscribers.
    // This JSON was already serialized once by PlcState::getDeltaSnapshot().
    // However, PersistenceService must:
    //   1. Parse the JSON (RapidJSON DOM construction)
    //   2. Extract individual field paths and values
    //   3. Apply namespace filtering per field
    //   4. Re-serialize each field individually for batching
    //   5. Compress the batch with zstd
    //
    // This is EXPENSIVE but necessary for:
    //   - Field-level namespace filtering (only persist what you need)
    //   - Atomic merge windows (batch multiple deltas into one record)
    //   - Compression (zstd dominates the cost, not parsing)
    //
    // The parsing overhead is ~25-30% of total persistence cost. The compression
    // step dominates. This runs on heavy_pool_ to avoid blocking the io_context.
    // ─────────────────────────────────────────────────────────────────────────

    // For the other two modes we apply namespace filtering and atomic merging.
    // Parse the incoming delta JSON to extract individual field paths+values.
    rapidjson::Document doc;
    doc.Parse(t_event.json_value->c_str());
    if (doc.HasParseError() || !doc.IsObject())
        return;

    // Iterate top-level keys (DB names) → fields
    for (auto db_it = doc.MemberBegin(); db_it != doc.MemberEnd(); ++db_it) {
        const std::string db_name = db_it->name.GetString();
        if (!db_it->value.IsObject())
            continue;
        for (auto f_it = db_it->value.MemberBegin(); f_it != db_it->value.MemberEnd(); ++f_it) {
            const std::string t_path = db_name + "." + f_it->name.GetString();
            if (!passesFilter(t_path))
                continue;

            rapidjson::StringBuffer sb;
            rapidjson::Writer<rapidjson::StringBuffer> w(sb);
            f_it->value.Accept(w);

            mergeOrFlushEvent(t_path, sb.GetString(), t_now);
        }
    }

    maybeFlushBatch(t_now);
}

// ─────────────────────────────────────────────────────────────────────────────
// Public ingestion of full-tree snapshots
// ─────────────────────────────────────────────────────────────────────────────

void PersistenceService::ingestFullTree(const std::string& t_full_tree_json) {
    const int64_t t_now = sgrn::utils::time::nowMilliseconds();

    // Flush any pending delta batch before writing a full tree, so that the
    // archive timeline is consistent.
    if (!batch_records_.empty())
        flushBatch(t_now);

    const bool t_is_anchor = (cfg_.mode == "full_tree_with_anchor");
    writeFullTreeFile(t_full_tree_json, t_now, t_is_anchor);

    if (t_is_anchor)
        resetAnchor(t_now);
}

// ─────────────────────────────────────────────────────────────────────────────
// Namespace filter
// ─────────────────────────────────────────────────────────────────────────────

bool PersistenceService::passesFilter(const std::string& t_path) const {
    if (cfg_.namespaces.empty())
        return true;
    for (const auto& ns : cfg_.namespaces) {
        if (t_path.rfind(ns, 0) == 0) // starts_with
            return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Atomic merge window
// ─────────────────────────────────────────────────────────────────────────────

void PersistenceService::mergeOrFlushEvent(const std::string& t_path, const std::string& t_value_json, int64_t t_now) {
    const uint32_t window = cfg_.atomic_window_ms;

    if (window == 0) {
        // Merging disabled — emit a standalone record immediately.
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        w.StartObject();
        w.Key("timestamp");
        const std::string t_ts = sgrn::utils::time::iso8601Timestamp(t_now);
        w.String(t_ts.c_str(), static_cast<rapidjson::SizeType>(t_ts.size()));
        w.Key("changes");
        w.StartObject();
        w.Key(t_path.c_str(), static_cast<rapidjson::SizeType>(t_path.size()));
        w.RawValue(t_value_json.c_str(), t_value_json.size(), rapidjson::kObjectType);
        w.EndObject();
        w.EndObject();
        appendRecord(sb.GetString());
        return;
    }

    // If the merge buffer is open and within the atomic window, accumulate.
    if (merge_buf_.open_ts != 0 && (t_now - merge_buf_.open_ts) <= static_cast<int64_t>(window)) {
        merge_buf_.fields[t_path] = t_value_json;
        return;
    }

    // Commit whatever was in the buffer before opening a new window.
    if (merge_buf_.open_ts != 0)
        commitMergeBuffer();

    merge_buf_.open_ts = t_now;
    merge_buf_.fields[t_path] = t_value_json;
}

void PersistenceService::commitMergeBuffer() {
    if (merge_buf_.open_ts == 0 || merge_buf_.fields.empty())
        return;

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("timestamp");
    const std::string t_ts = sgrn::utils::time::iso8601Timestamp(merge_buf_.open_ts);
    w.String(t_ts.c_str(), static_cast<rapidjson::SizeType>(t_ts.size()));
    w.Key("changes");
    w.StartObject();
    for (const auto& [t_path, val] : merge_buf_.fields) {
        w.Key(t_path.c_str(), static_cast<rapidjson::SizeType>(t_path.size()));
        w.RawValue(val.c_str(), val.size(), rapidjson::kObjectType);
    }
    w.EndObject();
    w.EndObject();

    appendRecord(sb.GetString());

    merge_buf_.open_ts = 0;
    merge_buf_.fields.clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// Batch accumulation
// ─────────────────────────────────────────────────────────────────────────────

void PersistenceService::appendRecord(const std::string& t_record_json) {
    if (batch_records_.empty())
        batch_start_ts_ = sgrn::utils::time::nowMilliseconds();

    batch_records_.push_back(t_record_json);
    ++changes_since_anchor_;
}

void PersistenceService::maybeFlushBatch(int64_t t_now) {
    const bool size_triggered = !batch_records_.empty() && batch_records_.size() >= static_cast<size_t>(cfg_.batch_size);

    const bool time_triggered = !batch_records_.empty() && (t_now - last_flush_ts_) >= static_cast<int64_t>(cfg_.batch_interval_s) * 1000;

    if (size_triggered || time_triggered)
        flushBatch(t_now);

    // For full_tree_with_anchor: check if a new anchor is due.
    if (cfg_.mode == "full_tree_with_anchor" && anchorDue(t_now)) {
        // Flush any pending delta batch first so the archive timeline is consistent.
        if (!batch_records_.empty())
            flushBatch(t_now);
        // Signal that a fresh FullSnapshot is needed. Do NOT call resetAnchor() here —
        // that would prematurely clear the counters. resetAnchor() is called inside
        // ingestFullTree() when the actual anchor snapshot is written.
        needs_first_anchor_ = true;
    }
}

void PersistenceService::flushBatch(int64_t t_ts_end) {
    if (batch_records_.empty())
        return;

    // Commit any open merge window before flushing.
    commitMergeBuffer();

    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("type");
    w.String("DELTA_BATCH");
    w.Key("size");
    w.Uint(static_cast<unsigned int>(batch_records_.size()));
    w.Key("data");
    w.StartArray();
    for (const auto& rec : batch_records_)
        w.RawValue(rec.c_str(), rec.size(), rapidjson::kObjectType);
    w.EndArray();
    w.EndObject();

    std::string envelope = sb.GetString();
    batch_records_.clear();

    const int64_t t_ts_start = batch_start_ts_;
    last_flush_ts_ = t_ts_end;
    batch_start_ts_ = 0;

    // Build file path: unsynced/<YYYY-MM-DD>/<start_time>-<end_time>.json.zst
    const std::string date_dir = sgrn::utils::time::datePath(t_ts_start);
    const std::string start_file = sgrn::utils::time::timePath(t_ts_start);
    const std::string end_file = sgrn::utils::time::timePath(t_ts_end);
    const std::string filename = fmt::format("{}-{}.json.zst", start_file, end_file);
    const std::string target_dir = unsynced_dir_ + "/" + date_dir;
    std::filesystem::create_directories(target_dir);
    const std::string t_filepath = target_dir + "/" + filename;

    ++pending_tasks_;
    asio::post(*heavy_pool_, [this, envelope = std::move(envelope), t_filepath, t_ts_start, t_ts_end]() {
        writeBatchFile(envelope, t_filepath, t_ts_start, t_ts_end, cfg_.zstd_level);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// File I/O (runs on heavy_pool_)
// ─────────────────────────────────────────────────────────────────────────────

void PersistenceService::writeFullTreeFile(const std::string& t_tree_json, int64_t t_ts, bool t_is_anchor) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> w(sb);
    w.StartObject();
    w.Key("type");
    w.String(t_is_anchor ? "ANCHOR" : "FULL_TREE");
    w.Key("timestamp");
    const std::string ts_str = sgrn::utils::time::iso8601Timestamp(t_ts);
    w.String(ts_str.c_str(), static_cast<rapidjson::SizeType>(ts_str.size()));
    w.Key("data");
    w.RawValue(t_tree_json.c_str(), t_tree_json.size(), rapidjson::kObjectType);
    w.EndObject();

    const std::string envelope = sb.GetString();
    const std::string date_dir = sgrn::utils::time::datePath(t_ts);
    const std::string time_file = sgrn::utils::time::timePath(t_ts);
    const std::string suffix = t_is_anchor ? "-anchor" : "";
    const std::string target_dir = unsynced_dir_ + "/" + date_dir;
    std::filesystem::create_directories(target_dir);
    const std::string t_filepath = target_dir + "/" + time_file + suffix + ".json.zst";

    const uint8_t level = t_is_anchor ? cfg_.anchor_zstd_level : cfg_.zstd_level;

    ++pending_tasks_;
    asio::post(*heavy_pool_,
        [this, envelope = std::move(envelope), t_filepath, t_ts, level]() { writeBatchFile(envelope, t_filepath, t_ts, t_ts, level); });
}

void PersistenceService::writeBatchFile(
    const std::string& t_envelope_json, const std::string& t_filepath, int64_t t_ts_start, int64_t t_ts_end, uint8_t t_compression_level) {
    try {
        auto comp_res = sgrn::utils::compressStringZstd(t_envelope_json, t_compression_level);
        if (comp_res.hasError()) {
            fmt::print(fg(fmt::color::red), "[persist] Compression failed: {}\n", comp_res.error());
            --pending_tasks_;
            return;
        }

        std::ofstream ofs(t_filepath, std::ios::binary);
        if (!ofs) {
            fmt::print(fg(fmt::color::red), "[persist] Cannot open for write: {}\n", t_filepath);
            --pending_tasks_;
            return;
        }
        ofs << comp_res.value();
        ofs.close();

        if (db_) {
            (void)db_->recordPendingBatch(t_filepath, t_ts_start, t_ts_end);
        }

        fmt::print(fg(fmt::color::green), "[persist] Wrote {}\n", t_filepath);
    } catch (const std::exception& e) {
        fmt::print(fg(fmt::color::red), "[persist] Write error: {}\n", e.what());
    }
    --pending_tasks_;
}

// ─────────────────────────────────────────────────────────────────────────────
// Anchor management
// ─────────────────────────────────────────────────────────────────────────────

bool PersistenceService::anchorDue(int64_t t_now) const {
    if (cfg_.mode != "full_tree_with_anchor")
        return false;
    if (needs_first_anchor_)
        return false; // we're already waiting for the first anchor

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
