#pragma once
#include <fmt/core.h>
#include <sgrn/gateway/twin/DbMemorySpan.hpp>
#include <sgrn/gateway/twin/DbSnapshot.hpp>
#include <sgrn/gateway/twin/field_update.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/structures/SharedBuffer.hpp>
#include <sgrn/utils/threading.hpp>
#include <ankerl/unordered_dense.h>
#include <asio.hpp>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace asio
{
class io_context;
class thread_pool;
} // namespace asio

namespace sgrn::gateway::twin
{
using PlcSchemaStore = ::sgrn::scl::PlcSchemaStore;

class PlcCommandProcessor;
class SnapshotRegistry;

/**
 * @brief Exact failure reason for a PlcMemory raw-memory access.
 *
 * A closed enum, not a string: callers branch on the *kind* of failure.
 * Human-readable text is derived on demand via toString()/fmt::formatter
 * from a static string_view literal — never stored or built dynamically,
 * so there's no dangling-pointer risk and no per-error allocation.
 */
enum class PlcMemoryError : uint8_t {
    PLC_STATE_NOT_INITIALIZED,      ///< attachState() was never called
    DB_SEGMENT_NOT_FOUND,           ///< db_number isn't registered in the arena
    RANGE_EXCEEDS_ALLOWED_SPACE,    ///< offset/size falls outside the segment/arena being addressed
    RANGE_CROSSES_SEGMENT_BOUNDARY, ///< an arena-relative range spans more than one DB segment
    UNMAPPED_ARENA_REGION,          ///< an arena-relative offset doesn't fall inside ANY registered DB segment
    NULL_BUFFER,                    ///< buffer == nullptr while size > 0
    INVALID_BIT_INDEX,              ///< writeBit: bit_index not in [0, 7]
    UKNOWN,
    EXTERNAL
};

constexpr std::string_view toString(PlcMemoryError t_status) noexcept {
    switch (t_status) {
        case PlcMemoryError::PLC_STATE_NOT_INITIALIZED:
            return "PLC state not attached";
        case PlcMemoryError::DB_SEGMENT_NOT_FOUND:
            return "DB segment not registered";
        case PlcMemoryError::RANGE_EXCEEDS_ALLOWED_SPACE:
            return "Range exceeds allowed space";
        case PlcMemoryError::RANGE_CROSSES_SEGMENT_BOUNDARY:
            return "Range crosses a DB segment boundary";
        case PlcMemoryError::UNMAPPED_ARENA_REGION:
            return "Offset does not fall inside any registered DB segment";
        case PlcMemoryError::NULL_BUFFER:
            return "Null buffer with non-zero size";
        case PlcMemoryError::INVALID_BIT_INDEX:
            return "Bit index out of range [0, 7]";
        case PlcMemoryError::UKNOWN:
            return "Uknown Plc Memory Error";
        case PlcMemoryError::EXTERNAL:
            return "External Error that does not pertain to the Plc Memory";
    }
    return "Unknown PlcMemory error";
}

/**
 * @brief One arena-relative byte range, for the whole-arena batch API.
 *
 * `offset` is absolute — measured from the start of the arena, not from
 * any one DB. `buffer` is intentionally non-const so ONE struct can serve
 * both read() (buffer is the destination PlcMemory fills in) and write()
 * (buffer is the source PlcMemory only reads from). write() never mutates
 * through it; the type system doesn't enforce that split here — it's a
 * deliberate trade of a little const-strictness for a single reusable
 * descriptor type. Split into separate ReadSpan/WriteSpan structs later if
 * that trade stops being worth it.
 */
struct MemorySpan {
    size_t offset;
    size_t size;
    uint8_t* p_buffer;
};

class PlcState;
/**
 * @brief OT Bridge connecting the digital twin memory to the real PLC.
 *
 * Architectural Relationship:
 * ───────────────────────────
 * PlcMemory acts as the primary IO engine and data ingestion point.
 *
 * - PlcClient / S7Adapter: Plugs into PlcMemory to write chunks of byte
 *   arrays retrieved from the PLC.
 * - PlcState: PlcMemory updates the underlying `PlcArena` mapped by PlcState.
 * - TelemetryBroker: When PlcMemory detects a change (via `handleWriteEvent`),
 *   it commands PlcState to generate a DeltaSnapshot, which is then
 *   published to the TelemetryBroker for downstream WebSocket clients.
 *
 * Deliberately NOT derived from anything: nothing in the codebase holds a
 * PlcMemory behind a base-class pointer/reference, so there is no vtable,
 * no dynamic dispatch cost on the read/write hot path, and no name-hiding
 * hazard from an unrelated base class sharing these method names. Marked
 * `final` since nothing subclasses it today, either.
 */
class PlcMemory final {
public:
    PlcMemory();
    ~PlcMemory();

    PlcMemory(const PlcMemory&) = delete;
    PlcMemory& operator=(const PlcMemory&) = delete;
    PlcMemory(PlcMemory&&) = delete;
    PlcMemory& operator=(PlcMemory&&) = delete;

    void attachState(PlcState& t_state);
    PlcState* state() const;

    sgrn::Result<void, PlcMemoryError> loadRegistry(const PlcSchemaStore& t_store);
    sgrn::Result<void, PlcMemoryError> registerDb(uint16_t t_db_number, size_t t_size);

    sgrn::Result<void, PlcMemoryError> updateField(uint16_t t_db_number, const std::string& t_field_path, const std::string& t_value_json);
    sgrn::Result<void, PlcMemoryError> updateFieldWithTimestamp(
        uint16_t t_db_number, const std::string& t_field_path, const std::string& t_value_json, uint64_t t_timestamp);

    /// When set, updateField() uses this instead of wall-clock time (e.g. script simulation).
    void setTimestampProvider(std::function<uint64_t()> t_provider) {
        timestamp_provider_ = std::move(t_provider);
    }
    void clearTimestampProvider() {
        timestamp_provider_ = {};
    }
    sgrn::Result<std::string, PlcMemoryError> getFieldValue(uint16_t t_db_number, const std::string& t_field_path) const;
    sgrn::Result<std::string, PlcMemoryError> getDbJson(uint16_t t_db_number) const;
    /// JSON snapshot of a DB or subtree. Empty field_path uses cached_full_json when the DB is clean;
    /// non-empty paths (e.g. "pump_1") always serialize that subtree via PlcNode::serialize.
    sgrn::Result<std::string, PlcMemoryError> getSubtreeJson(uint16_t t_db_number, const std::string& t_field_path) const;

    std::string getDbJsonString(uint16_t t_db_number) const;
    std::string getMemoryLayoutAsJson() const;
    std::string getDigitalTwinJson() const;
    std::string getDigitalTwinJsonString() const;
    std::vector<uint8_t> getFullPlantSnapshot() const;

    bool checkDirty();
    /// Nested JSON delta (legacy / firehose mode): {"ReactorCore": {"field": val}}
    std::string getDeltaSnapshot(const std::vector<uint16_t>& t_filter = {});
    /// Flat numeric-keyed delta (dictionary / Phase-4 mode): {"<id>": val, ...}
    std::string getDeltaSnapshotFlat(
        const ankerl::unordered_dense::map<std::string, uint32_t>& t_path_to_id, const std::vector<uint16_t>& t_filter = {});
    std::vector<FieldUpdateNotification> collectTypedDirtyLeaves(uint16_t t_db_number);
    std::vector<uint16_t> getDirtyDbNumbers() const;
    bool waitForDirty(int t_timeout_ms);
    void signalDirty();

    const PlcNode* findSymbol(const std::string& t_path) const;
    const PlcNode* findSymbol(uint16_t t_db_number, const std::string& t_field_path) const;

    // ── Tier 1: whole-arena, address-space view ─────────────────────────────
    // `offset` is relative to the START OF THE ARENA. PlcMemory resolves
    // which DB segment a request touches internally; a request that spans
    // more than one segment is rejected (RANGE_CROSSES_SEGMENT_BOUNDARY)
    // rather than silently split — a DB is a logical unit, and a range that
    // straddles two of them almost always means the caller's offset math
    // is wrong, not that a cross-DB copy was actually intended.

    /// Read [t_offset, t_offset + t_size) from the arena into t_buffer.
    sgrn::Result<void, PlcMemoryError> read(size_t t_offset, size_t t_size, uint8_t* tp_buffer) const;

    /// Write t_buffer into [t_offset, t_offset + t_size) of the arena.
    sgrn::Result<void, PlcMemoryError> write(size_t t_offset, size_t t_size, const uint8_t* tp_buffer);

    /// Scatter/gather over several arena-relative ranges, atomically: every
    /// span is resolved and validated up front; the unique set of DB
    /// segments touched is locked ONCE each (sorted by physical arena
    /// offset, to fix a single lock-acquisition order across all callers
    /// and avoid ABBA deadlocks between overlapping batches) — this is NOT
    /// sugar for calling read()/write() per span in a loop.
    sgrn::Result<void, PlcMemoryError> read(std::span<const MemorySpan> t_spans) const;
    sgrn::Result<void, PlcMemoryError> write(std::span<const MemorySpan> t_spans);

    // ── Tier 2: DB-scoped fast path ──────────────────────────────────────────
    // Caller already knows the DB — no reverse offset→segment lookup, no
    // boundary-crossing check needed, exactly one segment resolved directly
    // by id and exactly one lock acquisition. This is the path protocol
    // adapters should use for deterministic, low-latency single-DB access.

    sgrn::Result<void, PlcMemoryError> readDbMemory(uint16_t t_db_number, size_t t_offset, size_t t_size, uint8_t* tp_buffer) const;
    sgrn::Result<void, PlcMemoryError> writeDbMemory(uint16_t t_db_number, size_t t_offset, size_t t_size, const uint8_t* tp_buffer);

    /// Batch variant: spans may name different DBs; the unique set of DBs
    /// touched is resolved by id (no arena scan) and locked once each, in
    /// the same sorted-by-physical-offset order as the Tier-1 batch above.
    sgrn::Result<void, PlcMemoryError> readDbMemory(std::span<const DbMemorySpan> t_spans) const;
    sgrn::Result<void, PlcMemoryError> writeDbMemory(std::span<const DbMemorySpan> t_spans);

    sgrn::Result<void, PlcMemoryError> writeBit(uint16_t t_db_number, size_t t_byte_offset, int t_bit_index, bool t_value);

    PlcCommandProcessor* processor() const {
        return cmd_processor_.get();
    }
    SnapshotRegistry* snapshots() const {
        return snapshot_registry_.get();
    }

    void setCacheEnabled(bool t_is_cache__enabled) {
        is_cache_enabled_ = t_is_cache__enabled;
    }
    struct RangeEntry {
        uint16_t db;
        size_t offset;
        size_t size;
        const PlcNode* node; // leaf node that covers this range
        size_t node_span;    // cached node size (bytes)
    };

    // Sorted by (db, offset) – built at schema registration
    std::vector<RangeEntry> range_index_;
    mutable std::shared_mutex index_mutex_;

    // Build index from PlcState nodes (called in loadRegistry)
    void buildRangeIndex();

private:
    /// Result of resolving an arena-relative [offset, offset+size) range to
    /// the DB segment that contains it. `entry == nullptr` means failure;
    /// `error` explains why (only meaningful when entry == nullptr).
    struct SegmentLookup {
        DbEntry* p_entry{nullptr};
        size_t rel_offset{0};
        PlcMemoryError error{PlcMemoryError::UNMAPPED_ARENA_REGION};
    };
    SegmentLookup findContainingSegment(size_t t_abs_offset, size_t t_size) const;

    /// Shared tail of every write path: bumps per-field node versions for
    /// leaf fields intersecting [t_offset, t_offset + t_size) in
    /// t_db_number, comparing t_old (arena pointer at t_offset) against
    /// t_new (candidate bytes, indexed the same way). Must be called while
    /// still holding the segment's write lock.
    void bumpFieldVersions(uint16_t t_db_number, size_t t_offset, size_t t_size, const uint8_t* tp_old, const uint8_t* tp_new);

    PlcState* p_plc_state_{nullptr};
    std::unique_ptr<PlcCommandProcessor> cmd_processor_;
    std::unique_ptr<SnapshotRegistry> snapshot_registry_;

    mutable std::mutex dirty_cv_mutex_;
    std::condition_variable dirty_cv_;
    bool dirty_flag_{false};

    bool is_cache_enabled_ = false;
    std::function<uint64_t()> timestamp_provider_;
};

} // namespace sgrn::gateway::twin

template <>
struct fmt::formatter<sgrn::gateway::twin::PlcMemoryError> : formatter<std::string_view> {
    auto format(sgrn::gateway::twin::PlcMemoryError t_status, format_context& t_ctx) const {
        return formatter<std::string_view>::format(sgrn::gateway::twin::toString(t_status), t_ctx);
    }
};
