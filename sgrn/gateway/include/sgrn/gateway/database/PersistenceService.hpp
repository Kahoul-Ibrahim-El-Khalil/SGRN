#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/gateway/config/persistence.hpp>
#include <sgrn/gateway/core/TelemetryBroker.hpp>
#include <sgrn/gateway/database/GatewayDatabase.hpp>
#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sgrn::gateway::database
{

/**
 * @brief Independent local historian / archiver service.
 *
 * ARCHITECTURAL ROLE
 * ──────────────────
 * PersistenceService is the *local persistence layer* in the gateway.
 * It is completely independent of DatastoreBridge:
 *
 *   - PersistenceService subscribes to TelemetryBroker and writes
 *     .json.zst batch files to `state/unsynced/` on disk.
 *
 *   - DatastoreBridge is a dumb uploader: it polls `pending_batches`
 *     in GatewayDatabase and uploads the files when connectivity is
 *     available.
 *
 * This separation means persistence works even in fully offline mode.
 *
 * ARCHIVE MODES  (configured via PersistenceConfig::mode)
 * ─────────────────────────────────────────────────────────
 *
 *  "full_tree"
 *     Every batch flush writes the full PLC memory snapshot.
 *     High storage footprint; easy to consume downstream.
 *
 *  "full_tree_with_anchor"
 *     A full "anchor" snapshot is written on startup and then again when:
 *       • anchor_interval_s seconds have elapsed  (time-based, 0 = disabled)
 *       • anchor_change_count records have accumulated (count-based, 0 = disabled)
 *     Between anchors only changed fields are archived.
 *
 *  "changes_with_timestamp"
 *     Only changed fields are archived.  Multiple DeltaSnapshot events that
 *     arrive within atomic_window_ms of each other are merged into a single
 *     JSON record, preserving PLC-scan-cycle atomicity and preventing
 *     thousands of 1-field JSON lines in the archive.
 *
 * FILE FORMAT
 * ───────────
 * Each .json.zst file contains one Zstd-compressed JSON envelope:
 *
 *   full_tree:
 *     { "type": "FULL_TREE", "timestamp": "<iso8601>", "data": { <full tree> } }
 *
 *   full_tree_with_anchor (anchor):
 *     { "type": "ANCHOR",    "timestamp": "<iso8601>", "data": { <full tree> } }
 *
 *   full_tree_with_anchor (delta batch) and changes_with_timestamp:
 *     {
 *       "type": "DELTA_BATCH",
 *       "size": <n>,
 *       "data": [
 *         { "timestamp": "<iso8601>", "changes": { "DB10.field": <value>, … } },
 *         …
 *       ]
 *     }
 */
class PersistenceService {
public:
    explicit PersistenceService(asio::thread_pool* tp_heavy_pool);
    ~PersistenceService();

    /**
     * @brief Configure and start the service.
     *
     * Subscribes to TelemetryBroker.  Must be called before any events
     * are published.
     *
     * @param cfg       Persistence configuration.
     * @param state_dir Root directory for unsynced/synced sub-directories.
     * @param db        Shared gateway database (may be nullptr for no-DB mode).
     */
    sgrn::Result<void> configure(
        const sgrn::gateway::config::PersistenceConfig& t_cfg, const std::string& t_state_dir, std::shared_ptr<GatewayDatabase> tsp_db);

    void stop();

    /// True if the service is active (configured and not stopped).
    bool isActive() const {
        return active_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Inject a full-tree JSON string directly (e.g. from the S7 poller).
     *
     * Useful for the DatastoreBridge or SgrnShell to push an anchor snapshot
     * without going through TelemetryBroker.
     */
    void ingestFullTree(const std::string& t_full_tree_json);

private:
    // ── Internal event handler ──────────────────────────────────────────────

    void onTelemetryEvent(const sgrn::gateway::core::TelemetryEvent& t_event);

    // ── Namespace filter ────────────────────────────────────────────────────

    bool passesFilter(const std::string& t_path) const;

    // ── Atomic merge buffer ─────────────────────────────────────────────────

    struct MergeBuffer {
        int64_t open_ts{0};                                  ///< timestamp of first event in window
        std::unordered_map<std::string, std::string> fields; ///< path → JSON value string
    };

    void mergeOrFlushEvent(const std::string& t_path, const std::string& t_value_json, int64_t t_now);
    void commitMergeBuffer();

    // ── Batch accumulation & flushing ───────────────────────────────────────

    void appendRecord(const std::string& t_record_json);
    void maybeFlushBatch(int64_t t_now);
    void flushBatch(int64_t t_now);

    // ── File I/O ────────────────────────────────────────────────────────────

    void writeFullTreeFile(const std::string& t_tree_json, int64_t t_ts, bool t_is_anchor);
    void writeBatchFile(const std::string& t_envelope_json, const std::string& t_filepath, int64_t t_ts_start, int64_t t_ts_end,
        uint8_t t_compression_level);

    // ── Anchor management ───────────────────────────────────────────────────

    bool anchorDue(int64_t t_now) const;
    void resetAnchor(int64_t t_now);

    // ── Members ─────────────────────────────────────────────────────────────

    asio::thread_pool* heavy_pool_{nullptr};
    sgrn::gateway::config::PersistenceConfig cfg_;
    std::string state_dir_;
    std::string unsynced_dir_;
    std::shared_ptr<GatewayDatabase> db_;

    sgrn::gateway::core::TelemetryBroker::SubscriberId broker_sub_id_{0};
    std::atomic<bool> active_{false};

    // Batch state (all access on the io_context strand — no extra lock needed)
    std::vector<std::string> batch_records_;
    int64_t batch_start_ts_{0};
    int64_t last_flush_ts_{0};
    std::atomic<int> pending_writes_{0};

    // Atomic merge buffer
    MergeBuffer merge_buf_;

    // Anchor state
    int64_t last_anchor_ts_{0};
    uint32_t changes_since_anchor_{0};
    bool needs_first_anchor_{true};

    // Pending task guard
    const int max_pending_ = 200;
    std::atomic<int> pending_tasks_{0};
};

} // namespace sgrn::gateway::database
