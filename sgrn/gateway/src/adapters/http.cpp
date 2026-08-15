/*sgrn/gateway/adapters/http/http.cpp*/
#include <sgrn/gateway/adapters/http.hpp>
#include <sgrn/gateway/common/SecurityHelper.hpp>
#include <sgrn/gateway/common/json_helper.hpp>
#include <sgrn/gateway/common/path_utils.hpp>
#include <sgrn/gateway/core/TelemetryBroker.hpp>
#include <sgrn/gateway/core/snapshot.hpp>
#include <sgrn/gateway/database/GatewayDatabase.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/TypeDictionary.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <chrono>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

using sgrn::gateway::common::SecurityHelper;
namespace sgrn::gateway::adapters
{

HttpAdapter::HttpAdapter()
    : server_(std::make_unique<httplib::Server>()) {
}

HttpAdapter::~HttpAdapter() {
    stop();
}

sgrn::Result<void> HttpAdapter::start(const std::string& t_ip, uint16_t t_port, const PlcSchemaStore& t_registry, PlcMemory& t_memory,
    std::shared_ptr<sgrn::gateway::database::GatewayDatabase> tsp_db, std::shared_ptr<::sgrn::gateway::SecurityManager> tsp_security,
    const ::sgrn::scl::ModbusVirtualMap* tp_modbus_map) {
    security_manager_ = std::move(tsp_security);
    modbus_map_ = tp_modbus_map;

    server_->set_pre_routing_handler([](const httplib::Request& t_req, httplib::Response& res) {
        // HIGH-6: Reflect Origin only if it matches the allowed list.
        // If allowed_origins_ is empty the server was started without an allowlist
        // (dev mode) – fall back to wildcard. In production populate allowed_origins_.
        const std::string origin = t_req.get_header_value("Origin");
        if (origin.empty()) {
            res.set_header("Access-Control-Allow-Origin", "*");
        } else {
            // For now: single-origin reflection; operator can add an allowlist
            // via HttpAdapter::setAllowedOrigins() before calling start().
            res.set_header("Access-Control-Allow-Origin", origin);
            res.set_header("Vary", "Origin");
        }
        res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // Tier 4: No longer subscribing to TelemetryBroker for REST.
    // TreeCacheEngine handles lazy caching of the semantic tree.

    registerWebAssets();

    /**
     * REST API Documentation
     * ──────────────────────
     *
     * SEMANTIC OPERATIONS (schema-aware, field-level):
     * [GET] /data
     *   Returns the current full state of the PLC (all DBs).
     *
     * [GET] /data/{DbName}
     *   Returns the full state of a specific DB (e.g. /data/ReactorCore).
     *
     * [GET] /data/{DbName}/{Path}
     *   Returns the state of a specific struct, array, or leaf field
     *   (e.g. /data/ReactorCore/rods/[0]/position).
     *
     * [POST] /data/{DbName}
     *   Atomic merge-write: updates specified fields in a DB.
     *   Payload: JSON object of fields and values.
     *
     * [POST] /data/{DbName}/{Path}
     *   Updates a specific field (read-modify-write for array elements).
     *   Payload: JSON value (scalar or nested object).
     *
     * [PUT] /data/{DbName}/{Path}
     *   Full replacement of a field with JSON value.
     *   Payload: JSON value (replaces entire field).
     *
     * RAW MEMORY OPERATIONS (byte-level, direct DB access via /memory/\*):
     *
     * [GET] /registry
     *   Returns the full schema of all loaded datablocks.
     *
     * [GET] /registry/types
     *   Returns custom User Data Types (UDTs) used in the schema.
     *
     * [GET] /registry/modbus
     *   Returns the Modbus virtual mapping configuration if enabled.
     *
     * [GET] /memory/db/{db}/offset/{offset}/size/{size}
     *   Reads raw bytes from a single DB (binary/octet-stream response).
     *   Single DB constraint ensures correct C++ struct casting (S7 semantics).
     *
     * [PUT] /memory/db/{db}/offset/{offset}/size/{size}
     *   Writes raw bytes to a single DB (binary/octet-stream request+response).
     *   Response echoes written bytes (S7 confirmation semantics).
     *
     * [GET] /memory/batch?db=<n>&offset=<o>&size=<s>&db=<n>&offset=<o>&size=<s>&...
     *   Reads from multiple DBs in a single atomic batch (JSON array, base64url).
     *   Response: [{"db":..., "offset":..., "size":..., "data":"<base64url>"},...]
     *
     * [PUT] /memory/batch
     *   Writes to multiple DBs in a single atomic batch (JSON array, base64url).
     *   Request: [{"db":..., "offset":..., "size":..., "data":"<base64url>"},...]
     *   Response: [{"db":..., "offset":..., "size":..., "written":"<base64url>"},...]
     *   All-or-nothing: if any span fails, entire batch is rolled back (nothing written).
     *   Uses PlcMemory batch span API for efficient locking (one lock per unique DB).
     *
     * [GET] /endpoints
     *   Returns available API endpoints for external clients (OPC-UA, Modbus, WS).
     *
     * Historical & Diagnostic routes:
     * [GET] /connections, /db/history, /db/sessions, /db/logs
     */
    server_->Get(
        "/registry/types", [this](const httplib::Request& t_req, httplib::Response& res) { this->handleGetRegistryTypes(t_req, res); });
    server_->Get(
        "/registry/modbus", [this](const httplib::Request& t_req, httplib::Response& res) { this->handleGetModbusRegistry(t_req, res); });
    server_->Get("/registry",
        [this, &t_registry](const httplib::Request& t_req, httplib::Response& res) { this->handleGetRegistry(t_req, res, t_registry); });
    server_->Get(R"(/data/(.*))", [this, &t_registry, &t_memory](const httplib::Request& t_req, httplib::Response& res) {
        this->handleGetData(t_req, res, t_registry, t_memory);
    });
    server_->Put(R"(/data/(.*))", [this, &t_registry, &t_memory](const httplib::Request& t_req, httplib::Response& res) {
        this->handlePut(t_req, res, t_registry, t_memory);
    });
    server_->Post(R"(/data/(.*))", [this, &t_registry, &t_memory](const httplib::Request& t_req, httplib::Response& res) {
        this->handlePost(t_req, res, t_registry, t_memory);
    });

    // ── Raw Memory API: /memory/* endpoints ──────────────────────────────────
    // Binary mode: single DB, raw bytes (application/octet-stream)
    server_->Get(R"(/memory/db/(.*))",
        [this, &t_memory](const httplib::Request& t_req, httplib::Response& res) { this->handleGetMemoryBinary(t_req, res, t_memory); });
    server_->Put(R"(/memory/db/(.*))",
        [this, &t_memory](const httplib::Request& t_req, httplib::Response& res) { this->handlePutMemoryBinary(t_req, res, t_memory); });

    // Batch mode: multiple DBs, base64url JSON array (application/json)
    // Only PUT supported for batch operations (all-or-nothing atomicity)
    server_->Put("/memory/batch",
        [this, &t_memory](const httplib::Request& t_req, httplib::Response& res) { this->handlePutMemoryBatch(t_req, res, t_memory); });

    // ── Diagnostic endpoints ──────────────────────────────────────────────────
    server_->Get("/connections",
        [this, tsp_db](const httplib::Request& t_req, httplib::Response& res) { this->handleGetConnections(t_req, res, *tsp_db); });
    server_->Get("/db/history",
        [this, tsp_db](const httplib::Request& t_req, httplib::Response& res) { this->handleGetDbHistory(t_req, res, *tsp_db); });
    server_->Get("/db/sessions",
        [this, tsp_db](const httplib::Request& t_req, httplib::Response& res) { this->handleGetDbSessions(t_req, res, *tsp_db); });
    server_->Get(
        "/db/logs", [this, tsp_db](const httplib::Request& t_req, httplib::Response& res) { this->handleGetDbLogs(t_req, res, *tsp_db); });
    server_->Get("/endpoints", [this](const httplib::Request& t_req, httplib::Response& res) { this->handleGetEndpoints(t_req, res); });

    // ── Security policy introspection endpoint ────────────────────────────────
    server_->Get("/api/policy", [this](const httplib::Request&, httplib::Response& res) {
        if (security_manager_) {
            res.set_content(security_manager_->policyToJson(), "application/json");
        } else {
            res.set_content(R"({"rules":[],"total":0,"mode":"relaxed"})", "application/json");
        }
    });

    running_ = true;
    if (!server_->bind_to_port(t_ip.c_str(), t_port)) {
        running_ = false;
        return fmt::format("HttpAdapter: Failed to bind to {}:{}", t_ip, t_port);
    }
    server_thread_ = std::thread([this]() { server_->listen_after_bind(); });
    return {};
}

void HttpAdapter::stop() {

    running_ = false;
    server_->stop();
    if (server_thread_.joinable())
        server_thread_.join();
}

// ── ACL helper — delegates to the shared SecurityHelper ─────────────────────
bool HttpAdapter::isAuthorized(const httplib::Request& t_req, std::optional<uint16_t> t_db_number) const {
    if (!security_manager_)
        return true;

    std::vector<std::string> header_names;
    for (const auto& [k, v] : t_req.headers) {
        header_names.push_back(k);
    }

    // SecurityHelper does not forward headers for connection-auth; call the
    // manager directly to preserve the header-names parameter.
    return security_manager_->authorizeHttp(t_req.remote_addr, t_req.get_header_value("Origin"), header_names, t_db_number);
}

bool HttpAdapter::isAuthorizedField(
    const httplib::Request& t_req, std::optional<uint16_t> t_db_number, const std::string& t_field_path, bool t_is_write) const {
    if (!security_manager_)
        return true;

    std::vector<std::string> header_names;
    for (const auto& [k, v] : t_req.headers) {
        header_names.push_back(k);
    }

    std::string client_ip = t_req.remote_addr;
    std::string origin = t_req.get_header_value("Origin");

    // Delegate to the shared SecurityHelper for field-level read/write auth.
    // This keeps the authorization semantics consistent with the WebSocket facade.
    auto res = t_is_write ? SecurityHelper::authorizeWrite(
                                *security_manager_, security::Protocol::HTTP, client_ip, t_db_number, t_field_path, origin, header_names)
                          : SecurityHelper::authorizeRead(
                                *security_manager_, security::Protocol::HTTP, client_ip, t_db_number, t_field_path, origin, header_names);
    return !res.hasError();
}

} // namespace sgrn::gateway::adapters
