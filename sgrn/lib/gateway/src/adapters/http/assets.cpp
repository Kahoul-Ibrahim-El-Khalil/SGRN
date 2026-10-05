/*sgrn/gateway/adapters/http/assets.cpp*/
#include <sgrn/assets/EmbeddedAsset.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/gateway/adapters/http.hpp>
#include <sgrn/gateway/adapters/rate_limit.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/strings.hpp>
#include <web_assets.hpp>

#include <map>
#include <mutex>
#include <string>

namespace sgrn::gateway::adapters
{

static const std::string kEndpointsMessage = R"({
  "endpoints": [
    {"path":"/",                                    "method":"GET",  "description":"Web dashboard (replay variant on replay gateways; full gateway dashboard otherwise)."},
    {"path":"/registry/types",                      "method":"GET",  "description":"S7 type dictionary."},
    {"path":"/registry",                            "method":"GET",  "description":"Raw S7 memory layout and semantic mapping."},
    {"path":"/data/",                               "method":"GET",  "description":"Full Digital Twin state as nested JSON (semantic)."},
    {"path":"/data/<path>",                         "method":"GET",  "description":"Specific DB or field (e.g. /data/Mixer/speed) — schema-aware."},
    {"path":"/data/<path>/<N>",                     "method":"GET",  "description":"Single array element by zero-based index (e.g. /data/DB2/temperatures/2) — semantic."},
    {"path":"/data/<path>",                         "method":"POST", "description":"JSON write to a field or subtree (partial merge, atomic). Missing fields unchanged."},
    {"path":"/data/<path>/<N>",                     "method":"POST", "description":"Scalar write to a single array element by index (e.g. POST /data/DB2/temperatures/2 body=42.0)."},
    {"path":"/data/<path>",                         "method":"PUT",  "description":"Full replacement of field with JSON value."},
    {"path":"/memory/db/<db>/offset/<o>/size/<s>", "method":"GET",   "description":"Raw binary read from single DB. Response: application/octet-stream (raw bytes). S7 semantics."},
    {"path":"/memory/db/<db>/offset/<o>/size/<s>", "method":"PUT",   "description":"Raw binary write to single DB (atomic). Request+Response: application/octet-stream (raw bytes). Single DB enforces C++ struct casting."},
    {"path":"/memory/batch",                        "method":"PUT",  "description":"Atomic batch raw write (multiple DBs). Request/Response: JSON array [{db,offset,size,data:\"base64url\"}...]. All-or-nothing semantics (one lock per unique DB)."},
    {"path":"/ws",                                  "method":"WS",   "description":"WebSocket live stream — same port as HTTP. DeltaSnapshot frames, subscribe/unsubscribe commands."},
    {"path":"/connections",                         "method":"GET",  "description":"Active/recent south and north connections."},
    {"path":"/db/history",                          "method":"GET",  "description":"Full historical database as JSON."},
    {"path":"/db/sessions",                         "method":"GET",  "description":"Active and recent client sessions."},
    {"path":"/db/logs",                             "method":"GET",  "description":"Most recent system logs."},
    {"path":"/replay/status",                       "method":"GET",  "description":"Replay pacing status {speed,paused,unpaced,frames,ts}. 404 when not a replay gateway."},
    {"path":"/replay/speed",                        "method":"POST", "description":"Set replay speed {\"speed\": <number>|\"unpaced\"} (sim-sec per wall-sec)."},
    {"path":"/replay/pause",                        "method":"POST", "description":"Pause replay pacing."},
    {"path":"/replay/resume",                       "method":"POST", "description":"Resume replay pacing."},
    {"path":"/endpoints",                           "method":"GET",  "description":"This API documentation."}
  ]
})";

/*
 * Crow constructs the response from the returned crow::response object, so —
 * as with the old httplib layer — we cache only the asset payloads
 * themselves (compressed bytes + lazily decompressed bytes), never a full
 * response object.
 */
using sgrn::utils::strings::replaceAll;

namespace
{

/// Normalize the X-Forwarded-Prefix header nginx sends (see
/// sites-enabled/sgrn.conf, "/gateway"). Returns "" for direct access.
/// Strict allow-list: anything outside [A-Za-z0-9_.\-/~] (or a missing
/// leading '/') falls back to standalone so a forged header can never
/// inject markup into index.html.
std::string forwardedPrefix(const http::HttpRequest& t_req) {
    if (!t_req.has_header("X-Forwarded-Prefix"))
        return "";
    std::string prefix = t_req.get_header_value("X-Forwarded-Prefix");
    if (prefix.size() > 64 || prefix.empty() || prefix[0] != '/')
        return "";
    for (char c : prefix) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '.' ||
                        c == '-' || c == '~';
        if (!ok)
            return "";
    }
    while (prefix.size() > 1 && prefix.back() == '/')
        prefix.pop_back();
    if (prefix == "/")
        return "";
    return prefix;
}

/// Runtime head patched into index.html's <!-- SGRN_RUNTIME_HEAD --> slot.
/// The SPA resolves API/asset/WS URLs against the page's own origin + prefix
/// (ws() builds wss://host<prefix>/ws, which nginx proxies), so HTTP and WS
/// always share the same origin — no WS-port injection anymore.
std::string runtimeHead(const std::string& t_prefix) {
    if (t_prefix.empty()) {
        return R"(<base href="/">
<script>
window.__SGRN_BASE__="";
</script>)";
    }
    return std::string(R"(<base href=")") + t_prefix + R"(/">
<script>
window.__SGRN_BASE__=")" +
           t_prefix + R"(";
</script>)";
}

/// API/WS prefixes served by real routes — the SPA fallback must not swallow
/// them (a missing /data/... path is a genuine 404, not index.html).
bool isApiPath(const std::string& t_path) {
    for (const char* prefix : {"/api", "/data", "/memory", "/registry", "/endpoints", "/connections", "/db", "/ws", "/replay"}) {
        if (t_path.find(prefix) == 0)
            return true;
    }
    return false;
}

} // namespace

void HttpAdapter::registerWebAssets(GatewayApp& t_app) {
    namespace web = sgrn::gateway::assets::web;

    // Shared SPA-fallback responder for "/" and unknown non-API paths.
    auto spa_handler = std::make_shared<std::function<crow::response(const crow::request&)>>();
    auto index_handler = std::make_shared<std::function<crow::response(const crow::request&)>>();
    // Replay dashboard variant (replay.html): inherits the shared shell but
    // is NOT the gateway dashboard — no docs routes/bundle. Served at "/"
    // instead of index.html when replay pacing state is present.
    auto replay_page_handler = std::make_shared<std::function<crow::response(const crow::request&)>>();
    // Captured by value into the "/" + fallback lambdas below: when set, this
    // gateway is a replay gateway (HttpAdapter outlives route registration).
    ReplayControlPtr replay_control = replay_control_;

    for (size_t i = 0; i < web::ASSET_COUNT; ++i) {
        const auto& asset = web::ASSETS[i];
        const std::string route_path(asset.virtual_path.data());

        auto cached = std::make_shared<std::string>();
        auto flag = std::make_shared<std::once_flag>();
        auto has_error = std::make_shared<bool>(false);
        // HTML entry points only: patched variants keyed by forwarded prefix,
        // since direct (:8000) and proxied (https://host/gateway/) clients
        // need different <base>/__SGRN_BASE__ heads. Handlers run on the Crow
        // pool, so the map is mutex-guarded.
        auto variants = std::make_shared<std::map<std::string, std::string>>();
        auto variants_mutex = std::make_shared<std::mutex>();

        const bool is_entry = (route_path == "/index.html" || route_path == "/replay.html");
        auto handler = [i, cached, flag, has_error, variants, variants_mutex, is_entry](const crow::request& t_crow_req) {
            crow::response crow_res;
            // Dynamic rules cannot carry middleware — enforce manually.
            if (!RateLimitMiddleware::checkRequest(t_crow_req, crow_res)) {
                http::applyCors(crow_res, t_crow_req);
                return crow_res;
            }
            const auto& asset = web::ASSETS[i];
            http::HttpRequest t_req = http::fromCrowRequest(t_crow_req);
            http::HttpResponse t_res;

            t_res.set_header("Vary", is_entry ? "Accept-Encoding, X-Forwarded-Prefix" : "Accept-Encoding");

            const bool client_supports_zstd =
                t_req.has_header("Accept-Encoding") && t_req.get_header_value("Accept-Encoding").find("zstd") != std::string::npos;

            // Entry points carry the runtime <base>/__SGRN_BASE__ placeholder
            // that must be patched in — never serve the pre-baked zstd blob for them.
            bool serve_precompressed = client_supports_zstd && !is_entry;

            if (serve_precompressed) {
                t_res.set_header("Content-Encoding", "zstd");
                t_res.set_content(
                    reinterpret_cast<const char*>(asset.compressed_data), asset.compressed_size, std::string(asset.content_type.data()));
            } else {
                std::call_once(*flag, [&]() {
                    auto decompressed = sgrn::utils::compression::decompressStringZstd(asset.compressedView());

                    if (decompressed.hasError()) {
                        SGRN_ERROR("Gateway", "Decompress failed for {}: {}", asset.virtual_path, decompressed.error());
                        *has_error = true;
                        return;
                    }

                    *cached = std::move(decompressed.value());
                });

                if (*has_error || (cached->empty() && asset.original_size > 0)) {
                    t_res.status = 500;
                    t_res.set_content("Failed to decompress asset", "text/plain");
                } else if (!is_entry) {
                    t_res.set_content(cached->data(), cached->size(), std::string(asset.content_type.data()));
                } else {
                    const std::string prefix = forwardedPrefix(t_req);
                    std::lock_guard<std::mutex> lock(*variants_mutex);
                    auto it = variants->find(prefix);
                    if (it == variants->end()) {
                        std::string patched = *cached;
                        replaceAll(patched, "<!-- SGRN_RUNTIME_HEAD -->", runtimeHead(prefix));
                        it = variants->emplace(prefix, std::move(patched)).first;
                    }
                    t_res.set_content(it->second.data(), it->second.size(), std::string(asset.content_type.data()));
                }
            }

            http::applyToCrowResponse(t_res, crow_res);
            http::applyCors(crow_res, t_crow_req);
            return crow_res;
        };

        // NOTE: asset paths are runtime strings, so route_dynamic() is used
        // instead of CROW_ROUTE() (whose parameter tag needs a literal).
        // Dynamic rules match every method — harmless for static assets.
        // (Rate limiting is enforced manually at the top of `handler` above:
        // dynamic rules ignore .middlewares(), and global middleware only
        // runs for routes with explicit per-route middleware indices.)
        t_app.route_dynamic(route_path)(handler);

        if (route_path == "/index.html") {
            *index_handler = handler;
        } else if (route_path == "/replay.html") {
            *replay_page_handler = handler;
        }
    }

    // "/" serves the replay dashboard variant on replay gateways, the full
    // gateway dashboard otherwise. Both stay addressable directly.
    CROW_ROUTE(t_app, "/")
        .methods("GET"_method, "OPTIONS"_method)([index_handler, replay_page_handler, replay_control](const crow::request& t_crow_req) {
            crow::response crow_res;
            if (!RateLimitMiddleware::checkRequest(t_crow_req, crow_res)) {
                http::applyCors(crow_res, t_crow_req);
                return crow_res;
            }
            if (replay_control && *replay_page_handler)
                return (*replay_page_handler)(t_crow_req);
            if (*index_handler)
                return (*index_handler)(t_crow_req);
            crow_res.code = 404;
            crow_res.body = "Not found";
            crow_res.set_header("Content-Type", "text/plain");
            http::applyCors(crow_res, t_crow_req);
            return crow_res;
        });

    // SPA fallback: unknown non-API paths serve the active dashboard variant
    // (client-side routing). Registered once after all assets so entries exist.
    *spa_handler = [index_handler, replay_page_handler, replay_control](const crow::request& t_crow_req) {
        crow::response crow_res;
        // The catchall carries no middleware indices either — enforce here.
        if (!RateLimitMiddleware::checkRequest(t_crow_req, crow_res)) {
            http::applyCors(crow_res, t_crow_req);
            return crow_res;
        }
        const std::string path = t_crow_req.url;
        if (isApiPath(path)) {
            crow_res.code = 404;
            crow_res.body = "Not found: " + path;
            crow_res.set_header("Content-Type", "text/plain");
        } else if (replay_control && *replay_page_handler) {
            return (*replay_page_handler)(t_crow_req);
        } else if (*index_handler) {
            return (*index_handler)(t_crow_req);
        } else {
            crow_res.code = 404;
            crow_res.body = "Not found";
            crow_res.set_header("Content-Type", "text/plain");
        }
        http::applyCors(crow_res, t_crow_req);
        return crow_res;
    };
    auto spa = spa_handler;
    CROW_CATCHALL_ROUTE(t_app)([spa](const crow::request& t_crow_req) { return (*spa)(t_crow_req); });
}

void HttpAdapter::handleGetEndpoints(const http::HttpRequest&, http::HttpResponse& t_res) {
    t_res.set_content(kEndpointsMessage, "application/json");
}

} // namespace sgrn::gateway::adapters
