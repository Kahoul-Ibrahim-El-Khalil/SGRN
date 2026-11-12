#include <sgrn/utils/json-dom.hpp>
/*sgrn/gateway/adapters/http/http.cpp*/
#include <fmt/core.h>
#include <sgrn/common/path-utils.hpp>
#include <sgrn/gateway/adapters/http.hpp>
#include <sgrn/gateway/adapters/rate-limit.hpp>
#include <sgrn/gateway/common/SecurityHelper.hpp>
#include <sgrn/gateway/core/TelemetryBroker.hpp>
#include <sgrn/gateway/core/snapshot.hpp>
#include <sgrn/gateway/database/GatewayDatabase.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/TypeDictionary.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/utils/json-serialization.hpp>
#include <chrono>

using sgrn::gateway::common::SecurityHelper;
namespace sgrn::gateway::adapters
{

namespace
{

struct ReplayStatusResponse {
    double speed;
    bool paused;
    bool unpaced;
    uint64_t frames;
    int64_t ts;
};

struct ApiErrorResponse {
    std::string error;
};

} // namespace

namespace
{

void setJsonError(auto& t_response, std::string t_message) {
    auto json = sgrn::json::write(ApiErrorResponse{std::move(t_message)});
    if (json.hasError()) {
        t_response.status = 500;
        t_response.set_content(R"JSON({"error":"failed to serialize error response"})JSON", "application/json");
        return;
    }
    t_response.set_content(json.value(), "application/json");
}

} // namespace

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
    RETURN_ERROR_IF(running_.load(std::memory_order_acquire), "HttpAdapter: already running");

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

void HttpAdapter::registerRoutes(GatewayApp& t_app) {
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
     * DISCRETE TAGS (TIA-style tag tables; needs runtime hookup, else empty):
     * [GET] /tags
     *   Tag table listing: [{"name","table","type","address"}].
     *
     * [GET] /tags/{name}
     *   Current tag value as JSON (404 unknown).
     *
     * [POST] /tags/{name}  /  [PUT] /tags/{name}
     *   Write JSON value (raw scalars accepted; UDT partials merge).
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
    CROW_ROUTE(t_app, "/registry/types")
        .CROW_MIDDLEWARES(t_app, RateLimitMiddleware)
        .methods("GET"_method, "OPTIONS"_method)(
            [serve](const crow::request& t_req) { return serve(t_req, "", &HttpAdapter::handleGetRegistryTypes); });

    CROW_ROUTE(t_app, "/registry/modbus")
        .CROW_MIDDLEWARES(t_app, RateLimitMiddleware)
        .methods("GET"_method, "OPTIONS"_method)(
            [serve](const crow::request& t_req) { return serve(t_req, "", &HttpAdapter::handleGetModbusRegistry); });

    CROW_ROUTE(t_app, "/registry")
        .CROW_MIDDLEWARES(t_app, RateLimitMiddleware)
        .methods("GET"_method, "OPTIONS"_method)(
            [serve](const crow::request& t_req) { return serve(t_req, "", &HttpAdapter::handleGetRegistry); });

    // ── Semantic data API (Crow <path> captures the remainder incl. '/') ────
    CROW_ROUTE(t_app, "/data/<path>")
        .CROW_MIDDLEWARES(t_app, RateLimitMiddleware)
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

    CROW_ROUTE(t_app, "/data/")
        .CROW_MIDDLEWARES(t_app, RateLimitMiddleware)
        .methods("GET"_method, "POST"_method, "PUT"_method, "OPTIONS"_method)(data_root);

    // ── Discrete tags (TIA-style; see tags-handlers.cpp) ────────────────────
    //   GET  /tags            — tag table listing (name/table/type/address)
    //   GET  /tags/<name>     — current value as JSON (404 unknown)
    //   POST /tags/<name>     — write JSON value (UDT partials merge)
    //   PUT  /tags/<name>     — same write path as POST
    CROW_ROUTE(t_app, "/tags")
        .CROW_MIDDLEWARES(t_app, RateLimitMiddleware)
        .methods("GET"_method, "OPTIONS"_method)(
            [serve](const crow::request& t_req) { return serve(t_req, "", &HttpAdapter::handleGetTags); });
    CROW_ROUTE(t_app, "/tags/<string>")
        .CROW_MIDDLEWARES(t_app, RateLimitMiddleware)
        .methods("GET"_method, "POST"_method, "PUT"_method, "OPTIONS"_method)([serve](const crow::request& t_req, std::string t_sub) {
            if (t_req.method == "POST"_method || t_req.method == "PUT"_method)
                return serve(t_req, std::move(t_sub), &HttpAdapter::handleWriteTag);
            return serve(t_req, std::move(t_sub), &HttpAdapter::handleGetTag);
        });

    // ── Raw Memory API ───────────────────────────────────────────────────────
    CROW_ROUTE(t_app, "/memory/db/<path>")
        .CROW_MIDDLEWARES(t_app, RateLimitMiddleware)
        .methods("GET"_method, "PUT"_method, "OPTIONS"_method)([serve](const crow::request& t_req, std::string t_sub) {
            if (t_req.method == "PUT"_method)
                return serve(t_req, std::move(t_sub), &HttpAdapter::handlePutMemoryBinary);
            return serve(t_req, std::move(t_sub), &HttpAdapter::handleGetMemoryBinary);
        });
    CROW_ROUTE(t_app, "/memory/batch")
        .CROW_MIDDLEWARES(t_app, RateLimitMiddleware)
        .methods("PUT"_method, "OPTIONS"_method)(
            [serve](const crow::request& t_req) { return serve(t_req, "", &HttpAdapter::handlePutMemoryBatch); });

    // ── Diagnostics ──────────────────────────────────────────────────────────
    CROW_ROUTE(t_app, "/connections")
        .CROW_MIDDLEWARES(t_app, RateLimitMiddleware)
        .methods("GET"_method, "OPTIONS"_method)(
            [serve](const crow::request& t_req) { return serve(t_req, "", &HttpAdapter::handleGetConnections); });
    CROW_ROUTE(t_app, "/db/history")
        .CROW_MIDDLEWARES(t_app, RateLimitMiddleware)
        .methods("GET"_method, "OPTIONS"_method)(
            [serve](const crow::request& t_req) { return serve(t_req, "", &HttpAdapter::handleGetDbHistory); });
    CROW_ROUTE(t_app, "/db/sessions")
        .CROW_MIDDLEWARES(t_app, RateLimitMiddleware)
        .methods("GET"_method, "OPTIONS"_method)(
            [serve](const crow::request& t_req) { return serve(t_req, "", &HttpAdapter::handleGetDbSessions); });
    CROW_ROUTE(t_app, "/db/logs")
        .CROW_MIDDLEWARES(t_app, RateLimitMiddleware)
        .methods("GET"_method, "OPTIONS"_method)(
            [serve](const crow::request& t_req) { return serve(t_req, "", &HttpAdapter::handleGetDbLogs); });
    CROW_ROUTE(t_app, "/endpoints")
        .CROW_MIDDLEWARES(t_app, RateLimitMiddleware)
        .methods("GET"_method, "OPTIONS"_method)(
            [serve](const crow::request& t_req) { return serve(t_req, "", &HttpAdapter::handleGetEndpoints); });

    // ── Security policy introspection ────────────────────────────────────────
    CROW_ROUTE(t_app, "/api/policy")
        .CROW_MIDDLEWARES(t_app, RateLimitMiddleware)
        .methods("GET"_method, "OPTIONS"_method)(
            [serve](const crow::request& t_req) { return serve(t_req, "", &HttpAdapter::handleGetPolicy); });

    // ── Replay pacing (sgrn_replay only; handlers 404 without control) ──────
    CROW_ROUTE(t_app, "/replay/status")
        .CROW_MIDDLEWARES(t_app, RateLimitMiddleware)
        .methods("GET"_method, "OPTIONS"_method)(
            [serve](const crow::request& t_req) { return serve(t_req, "", &HttpAdapter::handleGetReplayStatus); });
    CROW_ROUTE(t_app, "/replay/speed")
        .CROW_MIDDLEWARES(t_app, RateLimitMiddleware)
        .methods("POST"_method, "OPTIONS"_method)(
            [serve](const crow::request& t_req) { return serve(t_req, "", &HttpAdapter::handlePostReplaySpeed); });
    CROW_ROUTE(t_app, "/replay/pause")
        .CROW_MIDDLEWARES(t_app, RateLimitMiddleware)
        .methods("POST"_method, "OPTIONS"_method)(
            [serve](const crow::request& t_req) { return serve(t_req, "", &HttpAdapter::handlePostReplayPause); });
    CROW_ROUTE(t_app, "/replay/resume")
        .CROW_MIDDLEWARES(t_app, RateLimitMiddleware)
        .methods("POST"_method, "OPTIONS"_method)(
            [serve](const crow::request& t_req) { return serve(t_req, "", &HttpAdapter::handlePostReplayResume); });

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

void HttpAdapter::handleGetReplayStatus(const http::HttpRequest&, http::HttpResponse& t_res) {
    if (!replay_control_) {
        t_res.status = 404;
        setJsonError(t_res, "replay control unavailable");
        return;
    }
    const double speed = replay_control_->speed.load(std::memory_order_relaxed);
    const bool paused = replay_control_->paused.load(std::memory_order_relaxed);
    const bool unpaced = replay_control_->unpaced.load(std::memory_order_relaxed);
    const int64_t ts = replay_control_->cur_ts.load(std::memory_order_relaxed);
    const uint64_t frames = replay_control_->frames.load(std::memory_order_relaxed);
    const auto response = sgrn::json::write(ReplayStatusResponse{speed, paused, unpaced, frames, ts});
    if (response.hasError()) {
        t_res.status = 500;
        t_res.set_content(R"JSON({"error":"failed to serialize replay status"})JSON", "application/json");
        return;
    }
    t_res.set_content(response.value(), "application/json");
}

void HttpAdapter::handlePostReplaySpeed(const http::HttpRequest& t_req, http::HttpResponse& t_res) {
    if (!replay_control_) {
        t_res.status = 404;
        setJsonError(t_res, "replay control unavailable");
        return;
    }
    sgrn::json::dom::Document doc;
    if (doc.parse(t_req.body.c_str()).hasParseError() || !doc.is_object() || !doc.hasMember("speed")) {
        t_res.status = 400;
        setJsonError(t_res, "expected JSON body with speed field");
        return;
    }
    const auto& v = doc["speed"];
    if (v.is_string() && std::string_view(v.getString()) == "unpaced") {
        replay_control_->unpaced.store(true, std::memory_order_relaxed);
    } else if (v.is_number()) {
        const double speed = v.getDouble();
        if (!(speed > 0) || speed > 1e9) {
            t_res.status = 400;
            setJsonError(t_res, "speed must be a positive number");
            return;
        }
        replay_control_->unpaced.store(false, std::memory_order_relaxed);
        replay_control_->speed.store(speed, std::memory_order_relaxed);
    } else {
        t_res.status = 400;
        setJsonError(t_res, "speed must be a number or unpaced");
        return;
    }
    handleGetReplayStatus(t_req, t_res);
}

void HttpAdapter::handlePostReplayPause(const http::HttpRequest& t_req, http::HttpResponse& t_res) {
    if (!replay_control_) {
        t_res.status = 404;
        setJsonError(t_res, "replay control unavailable");
        return;
    }
    replay_control_->paused.store(true, std::memory_order_relaxed);
    handleGetReplayStatus(t_req, t_res);
}

void HttpAdapter::handlePostReplayResume(const http::HttpRequest& t_req, http::HttpResponse& t_res) {
    if (!replay_control_) {
        t_res.status = 404;
        setJsonError(t_res, "replay control unavailable");
        return;
    }
    replay_control_->paused.store(false, std::memory_order_relaxed);
    handleGetReplayStatus(t_req, t_res);
}

// ── ACL helper — delegates to the shared SecurityHelper ─────────────────────
bool HttpAdapter::isAuthorized(const http::HttpRequest& t_req, std::optional<uint16_t> t_db_number) const {
    RETURN_IF(!security_manager_, true);

    // SecurityHelper does not forward headers for connection-auth; call the
    // manager directly to preserve the header-names parameter.
    return security_manager_->authorizeHttp(t_req.remote_ip, t_req.get_header_value("Origin"), t_req.headerNames(), t_db_number);
}

bool HttpAdapter::isAuthorizedField(
    const http::HttpRequest& t_req, std::optional<uint16_t> t_db_number, const std::string& t_field_path, bool t_is_write) const {
    // Schema-level #READ_ONLY beats policy: a read-only signal rejects
    // semantic writes regardless of ACLs. Unknown/unresolvable paths fall
    // through to the SecurityManager (fail-open, as before). Raw
    // /memory/* writes are byte-level S7 semantics and stay DB-ACL only.
    if (t_is_write && t_db_number.has_value() && refs_.registry) {
        RETURN_IF(auto loc = refs_.registry->findField(*t_db_number, t_field_path);
            loc.has_value() && loc->field && loc->field->is_read_only, false);
    }
    RETURN_IF(!security_manager_, true);

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
