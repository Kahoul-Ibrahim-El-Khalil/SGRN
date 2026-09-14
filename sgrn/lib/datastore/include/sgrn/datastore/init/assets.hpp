#pragma once

#include <drogon/drogon.h>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/compression.hpp>
#include <cctype>
#include <config_assets.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <sql_assets.hpp>
#include <string_view>
#include <web_assets.hpp>

/**
 * Per-asset response cache.
 *
 * We cache fully constructed HttpResponse instances for both the compressed
 * and decompressed variants of an asset.
 *
 * Why cache HttpResponsePtr instead of only the asset bytes?
 *
 * Even though the compressed asset bytes are embedded in the binary and the
 * decompressed bytes are generated only once, calling
 * drogon::HttpResponse::setBody() still requires the response to own its
 * body storage internally. Constructing a new HttpResponse for every request
 * would therefore repeatedly:
 *
 *   1. Allocate a new response body buffer.
 *   2. Copy the entire asset into that buffer.
 *   3. Destroy and deallocate that buffer after the request completes.
 *
 * This cost exists for BOTH paths:
 *
 *   - compressed assets: copy from embedded .rodata into the response body
 *   - decompressed assets: copy from the cached std::string into the response
 *     body (or move during initial construction)
 *
 * By caching complete HttpResponse objects, we pay these costs exactly once
 * during response construction and then simply share the same immutable
 * response instance across all requests via shared_ptr.
 *
 * Steady-state request handling becomes:
 *
 *   - inspect Accept-Encoding
 *   - select the cached response
 *   - copy a shared_ptr
 *   - invoke the callback
 *
 * No body allocations.
 * No body copies.
 * No decompression.
 * No allocator churn.
 *
 * The plain response is initialized lazily because most modern browsers
 * advertise "zstd" support and will never need the decompressed variant.
 * After initialization, only the HttpResponse object remains alive; the
 * temporary decompressed std::string used to construct it is destroyed.
 */

namespace sgrn::datastore::assets
{

using DashboardAssetHandler = std::function<void(const drogon::HttpRequestPtr&, std::function<void(const drogon::HttpResponsePtr&)>&&)>;

struct CachedAssetResponses {
    drogon::HttpResponsePtr t_compressed;
    drogon::HttpResponsePtr plain;
    drogon::HttpResponsePtr error;

    std::once_flag plain_init;
    bool plain_init_failed = false;
};

inline drogon::HttpResponsePtr makeErrorResponse(std::string_view t_message) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k500InternalServerError);
    resp->setBody(std::string(t_message));
    return resp;
}

inline drogon::HttpResponsePtr makeResponse(const EmbeddedAsset& t_asset, std::string_view t_body, bool t_compressed) {
    auto resp = drogon::HttpResponse::newHttpResponse();

    resp->addHeader("Vary", "Accept-Encoding");

    if (t_compressed) {
        resp->addHeader("Content-Encoding", "zstd");
    }

    resp->setContentTypeCodeAndCustomString(drogon::CT_CUSTOM, t_asset.content_type);

    resp->setBody(std::string(t_body));

    return resp;
}

inline drogon::HttpResponsePtr makeCompressedResponse(const EmbeddedAsset& t_asset) {
    return makeResponse(t_asset, std::string_view{reinterpret_cast<const char*>(t_asset.compressed_data), t_asset.compressed_size}, true);
}

inline drogon::HttpResponsePtr makePlainResponse(const EmbeddedAsset& t_asset) {
    auto dec = sgrn::utils::compression::decompressStringZstd(t_asset.compressedView());

    if (dec.hasError()) {
        SGRN_ERROR("Dashboard", "Decompress failed for {}: {}", t_asset.virtual_path, dec.error());

        return nullptr;
    }

    auto resp = drogon::HttpResponse::newHttpResponse();

    resp->addHeader("Vary", "Accept-Encoding");
    resp->setContentTypeCodeAndCustomString(drogon::CT_CUSTOM, t_asset.content_type);

    resp->setBody(std::move(dec.value()));

    return resp;
}

inline drogon::HttpResponsePtr getPlainResponse(const EmbeddedAsset& t_asset, const std::shared_ptr<CachedAssetResponses>& tsp_cache) {
    std::call_once(tsp_cache->plain_init, [&] {
        tsp_cache->plain = makePlainResponse(t_asset);
        tsp_cache->plain_init_failed = !tsp_cache->plain;
    });

    if (tsp_cache->plain_init_failed) {
        return tsp_cache->error;
    }

    return tsp_cache->plain;
}

inline DashboardAssetHandler getDashboardAssetHandler(size_t t_i) {
    const auto& t_asset = web::ASSETS[t_i];

    auto tsp_cache = std::make_shared<CachedAssetResponses>();

    // The compressed variant is constructed eagerly because it is the hot path.
    // Caching the HttpResponse avoids repeatedly copying the embedded asset bytes
    // into newly allocated response body buffers on every request.

    tsp_cache->t_compressed = makeCompressedResponse(t_asset);
    tsp_cache->error = makeErrorResponse("Asset decompression failed");

    return [assetPtr = &t_asset, tsp_cache](
               const drogon::HttpRequestPtr& tsp_req, std::function<void(const drogon::HttpResponsePtr&)>&& tsp_callback) {
        try {
            const auto& accept_enc = tsp_req->getHeader("Accept-Encoding");

            const bool wants_zstd = accept_enc.find("zstd") != std::string::npos;

            if (wants_zstd) {
                tsp_callback(tsp_cache->t_compressed);
                return;
            }

            tsp_callback(getPlainResponse(*assetPtr, tsp_cache));
        } catch (const std::exception& e) {
            SGRN_ERROR("Dashboard", "Exception in asset handler for {}: {}", assetPtr->virtual_path, e.what());

            tsp_callback(tsp_cache->error);
        }
    };
}

inline void registerDashboardAssets() {
    DashboardAssetHandler index_handler;
    bool has_index = false;

    for (size_t t_i = 0; t_i < web::ASSET_COUNT; ++t_i) {
        const auto& t_asset = web::ASSETS[t_i];

        auto handler = getDashboardAssetHandler(t_i);
        // a copy is necessary other wise this is a dangling reference and causes the process to crash when
        // a web page request is made.
        auto handler_copy = handler;

        if (t_asset.virtual_path == "/index.html") {
            // Keep a copy for the SPA fallback below (client-side routes like
            // /drive, /admin, /profile must serve the same shell so the React
            // router + ProtectedRoute can boot, validate the token, and either
            // render or redirect to /signin).
            index_handler = handler;
            has_index = true;
        }

        drogon::app().registerHandler(std::string(t_asset.virtual_path), std::move(handler), {drogon::Get});

        if (t_asset.virtual_path == "/index.html") {
            drogon::app().registerHandler("/", std::move(handler_copy), {drogon::Get});
        }
    }

    if (has_index) {
        // SPA fallback for client-side routing (BrowserRouter: /signin,
        // /drive, /admin, /profile, catch-all → /signin). Without this, a hard
        // reload on /admin issues GET /admin straight to the backend, which has
        // no exact route and falls through to the 403 "Unkown path" default
        // handler — the React app never boots and auth state is lost.
        //
        // Exact asset handlers (/, /index.html, /assets/*) keep priority;
        // the regex below additionally excludes /api* and /assets* so it can
        // never shadow them even on frameworks where regex wins. Inside, we
        // re-check exclusions + Accept: text/html and fall through to the
        // existing 403 JSON for anything non-page (genuinely unknown API-ish
        // paths stay a 403, not silently HTML).
        drogon::app().registerHandlerViaRegex("^/(?!api($|/)|assets($|/)).*$",
            [index_handler](const drogon::HttpRequestPtr& tsp_req, std::function<void(const drogon::HttpResponsePtr&)>&& tsp_callback) {
                if (tsp_req->method() != drogon::Get) {
                    tsp_callback(::sgrn::createErrorResponse("Unkown path", drogon::k403Forbidden));
                    return;
                }
                const std::string& path = tsp_req->path();
                // "/" and "/index.html" already have exact handlers; serve the
                // shell unconditionally if the fallback ever sees them (keeps
                // "/" behavior identical even if regex were to win).
                if (path != "/" && path != "/index.html") {
                    // Mirror classifyPath() Page exclusions: API + embedded
                    // assets must never fall back to index.html.
                    auto starts_with = [](std::string_view s, std::string_view p) {
                        return s.size() >= p.size() && s.substr(0, p.size()) == p;
                    };
                    if (path == "/api" || starts_with(path, "/api/") || path == "/assets" || starts_with(path, "/assets/")) {
                        tsp_callback(::sgrn::createErrorResponse("Unkown path", drogon::k403Forbidden));
                        return;
                    }
                    // Only browser navigations (Accept includes text/html) get
                    // the SPA shell; API/fetch clients asking for JSON must
                    // keep receiving the 403 safety net.
                    std::string accept = tsp_req->getHeader("Accept");
                    if (accept.empty()) {
                        accept = tsp_req->getHeader("accept");
                    }
                    for (auto& c : accept) {
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    }
                    if (accept.find("text/html") == std::string::npos) {
                        tsp_callback(::sgrn::createErrorResponse("Unkown path", drogon::k403Forbidden));
                        return;
                    }
                }
                index_handler(tsp_req, std::move(tsp_callback));
            },
            {drogon::Get});
    }
}

} // namespace sgrn::datastore::assets
