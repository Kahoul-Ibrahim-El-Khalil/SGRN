#include <drogon/HttpAppFramework.h>
#include <sgrn/datastore/filters/decompression.hpp>
#include <sgrn/datastore/plugins/threadpool/Threadpool.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/strings.hpp>

namespace sgrn::datastore::filters
{

static constexpr size_t kAsyncThreshold = 1ULL * 1024ULL; // 1KB

void DecompressionFilter::doFilter(const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_filter_callback,
    drogon::FilterChainCallback&& t_filter_chain_callback) {
    const std::string& content_encoding = tsp_req->getHeader("Content-Encoding");

    if (content_encoding != "zstd") {
        t_filter_chain_callback();
        return;
    }

    const std::string& content_type_raw = tsp_req->getHeader("Content-Type");
    std::string content_type = sgrn::utils::strings::toLower(content_type_raw);

    if (content_type.find("multipart/") != std::string::npos) {
        LOG_DEBUG << "[DecompressionFilter] Skipping decompression for multipart request (" << content_type_raw << ")";
        t_filter_chain_callback();
        return;
    }

    const std::string_view body = tsp_req->body();
    if (body.empty()) {
        t_filter_chain_callback();
        return;
    }

    auto* p_thread_pool = drogon::app().getPlugin<sgrn::datastore::plugins::Threadpool>();

    // ── Decompression Logic ───────────────────────────────────────────────
    auto decompress_task = [body]() { return sgrn::utils::decompressStringZstd(body); };

    auto on_decompressed = [tsp_req, fcb = std::move(tsp_filter_callback), fccb = std::move(t_filter_chain_callback)](
                               auto&& t_decompressed_res) mutable {
        if (t_decompressed_res.hasError()) {
            LOG_ERROR << "[DecompressionFilter] Failed to decompress body: " << t_decompressed_res.error();
            sgrn::respondWithError(t_decompressed_res.error(), drogon::k400BadRequest, fcb);
            return;
        }

        LOG_DEBUG << "[DecompressionFilter] Body size: " << t_decompressed_res.value().size();
        if (t_decompressed_res.value().size() > 100) {
            LOG_DEBUG << "[DecompressionFilter] Body preview: " << t_decompressed_res.value().substr(0, 100);
        }

        tsp_req->setBody(std::move(t_decompressed_res.value()));
        tsp_req->removeHeader("Content-Encoding");
        tsp_req->addHeader("Content-Length", std::to_string(tsp_req->body().size()));

        LOG_DEBUG << "[DecompressionFilter] Decompressed request body to " << tsp_req->body().size() << " bytes (updated Content-Length)";
        fccb();
    };

    // ── Execution choice ──────────────────────────────────────────────────
    if (body.size() >= kAsyncThreshold && p_thread_pool != nullptr) {
        LOG_DEBUG << "[DecompressionFilter] Offloading decompression of " << body.size() << " bytes to threadpool";
        // Capture a std::string copy — the string_view may dangle if the
        // request buffer is invalidated before the threadpool lambda runs.
        std::string body_copy(body);
        auto async_task = [body_copy = std::move(body_copy)]() { return sgrn::utils::decompressStringZstd(std::string_view(body_copy)); };
        p_thread_pool->dispatch(std::move(async_task), std::move(on_decompressed));
    } else {
        on_decompressed(decompress_task());
    }
}

} // namespace sgrn::datastore::filters
