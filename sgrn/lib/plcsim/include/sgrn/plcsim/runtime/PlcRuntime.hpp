#pragma once
// =============================================================================
// PlcRuntime — schema + memory ownership, extracted out of ScriptS7Connection
//
// This is step 1 of moving s7shell towards the "SGRN Runtime" architecture:
// schema and memory become an independently allocatable object that protocol
// endpoints attach to, instead of being private fields owned by a single S7
// connection.
//
//   Load SCL -> Compile Schema -> Allocate PlcMemory -> Initialize DBs
//
// A PlcRuntime can be constructed directly from a schema file, with no PLC
// connection involved at all. Every protocol endpoint attaches to a
// PlcRuntime in exactly one role: client bindings initiate a connection to
// an external or degenerate local target while using this runtime as their
// local state, and server bindings listen for external clients while exposing
// this runtime's schema, memory and dirty-state. No endpoint owns schema or
// memory itself.
//
// NOTE: promoted to its own library (sgrn_plcsim) per the SGRN Runtime
// design doc — schema and memory are an independently allocatable object
// that protocol endpoints attach to. Nothing in this class is S7-specific.
// =============================================================================

#include <sgrn/Result.hpp>
#include <sgrn/gateway/twin/DbIOProvider.hpp>
#include <sgrn/gateway/twin/DbSnapshot.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/PlcState.hpp> // NEW
#include <sgrn/plcsim/PlcTagTable.hpp>
#include <sgrn/scl/errors.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/types.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace sgrn::plcsim::runtime
{
using PlcState = ::sgrn::gateway::twin::PlcState;
using PlcMemory = ::sgrn::gateway::twin::PlcMemory;
using PlcSchemaStore = ::sgrn::scl::PlcSchemaStore;
using PlcTagTable = ::sgrn::plcsim::PlcTagTable;
using DbIOProvider = ::sgrn::gateway::twin::DbIOProvider;
using DbSnapshot = ::sgrn::gateway::twin::DbSnapshot;

class PlcRuntime;

/// Invoked at the end of loadSclSchema()/loadJsonSchema() when set.
/// Lets the script layer re-register schema types after a reload without
/// plcsim depending back on it — mirrors the p_g_as_engine pattern: null by
/// default (headless use), armed once where the script engine is registered.
/// Not synchronized (same contract as p_g_as_engine).
inline std::function<void(PlcRuntime&)> g_on_schema_loaded;

/// A dirty byte range within a single DB, relative to the start of the DB.
struct DirtyRegion {
    uint32_t offset{0};
    uint32_t length{0};
};

/// Owns schema, memory, tag table and DB I/O providers for one PLC's worth
/// of memory. Knows nothing about any wire protocol (S7, OPC UA, Modbus,
/// gateway sync, ...) — protocol endpoints attach to it and read/write
/// through it. No protocol endpoint owns this state.
class PlcRuntime {
public:
    PlcRuntime();
    PlcRuntime(const PlcRuntime&) = delete;
    PlcRuntime& operator=(const PlcRuntime&) = delete;

    /// Empty runtime, no schema loaded yet. Equivalent to "Allocate
    /// PlcMemory" before any DBs exist.
    static std::shared_ptr<PlcRuntime> empty();

    /// "Load SCL -> Compile Schema -> Allocate PlcMemory -> Initialize DBs"
    /// with no protocol connection required.
    static std::shared_ptr<PlcRuntime> fromSclSchema(const std::string& t_path);

    /// Same, from a JSON schema definition.
    static std::shared_ptr<PlcRuntime> fromJsonSchema(const std::string& t_path);

    // ---- Schema mutation (usable at any point after construction) -----
    void loadSclSchema(const std::string& t_path);
    void loadJsonSchema(const std::string& t_path);
    void registerDb(uint16_t t_num, uint32_t t_size, const std::string& t_name = "");
    void registerUdt(const std::string& t_name, uint32_t t_size);
    void addUdtField(
        const std::string& t_udt_name, const std::string& t_name, const std::string& t_type_str, uint32_t t_offset, uint16_t t_count = 1);
    void loadRegistry(const std::string& t_path_or_content);

    // ---- Direct state access ------------------------------------------
    // Deliberately exposed as plain references (not getters returning
    // copies) so that call sites which used to say `conn_->memory`,
    // `conn_->schema`, `conn_->tagTable`, `conn_->dbSnapshots_` when those
    // were members of ScriptS7Connection keep compiling unchanged once
    // ScriptS7Connection holds a reference into a PlcRuntime instead of
    // owning these directly. See ScriptS7Connection in S7Connection.hpp.
    PlcState& getState() {
        return state_;
    }
    PlcMemory& getMemory() {
        return memory_;
    }
    PlcSchemaStore& getSchema() {
        return schema_;
    }
    std::unique_ptr<PlcTagTable>& getTagTableSlot() {
        return tag_table_;
    }
    std::map<uint16_t, DbSnapshot>& getPendingWrites() {
        return pending_writes_;
    }
    std::unordered_map<uint16_t, std::vector<uint8_t>>& getDbSnapshots() {
        return db_snapshots_;
    }

    DbIOProvider* getOrCreateDbProvider(uint16_t t_db_num);

    // ---- Dirty-region API -----------------------------------------------
    // The seam future protocol endpoints bind to instead of keeping their
    // own private diff buffers (see ProxySession, GatewaySync). A writer
    // (a script, a GatewaySync delta, a proxy poll) calls markDirty() after
    // mutating memory() directly; a push cycle calls takeDirty() to find
    // out what needs to be flushed and clears it in the same call.
    void markDirty(uint16_t t_db_num, uint32_t t_offset, uint32_t t_length);
    std::vector<DirtyRegion> takeDirty(uint16_t t_db_num);
    bool hasDirty(uint16_t t_db_num) const;

    using DirtyObserver = std::function<void(uint16_t, uint32_t, uint32_t)>;
    size_t addDirtyObserver(DirtyObserver t_observer);
    void removeDirtyObserver(size_t t_id);

    // ---- Discrete areas + TIA-style tag table -----------------------------
    // Tags (%I0.0, %Q0.0, %MW10, DB1.DBX0.0, …) are discrete addressed
    // objects in their own memory areas — a different beast from DataBlocks,
    // which are whole memory areas. Defined TIA-style as rows of
    // (name, type, address); a tag may be any scalar type or a UDT.
    //
    // Backing: PE/PA/MK live in runtime-owned arenas (auto-sized by
    // defineTag); discrete tags never alias twin memory — one backing each.
    // stay bit-identical with S7 traffic and deltas. Every tag write marks
    // the tag dirty (see takeDirtyTags).
    struct RuntimeTag {
        std::string name;
        std::string table; ///< source table (#TAG_TABLE name, "" for manual defineTag)
        std::string type_str;
        ::sgrn::scl::DataType type{::sgrn::scl::DataType::Bool};
        std::string udt_name;         // non-empty for UDT-typed tags
        int span_bytes{1};            // encoded size (UDT size for struct tags)
        ::sgrn::scl::PlcAddress addr; // resolved physical address
        ::sgrn::scl::DbField field;   // codec descriptor (children for UDTs)
    };

    /// Define one tag row: name + type + address, e.g.
    ///   defineTag("StartButton", "Bool", "%I0.0")
    ///   defineTag("LineSpeed", "Real", "%MD20")
    ///   defineTag("Drive", "MotorUDT", "DB3.DBB10")
    /// The address may be %-prefixed or bare. Returns an error string on failure
    /// (unknown type/address, duplicate name, range overflow, UDT missing).
    sgrn::Result<void, std::string> defineTag(
        const std::string& t_name, const std::string& t_type_str, const std::string& t_addr_str, const std::string& t_table = "");
    /// Same, with a pre-resolved address (schema import path — skips parsing).
    sgrn::Result<void, std::string> defineTagResolved(
        const std::string& t_name, const std::string& t_type_str, const ::sgrn::scl::PlcAddress& t_addr, const std::string& t_table = "");
    bool hasTag(const std::string& t_name) const;
    std::vector<std::string> tagNames() const;
    /// Tag names in one table ("" lists manually defined tags).
    std::vector<std::string> tagNamesInTable(const std::string& t_table) const;
    /// Distinct table names present (schema #TAG_TABLE blocks).
    std::vector<std::string> tagTables() const;
    sgrn::Result<RuntimeTag, std::string> describeTag(const std::string& t_name) const;

    /// Decode the tag's current bytes to JSON (object JSON for UDT tags).
    sgrn::Result<std::string, ::sgrn::scl::SclError> readTagJson(const std::string& t_name) const;
    /// Encode JSON into the tag's bytes (write-through + dirty marking).
    sgrn::Result<void, std::string> writeTagJson(const std::string& t_name, const std::string& t_value_json);

    /// Raw discrete-area access (backing store for tags; also serves the
    /// virtual S7 server's PE/PA/MK reads/writes). DB numbers are NOT valid
    /// here — use getMemory().read/writeDbMemory for DB areas.
    sgrn::Result<void, std::string> readAreaMemory(int t_area, size_t t_offset, size_t t_size, uint8_t* tp_buffer) const;
    sgrn::Result<void, std::string> writeAreaMemory(
        int t_area, size_t t_offset, size_t t_size, const uint8_t* tp_data, bool t_mark_dirty = true);
    sgrn::Result<void, std::string> writeAreaBit(int t_area, size_t t_byte_offset, int t_bit_index, bool t_value);
    /// Current arena size in bytes (0 when the area was never touched).
    size_t areaSize(int t_area) const;

    // ---- Tag dirty tracking ------------------------------------------------
    // Parallel to the DB ledger: tag writes mark the tag name dirty; the
    // gateways consume it into flat leaf-id deltas (takeDirtyTags) while
    // GatewaySync-style publishers keep using the DB ledger untouched.
    void markTagDirty(const std::string& t_name);
    std::vector<std::string> takeDirtyTags();
    bool hasDirtyTags() const;
    // ---- Reliable-uplink ledger ------------------------------------------------
    // GatewaySync-style publishers need at-least-once delivery with retries,
    // which take-only ledgers cannot provide (a failed send must be
    // re-queued). markTagDirty() records into BOTH ledgers; gateways consume
    // the broadcast ledger via takeDirtyTags(), publishers consume this one
    // via takePublishTags() and restore on NACK/timeout.
    std::vector<std::string> takePublishTags();
    std::vector<std::string> peekPublishTags() const;
    void restorePublishTags(const std::vector<std::string>& t_names);
    /// Re-fire tag observers for all currently-dirty tags without consuming
    /// the ledger (explicit-sync path, cf. sync() in s7shell).
    void renotifyDirtyTags();

    using TagDirtyObserver = std::function<void(const std::string& t_tag_name)>;
    size_t addTagDirtyObserver(TagDirtyObserver t_observer);
    void removeTagDirtyObserver(size_t t_id);

private:
    PlcState state_;
    PlcMemory memory_;
    PlcSchemaStore schema_;
    std::unique_ptr<PlcTagTable> tag_table_;

    std::unordered_map<uint16_t, std::unique_ptr<DbIOProvider>> db_providers_;
    std::map<uint16_t, DbSnapshot> pending_writes_;
    std::unordered_map<uint16_t, std::vector<uint8_t>> db_snapshots_;

    mutable std::mutex dirty_mutex_;
    std::unordered_map<uint16_t, std::vector<DirtyRegion>> dirty_regions_;

    std::mutex observer_mutex_;
    std::unordered_map<size_t, DirtyObserver> dirty_observers_;
    std::atomic_size_t next_observer_id_{1};

    // Discrete-area arenas (PE/PA/MK) + TIA-style tag rows. Guarded by
    // tag_mutex_ (single lock: arenas, tag map and tag-dirty set always
    // move together, so no lock ordering exists to get wrong).
    static constexpr size_t kDefaultAreaSize = 1024;
    static constexpr size_t kMaxAreaSize = 65536;
    mutable std::mutex tag_mutex_;
    std::unordered_map<int, std::vector<uint8_t>> areas_;
    std::unordered_map<std::string, RuntimeTag> tags_;
    std::set<std::string> dirty_tags_;
    std::set<std::string> publish_tags_;

    std::mutex tag_observer_mutex_;
    std::unordered_map<size_t, TagDirtyObserver> tag_observers_;
    std::atomic_size_t next_tag_observer_id_{1};

    /// Fire tag observers for the given names (no ledger changes).
    /// Must be called WITHOUT tag_mutex_ held.
    void notifyTagObservers(const std::vector<std::string>& t_names);

    /// Ensure the arena covers [0, t_min_size); grows (zero-filled) with a
    /// warning, errors past kMaxAreaSize. DB areas are rejected (twin-owned).
    sgrn::Result<void, std::string> ensureAreaSize(int t_area, size_t t_min_size);
    /// (Re)define every schema tag row (#TAG_TABLE blocks, JSON tags, TIA
    /// XML tags) after a schema load. Clears manual tags first — a schema
    /// load re-initializes the whole PLC image, tags included.
    void importSchemaTags();
};

using PlcRuntimeSPtr = std::shared_ptr<::sgrn::plcsim::runtime::PlcRuntime>;
} // namespace sgrn::plcsim::runtime
