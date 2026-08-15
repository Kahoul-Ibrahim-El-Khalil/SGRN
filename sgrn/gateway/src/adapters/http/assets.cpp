/*sgrn/gateway/adapters/http/assets.cpp*/
#include <sgrn/assets/EmbeddedAsset.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/gateway/adapters/http.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/strings.hpp>
#include <httplib.h>
#include <web_assets.hpp>

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
 *
 * Eliminating that final copy would require library support for zero-copy
 * response buffers or response body views, which cpp-httplib does not
 * currently provide.
 */
constexpr std::string_view kInjectedRuntimeForHtmlOverProxy = R"(<base href="/gateway/">
<script>
window.__SGRN_BASE__="/gateway";
</script>)";

[[maybe_unused]] constexpr std::string_view kInjectedRuntimeForHtmlOverLocal = R"(<base href="/gateway/">
<script>
window.__SGRN_BASE__= null;
</script>)";

void HttpAdapter::registerWebAssets() {
    namespace web = sgrn::gateway::assets::web;

    for (size_t i = 0; i < web::ASSET_COUNT; ++i) {

        struct Cache {
            std::string normal;
            std::string gateway;
        };

        auto cache = std::make_shared<Cache>();
        auto flag = std::make_shared<std::once_flag>();
        auto has_error = std::make_shared<bool>(false);

        auto handler = [i, cache, flag, has_error](const httplib::Request& t_req, httplib::Response& t_res) {
            const auto& asset = web::ASSETS[i];

            t_res.set_header("Vary", "Accept-Encoding");

            const bool client_supports_zstd =
                t_req.has_header("Accept-Encoding") && t_req.get_header_value("Accept-Encoding").find("zstd") != std::string::npos;

            bool serve_precompressed = client_supports_zstd;
            if (asset.virtual_path == "/index.html" && t_req.has_header("X-Forwarded-Prefix")) {
                serve_precompressed = false;
            }

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

                cache->normal = std::move(decompressed.value());

                //
                // Only modify index.html
                //

                cache->gateway = cache->normal;
                if (asset.virtual_path == "/index.html") {
                    // Inject runtime HTML (<base> tag + __SGRN_BASE__ script) into
                    // index.html so the SPA knows its mount point behind a reverse proxy.
                    // Uses sgrn::utils::strings::replaceAll() for the in-place substitution.
                    sgrn::utils::strings::replaceAll(cache->gateway, "<!-- SGRN_RUNTIME_HEAD -->", kInjectedRuntimeForHtmlOverProxy);

                } else {

                    cache->gateway = cache->normal;
                }
            });

            if (*has_error || (cache->normal.empty() && asset.original_size > 0)) {

                t_res.status = 500;
                t_res.set_content("Failed to decompress asset", "text/plain");
                return;
            }

            //
            // Decide which cached version to serve.
            //
            if (asset.virtual_path == "/index.html" && t_req.has_header("X-Forwarded-Prefix")) {

                t_res.set_content(cache->gateway.data(), cache->gateway.size(), asset.content_type.data());

            } else {

                t_res.set_content(cache->normal.data(), cache->normal.size(), asset.content_type.data());
            }
            fmt::print("Serving: {}", t_res.body);
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
