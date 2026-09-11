/*sgrn/gateway/adapters/http/http.cpp*/
#include <sgrn/common/json_helper.hpp>
#include <sgrn/common/path_utils.hpp>
#include <sgrn/gateway/adapters/http.hpp>
#include <sgrn/gateway/common/SecurityHelper.hpp>
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

HttpAdapter::HttpAdapter() = default;

HttpAdapter::~HttpAdapter() {
    stop();
}

sgrn::Result<void> HttpAdapter::start(const std::string& t_ip, uint16_t t_port, const PlcSchemaStore& t_registry, PlcMemory& t_memory,
    std::shared_ptr<sgrn::gateway::database::GatewayDatabase> tsp_db, std::shared_ptr<::sgrn::gateway::SecurityManager> tsp_security,
    const ::sgrn::scl::ModbusVirtualMap* tp_modbus_map, uint16_t /*t_ws_port*/) {
    // NOTE: t_ws_port is accepted for source compatibility but ignored: since
    // the Crow migration the WebSocket endpoint (`/ws`) shares this same HTTP
    // listener instead of living on a separate port.
    if (running_.load(std::memory_order_acquire))
        return sgrn::Result<void>::Error("HttpAdapter: already running");

    configure(t_registry, t_memory, std::move(tsp_db), std::move(tsp_security), tp_modbus_map);

    // Tier 4: No longer subscribing to TelemetryBroker for REST.
    // TreeCacheEngine handles lazy caching of the semantic tree.

    server_ = std::make_unique<northbound::NorthboundServer>();
    registerRoutes(server_->app());
    if (auto r = server_->start(t_ip, t_port); r.hasError()) {
        server_.reset();
        return fmt::format("HttpAdapter: {}", r.error());
    }
    running_.store(true, std::memory_order_release);
    return {};
}

void HttpAdapter::configure(const PlcSchemaStore& t_registry, PlcMemory& t_memory,
    std::shared_ptr<sgrn::gateway::database::GatewayDatabase> tsp_db, std::shared_ptr<::sgrn::gateway::SecurityManager> tsp_security,
    const ::sgrn::scl::ModbusVirtualMap* tp_modbus_map) {
    security_manager_ = std::move(tsp_security);
    modbus_map_ = tp_modbus_map;
    refs_.registry = &t_registry;
    refs_.memory = &t_memory;
    refs_.db = std::move(tsp_db);
}

void HttpAdapter::registerRoutes(crow::SimpleApp& t_app) {
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

    // Small local helper: translate HttpRequest -> handler -> crow::response.
    auto serve = [this](const crow::request& t_crow_req, std::string t_captured,
                     void (HttpAdapter::*t_handler)(const http::HttpRequest&, http::HttpResponse&)) {
        http::HttpRequest req = http::fromCrowRequest(t_crow_req, std::move(t_captured));
        http::HttpResponse res;
        (this->*t_handler)(req, res);
        crow::response crow_res;
        http::applyToCrowResponse(res, crow_res);
        http::applyCors(crow_res, t_crow_req);
        return crow_res;
    };

    // ── Registry ─────────────────────────────────────────────────────────────
    CROW_ROUTE(t_app, "/registry/types").methods("GET"_method, "OPTIONS"_method)([serve](const crow::request& t_req) {
        return serve(t_req, "", &HttpAdapter::handleGetRegistryTypes);
    });
    CROW_ROUTE(t_app, "/registry/modbus").methods("GET"_method, "OPTIONS"_method)([serve](const crow::request& t_req) {
        return serve(t_req, "", &HttpAdapter::handleGetModbusRegistry);
    });
    CROW_ROUTE(t_app, "/registry").methods("GET"_method, "OPTIONS"_method)([serve](const crow::request& t_req) {
        return serve(t_req, "", &HttpAdapter::handleGetRegistry);
    });

    // ── Semantic data API (Crow <path> captures the remainder incl. '/') ────
    CROW_ROUTE(t_app, "/data/<path>")
        .methods("GET"_method, "POST"_method, "PUT"_method, "OPTIONS"_method)([serve](const crow::request& t_req, std::string t_sub) {
            if (t_req.method == "POST"_method)
                return serve(t_req, std::move(t_sub), &HttpAdapter::handlePost);
            if (t_req.method == "PUT"_method)
                return serve(t_req, std::move(t_sub), &HttpAdapter::handlePut);
            return serve(t_req, std::move(t_sub), &HttpAdapter::handleGetData);
        });
    // Bare /data and /data/ (full-twin read / multi-DB merge-write).
    // NOTE: only /data/ is registered: Crow auto-serves the slashless form
    // with a 301 redirect to /data/ (registering both collides in the trie).
    auto data_root = [serve](const crow::request& t_req) {
        if (t_req.method == "POST"_method)
            return serve(t_req, "", &HttpAdapter::handlePost);
        if (t_req.method == "PUT"_method)
            return serve(t_req, "", &HttpAdapter::handlePut);
        return serve(t_req, "", &HttpAdapter::handleGetData);
    };
    CROW_ROUTE(t_app, "/data/").methods("GET"_method, "POST"_method, "PUT"_method, "OPTIONS"_method)(data_root);

    // ── Raw Memory API ───────────────────────────────────────────────────────
    CROW_ROUTE(t_app, "/memory/db/<path>")
        .methods("GET"_method, "PUT"_method, "OPTIONS"_method)([serve](const crow::request& t_req, std::string t_sub) {
            if (t_req.method == "PUT"_method)
                return serve(t_req, std::move(t_sub), &HttpAdapter::handlePutMemoryBinary);
            return serve(t_req, std::move(t_sub), &HttpAdapter::handleGetMemoryBinary);
        });
    CROW_ROUTE(t_app, "/memory/batch").methods("PUT"_method, "OPTIONS"_method)([serve](const crow::request& t_req) {
        return serve(t_req, "", &HttpAdapter::handlePutMemoryBatch);
    });

    // ── Diagnostics ──────────────────────────────────────────────────────────
    CROW_ROUTE(t_app, "/connections").methods("GET"_method, "OPTIONS"_method)([serve](const crow::request& t_req) {
        return serve(t_req, "", &HttpAdapter::handleGetConnections);
    });
    CROW_ROUTE(t_app, "/db/history").methods("GET"_method, "OPTIONS"_method)([serve](const crow::request& t_req) {
        return serve(t_req, "", &HttpAdapter::handleGetDbHistory);
    });
    CROW_ROUTE(t_app, "/db/sessions").methods("GET"_method, "OPTIONS"_method)([serve](const crow::request& t_req) {
        return serve(t_req, "", &HttpAdapter::handleGetDbSessions);
    });
    CROW_ROUTE(t_app, "/db/logs").methods("GET"_method, "OPTIONS"_method)([serve](const crow::request& t_req) {
        return serve(t_req, "", &HttpAdapter::handleGetDbLogs);
    });
    CROW_ROUTE(t_app, "/endpoints").methods("GET"_method, "OPTIONS"_method)([serve](const crow::request& t_req) {
        return serve(t_req, "", &HttpAdapter::handleGetEndpoints);
    });

    // ── Security policy introspection ────────────────────────────────────────
    CROW_ROUTE(t_app, "/api/policy").methods("GET"_method, "OPTIONS"_method)([serve](const crow::request& t_req) {
        return serve(t_req, "", &HttpAdapter::handleGetPolicy);
    });

    registerWebAssets(t_app);
}

void HttpAdapter::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel))
        return;
    if (server_)
        server_->stop();
    server_.reset();
}

void HttpAdapter::handleGetPolicy(const http::HttpRequest&, http::HttpResponse& t_res) {
    if (security_manager_) {
        t_res.set_content(security_manager_->policyToJson(), "application/json");
    } else {
        t_res.set_content(R"({"rules":[],"total":0,"mode":"relaxed"})", "application/json");
    }
}

// ── ACL helper — delegates to the shared SecurityHelper ─────────────────────
bool HttpAdapter::isAuthorized(const http::HttpRequest& t_req, std::optional<uint16_t> t_db_number) const {
    if (!security_manager_)
        return true;

    // SecurityHelper does not forward headers for connection-auth; call the
    // manager directly to preserve the header-names parameter.
    return security_manager_->authorizeHttp(t_req.remote_ip, t_req.get_header_value("Origin"), t_req.headerNames(), t_db_number);
}

bool HttpAdapter::isAuthorizedField(
    const http::HttpRequest& t_req, std::optional<uint16_t> t_db_number, const std::string& t_field_path, bool t_is_write) const {
    if (!security_manager_)
        return true;

    std::vector<std::string> header_names = t_req.headerNames();
    std::string client_ip = t_req.remote_ip;
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
