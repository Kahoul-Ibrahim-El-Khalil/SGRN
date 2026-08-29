#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/gateway/adapters/s7/PlcClient.hpp>
#include <sgrn/gateway/twin/DbIOProvider.hpp>
#include <sgrn/gateway/twin/DbSnapshot.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/wrappers/s7/error.hpp>
#include <sgrn/s7shell/PlcTagTable.hpp>
#include <sgrn/s7shell/runtime/PlcRuntime.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/types.hpp>
#include <cstdint>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sgrn::s7shell
{
using DbIOProvider = ::sgrn::gateway::twin::DbIOProvider;
using PlcState = ::sgrn::gateway::twin::PlcState;
using PlcMemory = ::sgrn::gateway::twin::PlcMemory;
using PlcTagTable = ::sgrn::s7shell::PlcTagTable;
using PlcSchemaStore = ::sgrn::scl::PlcSchemaStore;
using DbSnapshot = ::sgrn::gateway::twin::DbSnapshot;
using FieldTarget = ::sgrn::scl::FieldTarget;
} // namespace sgrn::s7shell

namespace sgrn::s7shell::shell
{

// Forward declarations
class ScriptDataBlock;
class ScriptTagTable;
class ScriptS7Diagnostics;
class ScriptS7PlcControl;
class ScriptS7ConnectionProxy;
class ScriptS7Blocks;
class ScriptS7Async;
class ScriptS7Memory;

struct ScriptS7Connection {
    /// Owns schema + memory + tag table + DB I/O providers. Either private
    /// to this connection (default constructor) or shared with other
    /// ScriptS7Connections that were explicitly constructed against the
    /// same PlcRuntime — see the (ip, rack, slot, runtime) constructor.
    std::shared_ptr<::sgrn::s7shell::runtime::PlcRuntime> runtime_;

    ::sgrn::gateway::adapters::s7::PlcClient client_;

    // References into `runtime`'s state. Kept as references (not owned
    // values) so every existing call site of the form `conn_->memory`,
    // `conn_->schema`, `conn_->tagTable`, `conn_->dbSnapshots_`,
    // `conn_->pendingWrites_` keeps compiling unchanged — only the
    // ownership moved, not the access pattern.
    ::sgrn::s7shell::PlcMemory& memory_;
    ::sgrn::s7shell::PlcSchemaStore& schema_;
    std::unique_ptr<::sgrn::s7shell::PlcTagTable>& tag_table_;
    std::map<uint16_t, ::sgrn::s7shell::DbSnapshot>& pending_writes_;
    std::unordered_map<uint16_t, std::vector<uint8_t>>& db_snapshots_;

    std::string conn_ip_;
    int conn_rack_{0};
    int conn_slot_{1};
    uint16_t conn_port_{102};
    uint16_t conn_type_{CONNTYPE_PG};
    bool conn_use_tsap_{false};
    uint16_t conn_local_tsap_{0x0100};
    uint16_t conn_remote_tsap_{0x0102};

    // ── SclError tracking — updated by all S7 operations —————————————
    std::string last_error_msg_;
    int last_error_code_{0}; ///< 0 = no error

    void setLastError(::sgrn::scl::SclError t_err) {
        last_error_msg_ = toString(t_err);
        last_error_code_ = static_cast<int>(t_err);
    }
    void setLastError(::sgrn::gateway::wrappers::s7::S7Error t_err) {
        last_error_msg_ = toString(t_err);
        last_error_code_ = static_cast<int>(t_err);
    }
    void clearLastError() {
        last_error_msg_.clear();
        last_error_code_ = 0;
    }

    /// Creates a private PlcRuntime for this connection alone (previous
    /// behavior — nothing else can see this connection's memory/schema).
    ScriptS7Connection(const std::string& t_ip, int t_rack, int t_slot, uint16_t t_port = 102);

    /// Attaches this connection to an existing, possibly shared,
    /// PlcRuntime. Two ScriptS7Connections constructed this way against the
    /// same `rt` observe the same memory image, schema and dirty-region
    /// ledger — the seam other protocol endpoints (proxy mirroring, gateway
    /// sync, and eventually a protocol server) bind to instead of each
    /// keeping a private copy.
    ScriptS7Connection(
        const std::string& t_ip, int t_rack, int t_slot, uint16_t t_port, std::shared_ptr<::sgrn::s7shell::runtime::PlcRuntime> tsp_rt);

    ~ScriptS7Connection();

    void loadRegistry(const std::string& t_path);
    sgrn::Result<void, scl::SclError> reconnect();
    void setConnectionSettings(uint16_t t_type, uint16_t t_port, bool t_reconnect_if_connected = true);
    sgrn::Result<void, scl::SclError> connectWithTsap(const std::string& t_ip, uint16_t t_local_tsap, uint16_t t_remote_tsap);
    void setTsapMode(uint16_t t_local_tsap, uint16_t t_remote_tsap);
    void setRackSlotMode();

    void loadSclSchema(const std::string& t_path);
    void loadJsonSchema(const std::string& t_path);
    void registerDb(uint16_t t_num, uint32_t t_size, const std::string& t_name = "");
    void registerUdt(const std::string& t_name, uint32_t t_size);
    void addUdtField(
        const std::string& t_udt_name, const std::string& t_name, const std::string& t_type_str, uint32_t t_offset, uint16_t t_count = 1);

    ::sgrn::gateway::twin::DbIOProvider* getOrCreateDbProvider(uint16_t t_db_num);
};

class ScriptSchemaStore;

class ScriptS7Client {
public:
    ScriptS7Client(const std::string& t_ip, int t_rack, int t_slot, uint16_t t_port = 102);

    /// Attaches this client to an existing (possibly shared) PlcRuntime
    /// instead of allocating a private one. Lets two S7Client instances —
    /// or an S7Client and a future protocol server/GatewaySync/ProxySession
    /// — observe and mutate the same schema, memory and dirty-region ledger.
    ScriptS7Client(
        const std::string& t_ip, int t_rack, int t_slot, uint16_t t_port, std::shared_ptr<::sgrn::s7shell::runtime::PlcRuntime> tsp_rt);

    void addRef();
    void release();

    /// The PlcRuntime backing this client's schema/memory. Share this with
    /// other S7Client/protocol endpoint constructors to have them operate
    /// on the same memory image.
    std::shared_ptr<::sgrn::s7shell::runtime::PlcRuntime> getRuntime() const;

    ScriptDataBlock* db(uint16_t t_db_num);
    ScriptDataBlock* dbByName(const std::string& t_name);

    ScriptTagTable* tags();

    std::string tagGet(const std::string& t_name);
    double tagGetReal(const std::string& t_name);
    int32_t tagGetInt(const std::string& t_name);
    bool tagGetBool(const std::string& t_name);

    void tagPut(const std::string& t_name, const std::string& t_raw_val);
    void tagPutDouble(const std::string& t_name, double t_val);
    void tagPutInt(const std::string& t_name, int32_t t_val);
    void tagPutBool(const std::string& t_name, bool t_val);

    std::string read(const std::string& t_target);
    void write(const std::string& t_target, const std::string& t_val_str);
    void write(const std::string& t_target, double t_val);
    void write(const std::string& t_target, int32_t t_val);
    void write(const std::string& t_target, bool t_val);

    void reconnect();
    bool reconnectOk();                                          ///< like reconnect() but returns true on success
    bool reconnectWithRetry(int t_max_attempts, int t_delay_ms); ///< blocking retry loop
    void disconnect();
    bool isConnected() const;
    bool ping(); ///< lightweight PLC liveness check

    // ── SclError introspection ──────────────────────────────────────────
    std::string lastError() const; ///< last S7 error message (empty = no error)
    int getLastErrorCode() const;  ///< last S7 error code (0 = no error)
    bool lastOpOk() const {
        return getLastErrorCode() == 0;
    }
    void clearLastError();

    std::string listSymbols(const std::string& t_ref);
    std::string searchSymbols(const std::string& t_regex_str);

    ScriptS7Diagnostics* diagnostics();
    ScriptS7PlcControl* control();
    ScriptS7ConnectionProxy* connection();
    ScriptS7Blocks* blocks();
    ScriptS7Async* asyncIo();
    ScriptS7Memory* memory_();
    ScriptSchemaStore* schema_();

    void loadSclSchema(const std::string& t_path);
    void loadJsonSchema(const std::string& t_path);
    void registerDb(uint16_t t_num, uint32_t t_size, const std::string& t_name = "");
    void registerUdt(const std::string& t_name, uint32_t t_size);
    void addUdtField(
        const std::string& t_udt_name, const std::string& t_name, const std::string& t_type_str, uint32_t t_offset, uint16_t t_count = 1);

    void loadRegistry(const std::string& t_path);
    bool hasSchema() const;
    bool hasRegistry() const;

    void setConnectionType(uint16_t t_type);
    void setPort(uint16_t t_port);
    int connectionType() const;
    uint16_t getPort() const;

    ScriptS7Connection* internalConn() {
        return conn_.get();
    }

private:
    int ref_count_{1};
    std::unique_ptr<ScriptS7Connection> conn_;
};

} // namespace sgrn::s7shell::shell
