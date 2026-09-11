#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/gateway/adapters/http/types.hpp>
#include <sgrn/gateway/adapters/northbound/NorthboundServer.hpp>
#include <sgrn/gateway/security/SecurityManager.hpp>
#include <atomic>
#include <crow.h>
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
 *
 * Transport: Crow (asio-based). In the full gateway the HTTP routes share one
 * NorthboundServer listener — and therefore one port — with the WebSocket
 * adapter (`/ws`). Standalone start() (used by s7shell's HttpServer binding)
 * brings up a private listener instead.
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
        const ::sgrn::scl::ModbusVirtualMap* tp_modbus_map = nullptr, uint16_t t_ws_port = 0);

    /**
     * @brief Store the bound references used by registerRoutes().
     *
     * start() calls this internally; the unified gateway path calls it
     * explicitly before registerRoutes().
     */
    void configure(const PlcSchemaStore& t_registry, PlcMemory& t_memory, std::shared_ptr<sgrn::gateway::database::GatewayDatabase> tsp_db,
        std::shared_ptr<::sgrn::gateway::SecurityManager> tsp_security, const ::sgrn::scl::ModbusVirtualMap* tp_modbus_map = nullptr);

    /**
     * @brief Register all HTTP routes on an externally owned Crow app.
     *
     * Used by the unified gateway path: HTTP and WebSocket routes are
     * registered on the SAME app before its listener starts, so both share
     * one port. The referenced objects (registry/memory/db/...) must outlive
     * the server. Must be called before the app starts listening.
     */
    void registerRoutes(crow::SimpleApp& t_app);

    void stop();

    /// Bound references — set by start()/registerRoutes() before any handler runs.
    struct BoundRefs {
        const PlcSchemaStore* registry{nullptr};
        PlcMemory* memory{nullptr};
        std::shared_ptr<sgrn::gateway::database::GatewayDatabase> db;
    };
    const BoundRefs& boundRefs() const {
        return refs_;
    }

private:
    // ── ACL helpers ─────────────────────────────────────────────────────────
    bool isAuthorized(const http::HttpRequest& t_req, std::optional<uint16_t> t_db_number = std::nullopt) const;
    bool isAuthorizedField(
        const http::HttpRequest& t_req, std::optional<uint16_t> t_db_number, const std::string& t_field_path, bool t_is_write) const;

    // ── HTTP Handlers ────────────────────────────────────────────────────────
    // Semantic (schema-aware) endpoints: /data/*
    void handleGetData(const http::HttpRequest& t_req, http::HttpResponse& t_res);
    void handlePost(const http::HttpRequest& t_req, http::HttpResponse& t_res);
    void handlePut(const http::HttpRequest& t_req, http::HttpResponse& t_res);

    // Raw memory endpoints: /memory/*
    // Binary mode (single DB, raw bytes):
    void handleGetMemoryBinary(const http::HttpRequest& t_req, http::HttpResponse& t_res);
    void handlePutMemoryBinary(const http::HttpRequest& t_req, http::HttpResponse& t_res);
    // Batch mode (multiple DBs, base64url JSON):
    void handlePutMemoryBatch(const http::HttpRequest& t_req, http::HttpResponse& t_res);

    // Registry and diagnostic endpoints
    void handleGetRegistry(const http::HttpRequest& t_req, http::HttpResponse& t_res);
    void handleGetModbusRegistry(const http::HttpRequest& t_req, http::HttpResponse& t_res);
    void handleGetRegistryTypes(const http::HttpRequest& t_req, http::HttpResponse& t_res);
    void handleGetConnections(const http::HttpRequest& t_req, http::HttpResponse& t_res);
    void handleGetDbHistory(const http::HttpRequest& t_req, http::HttpResponse& t_res);
    void handleGetDbSessions(const http::HttpRequest& t_req, http::HttpResponse& t_res);
    void handleGetDbLogs(const http::HttpRequest& t_req, http::HttpResponse& t_res);
    void handleGetEndpoints(const http::HttpRequest& t_req, http::HttpResponse& t_res);
    void handleGetPolicy(const http::HttpRequest& t_req, http::HttpResponse& t_res);
    void registerWebAssets(crow::SimpleApp& t_app);

    // ── Server internals ─────────────────────────────────────────────────────
    // Standalone listener, used only by start(). The unified gateway path
    // registers routes on an external app instead (registerRoutes()).
    std::unique_ptr<northbound::NorthboundServer> server_;
    std::atomic<bool> running_{false};

    // Live telemetry cache removed (Tier 4: Unified via TreeCacheEngine)

    std::shared_ptr<::sgrn::gateway::SecurityManager> security_manager_;

    // Modbus virtual map for REST discovery
    const ::sgrn::scl::ModbusVirtualMap* modbus_map_{nullptr};

    // Route handlers run against these bound references (no per-request args).
    BoundRefs refs_;
};

} // namespace sgrn::gateway::adapters
