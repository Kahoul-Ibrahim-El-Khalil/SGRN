/*sgrn/gateway/adapters/http/assets.cpp*/
#include <sgrn/assets/EmbeddedAsset.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/gateway/adapters/http.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/strings.hpp>
#include <httplib.h>
#include <web_assets.hpp>

#include <map>
#include <mutex>
#include <string>

namespace sgrn::gateway::adapters
{

static const std::string kEndpointsMessage = R"({
  "endpoints": [
    {"path":"/",                                    "method":"GET",  "description":"SGRN Web Dashboard."},
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
    {"path":"/connections",                         "method":"GET",  "description":"Active/recent south and north connections."},
    {"path":"/db/history",                          "method":"GET",  "description":"Full historical database as JSON."},
    {"path":"/db/sessions",                         "method":"GET",  "description":"Active and recent client sessions."},
    {"path":"/db/logs",                             "method":"GET",  "description":"Most recent system logs."},
    {"path":"/endpoints",                           "method":"GET",  "description":"This API documentation."}
  ]
})";

/*
 * Unlike Drogon, cpp-httplib constructs a fresh Response object for every
 * request and owns its response body storage internally. Consequently we
 * cannot cache and reuse a fully constructed response object.
 *
 * We therefore cache only the asset payloads themselves:
 *
 *   - compressed bytes
 *   - lazily decompressed bytes
 *
 * This avoids repeated decompression and repeated allocations for the asset
 * buffers, but each request still requires cpp-httplib to allocate its own
 * response body and copy the payload into it.
 */
using sgrn::utils::strings::replaceAll;

namespace
{

/// Normalize the X-Forwarded-Prefix header nginx sends (see
/// sites-enabled/sgrn.conf, "/gateway"). Returns "" for direct access.
/// Strict allow-list: anything outside [A-Za-z0-9_.\-/~] (or a missing
/// leading '/') falls back to standalone so a forged header can never
/// inject markup into index.html.
std::string forwardedPrefix(const httplib::Request& t_req) {
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
/// Behind the proxy the SPA must resolve API/asset/WS URLs against the page's
/// own origin + prefix (no WS-port injection: ws() then builds
/// wss://host<prefix>/ws, which nginx proxies). Direct access keeps the
/// standalone head with the explicit WS port.
std::string runtimeHead(const std::string& t_prefix, uint16_t t_ws_port) {
    if (t_prefix.empty()) {
        return fmt::format(R"(<base href="/">
<script>
window.__SGRN_BASE__="";
window.__SGRN_WS_PORT__={};
</script>)",
            t_ws_port);
    }
    return fmt::format(R"(<base href="{0}/">
<script>
window.__SGRN_BASE__="{0}";
</script>)",
        t_prefix);
}

} // namespace

void HttpAdapter::registerWebAssets() {
    namespace web = sgrn::gateway::assets::web;

    for (size_t i = 0; i < web::ASSET_COUNT; ++i) {

        auto cached = std::make_shared<std::string>();
        auto flag = std::make_shared<std::once_flag>();
        auto has_error = std::make_shared<bool>(false);
        // index.html only: patched variants keyed by forwarded prefix, since
        // direct (:8000) and proxied (https://host/gateway/) clients need
        // different <base>/__SGRN_BASE__ heads. Handlers run on the httplib
        // pool, so the map is mutex-guarded.
        auto variants = std::make_shared<std::map<std::string, std::string>>();
        auto variants_mutex = std::make_shared<std::mutex>();

        const uint16_t ws_port = ws_port_;
        auto handler = [i, cached, flag, has_error, variants, variants_mutex, ws_port](
                           const httplib::Request& t_req, httplib::Response& t_res) {
            const auto& asset = web::ASSETS[i];
            const bool is_index = (asset.virtual_path == "/index.html");

            t_res.set_header("Vary", is_index ? "Accept-Encoding, X-Forwarded-Prefix" : "Accept-Encoding");

            const bool client_supports_zstd =
                t_req.has_header("Accept-Encoding") && t_req.get_header_value("Accept-Encoding").find("zstd") != std::string::npos;

            // index.html carries the runtime <base>/__SGRN_BASE__ placeholder
            // that must be patched in — never serve the pre-baked zstd blob for it.
            bool serve_precompressed = client_supports_zstd && !is_index;

            if (serve_precompressed) {
                t_res.set_header("Content-Encoding", "zstd");
                t_res.set_content(reinterpret_cast<const char*>(asset.compressed_data), asset.compressed_size, asset.content_type.data());
                return;
            }

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
                return;
            }

            if (!is_index) {
                t_res.set_content(cached->data(), cached->size(), asset.content_type.data());
                return;
            }

            const std::string prefix = forwardedPrefix(t_req);
            std::lock_guard<std::mutex> lock(*variants_mutex);
            auto it = variants->find(prefix);
            if (it == variants->end()) {
                std::string patched = *cached;
                replaceAll(patched, "<!-- SGRN_RUNTIME_HEAD -->", runtimeHead(prefix, ws_port));
                it = variants->emplace(prefix, std::move(patched)).first;
            }
            t_res.set_content(it->second.data(), it->second.size(), asset.content_type.data());
        };

        const auto& asset = web::ASSETS[i];

        server_->Get(asset.virtual_path.data(), handler);

        if (asset.virtual_path == "/index.html") {

            server_->Get("/", handler);

            server_->set_error_handler([handler](const httplib::Request& req, httplib::Response& res) {
                if (res.status == 404) {

                    auto p = req.path;

                    if (p.find("/api") != 0 && p.find("/data") != 0 && p.find("/memory") != 0 && p.find("/registry") != 0 &&
                        p.find("/endpoints") != 0 && p.find("/connections") != 0 && p.find("/db") != 0) {

                        res.status = 200;
                        handler(req, res);
                    }
                }
            });
        }
    }
}
void HttpAdapter::handleGetEndpoints(const httplib::Request&, httplib::Response& t_res) {
    t_res.set_content(kEndpointsMessage, "application/json");
}

} // namespace sgrn::gateway::adapters
