#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sgrn::gateway::config
{

/**
 * @brief Controls the PersistenceService (local historian / archiver).
 *
 * The PersistenceService is independent of the DatastoreBridge.
 * It can produce local `.json.zst` archives even with no backend configured.
 * The DatastoreBridge then simply reads the `pending_batches` DB table and
 * uploads whatever the PersistenceService has written to disk.
 */
struct PersistenceConfig {

    /// Master on/off switch.
    bool enabled{false};

    /**
     * @brief Archive mode. One of:
     *
     *   "full_tree"           — Every flush writes the complete PLC snapshot.
     *                          High storage, simplest to consume.
     *
     *   "full_tree_with_anchor" — Writes a full snapshot on the first flush
     *                          and again whenever anchor_interval_s elapses OR
     *                          anchor_change_count accumulated changes are
     *                          recorded (whichever fires first).  In between,
     *                          only changed fields are recorded.
     *                          Setting either anchor trigger to 0 disables it.
     *
     *   "changes_with_timestamp" — Only changed fields are archived, each
     *                          group atomically merged within atomic_window_ms
     *                          into a single timestamped object.
     */
    std::string mode{"changes_with_timestamp"};

    /**
     * @brief Storage serialization format. One of:
     *   "jsonl"  — Line-delimited JSON inside Zstd (.jsonl.zst) [Default]
     *   "binary" — Compact binary frame sequence inside Zstd (.bin.zst)
     *              [Canonical: new archival features target this format first.
     *              RecoveryEngine restores both formats.]
     */
    std::string format{"jsonl"};

    /**
     * @brief Field-level namespace filter.
     *
     * If empty, all fields of all DBs are persisted.
     * Otherwise only fields whose canonical path starts with one of these
     * prefixes are archived.  Examples:
     *   "DB10"                   → entire ReactorCore block
     *   "DB11.hot_leg_temp"      → a single field
     */
    std::vector<std::string> namespaces;

    /**
     * @brief Atomic merge window (milliseconds).
     *
     * DeltaSnapshot events that arrive within this window of each other are
     * merged into a single JSON object before being appended to the batch.
     * This mirrors the PLC scan cycle granularity and avoids thousands of
     * 1-field JSON entries in the archive.
     *
     * Set to 0 to disable merging (every event is a standalone record).
     */
    uint32_t atomic_window_ms{10};

    // ── Batch flush triggers ────────────────────────────────────────────────

    /// Flush to disk when the pending batch reaches this many entries.
    uint32_t batch_size{1000};

    /// Flush to disk after this many seconds even if batch_size is not reached.
    uint32_t batch_interval_s{300};

    // ── Anchor triggers (full_tree_with_anchor mode only) ──────────────────

    /**
     * @brief Re-issue a full anchor snapshot after this many seconds.
     * Set to 0 to disable time-based anchoring.
     */
    uint32_t anchor_interval_s{86400}; // default: 1 day

    /**
     * @brief Re-issue a full anchor snapshot after this many accumulated
     * field-change records since the last anchor.
     * Set to 0 to disable count-based anchoring.
     */
    uint32_t anchor_change_count{10000};

    // ── Compression ─────────────────────────────────────────────────────────

    /// Zstd level for delta / changes batches.
    uint8_t zstd_level{5};

    /// Zstd level for full-tree anchor snapshots (higher = smaller file).
    uint8_t anchor_zstd_level{12};
};

} // namespace sgrn::gateway::config
