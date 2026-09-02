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
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace sgrn::gateway::database
{

/**
 * @brief Independent local historian / JSONL-WAL archiver service.
 *
 * ARCHITECTURAL ROLE
 * ------------------
 * PersistenceService is the *local persistence layer* in the gateway: it is the
 * single writer of the write-ahead log (WAL) that doubles as both the historian
 * and the boot-recovery source of the digital twin.
 *
 *   - PersistenceService subscribes to TelemetryBroker and writes
 *     .jsonl.zst WAL files to `state/unsynced/` on disk.
 *
 *   - DatastoreBridge is a dumb uploader: it polls `pending_batches` in
 *     GatewayDatabase and uploads the files when connectivity is available,
 *     then moves them into `state/synced/`.
 *
 *   - At boot, RecoveryEngine scans unsynced+synced for the newest archive and
 *     rebuilds the twin from its last anchor + deltas. PersistenceService does
 *     NOT read anything itself.
 *
 * ARCHIVE MODES  (configured via PersistenceConfig::mode)
 * -------------------------------------------------------
 *  "full_tree"
 *     Every delta is written as a full-snapshot "anchor" line.
 *     High storage footprint; easy to consume downstream.
 *
 *  "full_tree_with_anchor"
 *     A full "anchor" line is written on startup and then again when:
 *       * anchor_interval_s seconds have elapsed  (time-based, 0 = disabled)
 *       * anchor_change_count records have accumulated (count-based, 0 = disabled)
 *     Between anchors only changed fields are archived as "delta" lines.
 *
 *  "changes_with_timestamp"
 *     Only changed fields are archived as "delta" lines.  Multiple
 *     DeltaSnapshot events that arrive within atomic_window_ms of each other are
 *     merged into a single JSONL line, preserving PLC-scan-cycle atomicity.
 *
 * WAL FILE FORMAT (JSONL, one object per line)
 * ---------------------------------------------
 * Line 1  {"type":"schema","schema":{<serialized PlcSchemaStore>}}
 * Line 2  {"type":"manifest","start_time":"<iso8601>","mode":"...","namespaces":[...]}
 * 3..N-1  {"type":"anchor","ts":<ms>,"data":{<full tree>}}
 *         {"type":"delta","ts":<ms>,"changes":{<path>:<value>, ...}}
 * Line N  {"type":"footer","last_anchor_line":<line>,"record_count":<line>}
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
        const scl::PlcSchemaStore* t_schema_store = nullptr);

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

    // Anchor state
    int64_t last_anchor_ts_{0};
    uint32_t changes_since_anchor_{0};
    bool needs_first_anchor_{true};

    // Pending finalize task guard
    const int max_pending_ = 200;
    std::atomic<int> pending_tasks_{0};
};

} // namespace sgrn::gateway::database
