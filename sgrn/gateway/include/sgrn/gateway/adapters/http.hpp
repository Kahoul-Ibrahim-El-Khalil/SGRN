#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/gateway/common/AdapterBase.hpp>
#include <sgrn/gateway/security/SecurityManager.hpp>
#include <httplib.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sgrn::scl
{
class PlcSchemaStore;
struct ModbusVirtualMap;
} // namespace sgrn::scl

namespace sgrn::gateway::twin
{
class PlcMemory;
} // namespace sgrn::gateway::twin

namespace sgrn::gateway::database
{
class GatewayDatabase;
}

namespace sgrn::gateway::adapters
{
using ::sgrn::gateway::twin::PlcMemory;
using PlcSchemaStore = ::sgrn::scl::PlcSchemaStore;

/**
 * @brief HTTP Adapter for Gateway (northward-facing).
 *
 * Provides HTTP endpoints to query and write to PLC memory:
 *   GET  /data/<path>   — read a field, subtree, or full twin
 *   POST /data/<path>   — write via JSON (partial subtree, atomic merge)
 *   PUT  /data/         — write raw bytes (octet-stream or base64 text/plain)
 *   PUT  /data/?db=&offset=&size=  — same, with explicit addressing
 *
 * Security: each client IP is checked against the same DB-ACL table used by
 * S7ProtocolAdapter.  Unauthorized writes return HTTP 403.
 */
class HttpAdapter {
public:
    HttpAdapter();
    ~HttpAdapter();

    /**
     * @brief Start the HTTP server.
     * @param t_acls  DB→IP whitelist imported from the gateway config
     *                (same map used by S7ProtocolAdapter).
     * @param t_policy The security policy to use for missing ACL entries.
     */
    sgrn::Result<void> start(const std::string& t_ip, uint16_t t_port, const PlcSchemaStore& t_registry, PlcMemory& t_memory,
        std::shared_ptr<sgrn::gateway::database::GatewayDatabase> tsp_db, std::shared_ptr<::sgrn::gateway::SecurityManager> tsp_security,
        const ::sgrn::scl::ModbusVirtualMap* tp_modbus_map = nullptr);

    void stop();

private:
    // ── ACL helpers ─────────────────────────────────────────────────────────
    bool isAuthorized(const httplib::Request& t_req, std::optional<uint16_t> t_db_number = std::nullopt) const;
    bool isAuthorizedField(
        const httplib::Request& t_req, std::optional<uint16_t> t_db_number, const std::string& t_field_path, bool t_is_write) const;

    // ── HTTP Handlers ────────────────────────────────────────────────────────
    // Semantic (schema-aware) endpoints: /data/*
    void handleGetData(const httplib::Request& t_req, httplib::Response& t_res, const PlcSchemaStore& t_registry, PlcMemory& t_memory);
    void handlePost(const httplib::Request& t_req, httplib::Response& t_res, const PlcSchemaStore& t_registry, PlcMemory& t_memory);
    void handlePut(const httplib::Request& t_req, httplib::Response& t_res, const PlcSchemaStore& t_registry, PlcMemory& t_memory);

    // Raw memory endpoints: /memory/*
    // Binary mode (single DB, raw bytes):
    void handleGetMemoryBinary(const httplib::Request& t_req, httplib::Response& t_res, PlcMemory& t_memory);
    void handlePutMemoryBinary(const httplib::Request& t_req, httplib::Response& t_res, PlcMemory& t_memory);
    // Batch mode (multiple DBs, base64url JSON):
    void handlePutMemoryBatch(const httplib::Request& t_req, httplib::Response& t_res, PlcMemory& t_memory);

    // Registry and diagnostic endpoints
    void handleGetRegistry(const httplib::Request& t_req, httplib::Response& t_res, const PlcSchemaStore& t_registry);
    void handleGetModbusRegistry(const httplib::Request& t_req, httplib::Response& t_res);
    void handleGetRegistryTypes(const httplib::Request& t_req, httplib::Response& t_res);
    void handleGetConnections(const httplib::Request& t_req, httplib::Response& t_res, sgrn::gateway::database::GatewayDatabase& t_db);
    void handleGetDbHistory(const httplib::Request& t_req, httplib::Response& t_res, sgrn::gateway::database::GatewayDatabase& t_db);
    void handleGetDbSessions(const httplib::Request& t_req, httplib::Response& t_res, sgrn::gateway::database::GatewayDatabase& t_db);
    void handleGetDbLogs(const httplib::Request& t_req, httplib::Response& t_res, sgrn::gateway::database::GatewayDatabase& t_db);
    void handleGetEndpoints(const httplib::Request& t_req, httplib::Response& t_res);
    void registerWebAssets();

    // ── Server internals ─────────────────────────────────────────────────────
    std::unique_ptr<httplib::Server> server_;
    std::atomic<bool> running_{false};
    std::thread server_thread_;

    // Live telemetry cache removed (Tier 4: Unified via TreeCacheEngine)

    std::shared_ptr<::sgrn::gateway::SecurityManager> security_manager_;

    // Modbus virtual map for REST discovery
    const ::sgrn::scl::ModbusVirtualMap* modbus_map_{nullptr};
};

} // namespace sgrn::gateway::adapters
