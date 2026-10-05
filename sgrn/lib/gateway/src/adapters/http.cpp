/*sgrn/gateway/adapters/http/http.cpp*/
#include <fmt/core.h>
#include <sgrn/common/json_helper.hpp>
#include <sgrn/common/path_utils.hpp>
#include <sgrn/gateway/adapters/http.hpp>
#include <sgrn/gateway/adapters/rate_limit.hpp>
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
        t_res.set_content(R"JSON({"error":"replay control unavailable"})JSON", "application/json");
        return;
    }
    const double speed = replay_control_->speed.load(std::memory_order_relaxed);
    const bool paused = replay_control_->paused.load(std::memory_order_relaxed);
    const bool unpaced = replay_control_->unpaced.load(std::memory_order_relaxed);
    const int64_t ts = replay_control_->cur_ts.load(std::memory_order_relaxed);
    const uint64_t frames = replay_control_->frames.load(std::memory_order_relaxed);
    t_res.set_content(fmt::format(R"({{"speed":{},"paused":{},"unpaced":{},"frames":{},"ts":{}}})", speed, paused ? "true" : "false",
                          unpaced ? "true" : "false", frames, ts),
        "application/json");
}

void HttpAdapter::handlePostReplaySpeed(const http::HttpRequest& t_req, http::HttpResponse& t_res) {
    if (!replay_control_) {
        t_res.status = 404;
        t_res.set_content(R"JSON({"error":"replay control unavailable"})JSON", "application/json");
        return;
    }
    rapidjson::Document doc;
    if (doc.Parse(t_req.body.c_str()).HasParseError() || !doc.IsObject() || !doc.HasMember("speed")) {
        t_res.status = 400;
        t_res.set_content(R"JSON({"error":"expected JSON body with speed field"})JSON", "application/json");
        return;
    }
    const auto& v = doc["speed"];
    if (v.IsString() && std::string_view(v.GetString()) == "unpaced") {
        replay_control_->unpaced.store(true, std::memory_order_relaxed);
    } else if (v.IsNumber()) {
        const double speed = v.GetDouble();
        if (!(speed > 0) || speed > 1e9) {
            t_res.status = 400;
            t_res.set_content(R"JSON({"error":"speed must be a positive number"})JSON", "application/json");
            return;
        }
        replay_control_->unpaced.store(false, std::memory_order_relaxed);
        replay_control_->speed.store(speed, std::memory_order_relaxed);
    } else {
        t_res.status = 400;
        t_res.set_content(R"JSON({"error":"speed must be a number or unpaced"})JSON", "application/json");
        return;
    }
    handleGetReplayStatus(t_req, t_res);
}

void HttpAdapter::handlePostReplayPause(const http::HttpRequest& t_req, http::HttpResponse& t_res) {
    if (!replay_control_) {
        t_res.status = 404;
        t_res.set_content(R"JSON({"error":"replay control unavailable"})JSON", "application/json");
        return;
    }
    replay_control_->paused.store(true, std::memory_order_relaxed);
    handleGetReplayStatus(t_req, t_res);
}

void HttpAdapter::handlePostReplayResume(const http::HttpRequest& t_req, http::HttpResponse& t_res) {
    if (!replay_control_) {
        t_res.status = 404;
        t_res.set_content(R"JSON({"error":"replay control unavailable"})JSON", "application/json");
        return;
    }
    replay_control_->paused.store(false, std::memory_order_relaxed);
    handleGetReplayStatus(t_req, t_res);
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
