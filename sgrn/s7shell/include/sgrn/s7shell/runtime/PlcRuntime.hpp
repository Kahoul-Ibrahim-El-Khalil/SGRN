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
// NOTE: this currently lives inside the s7shell component because s7shell
// is its only consumer today. If/when a standalone protocol server module
// (OPC UA, Modbus, ...) is added, this should be promoted to its own
// library (e.g. sgrn_runtime) that both depend on, per the SGRN Runtime
// design doc. Nothing in this class is S7-specific, so that move should be
// mechanical.
// =============================================================================

#include <sgrn/gateway/twin/DbIOProvider.hpp>
#include <sgrn/gateway/twin/DbSnapshot.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/PlcState.hpp> // NEW
#include <sgrn/s7shell/PlcTagTable.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/types.hpp>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sgrn::s7shell::runtime
{
using PlcState = ::sgrn::gateway::twin::PlcState;
using PlcMemory = ::sgrn::gateway::twin::PlcMemory;
using PlcSchemaStore = ::sgrn::scl::PlcSchemaStore;
using PlcTagTable = ::sgrn::s7shell::PlcTagTable;
using DbIOProvider = ::sgrn::gateway::twin::DbIOProvider;
using DbSnapshot = ::sgrn::gateway::twin::DbSnapshot;

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
};

using PlcRuntimeSPtr = std::shared_ptr<::sgrn::s7shell::runtime::PlcRuntime>;
} // namespace sgrn::s7shell::runtime
