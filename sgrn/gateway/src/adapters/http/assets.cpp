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
 */
using sgrn::utils::strings::replaceAll;

// The gateway assumes it always runs behind the nginx reverse proxy (see
// sites-enabled/sgrn.conf). index.html is served with a single, fixed
// runtime head: <base href="/gateway/"> plus __SGRN_BASE__, so the SPA's
// asset/API/WS requests all resolve through nginx on the page's own
// origin. There is no unproxied variant and no WS-port injection.
constexpr std::string_view kInjectedRuntimeForHtmlOverProxy = R"(<base href="/gateway/">
<script>
window.__SGRN_BASE__="/gateway";
</script>)";

void HttpAdapter::registerWebAssets() {
    namespace web = sgrn::gateway::assets::web;

    for (size_t i = 0; i < web::ASSET_COUNT; ++i) {

        auto cached = std::make_shared<std::string>();
        auto flag = std::make_shared<std::once_flag>();
        auto has_error = std::make_shared<bool>(false);

        const uint16_t ws_port = ws_port_;
        auto handler = [i, cached, flag, has_error, ws_port](const httplib::Request& t_req, httplib::Response& t_res) {
            const auto& asset = web::ASSETS[i];

            t_res.set_header("Vary", "Accept-Encoding");

            const bool client_supports_zstd =
                t_req.has_header("Accept-Encoding") && t_req.get_header_value("Accept-Encoding").find("zstd") != std::string::npos;

            // index.html carries the runtime <base>/__SGRN_BASE__ placeholder
            // that must be patched in — never serve the pre-baked zstd blob for it.
            bool serve_precompressed = client_supports_zstd && asset.virtual_path != "/index.html";

            if (serve_precompressed) {
                t_res.set_header("Content-Encoding", "zstd");
                t_res.set_content(reinterpret_cast<const char*>(asset.compressed_data), asset.compressed_size, asset.content_type.data());
                return;
            }

            std::call_once(*flag, [&, ws_port]() {
                auto decompressed = sgrn::utils::compression::decompressStringZstd(asset.compressedView());

                if (decompressed.hasError()) {
                    SGRN_ERROR("Gateway", "Decompress failed for {}: {}", asset.virtual_path, decompressed.error());
                    *has_error = true;
                    return;
                }

                *cached = std::move(decompressed.value());

                if (asset.virtual_path == "/index.html") {
                    std::string standalone_runtime = fmt::format(R"(<base href="/">
<script>
window.__SGRN_BASE__="";
window.__SGRN_WS_PORT__={};
</script>)",
                        ws_port);
                    replaceAll(*cached, "<!-- SGRN_RUNTIME_HEAD -->", standalone_runtime);
                }
            });

            if (*has_error || (cached->empty() && asset.original_size > 0)) {
                t_res.status = 500;
                t_res.set_content("Failed to decompress asset", "text/plain");
                return;
            }

            t_res.set_content(cached->data(), cached->size(), asset.content_type.data());
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
