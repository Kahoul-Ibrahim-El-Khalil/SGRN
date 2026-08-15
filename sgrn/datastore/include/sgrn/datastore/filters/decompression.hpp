#pragma once
#include <drogon/HttpFilter.h>

namespace sgrn::datastore::filters
{

/**
 * @brief A filter that decompresses ZSTD-encoded request bodies.
 *
 * This filter checks for the 'Content-Encoding: zstd' header. If present,
 * it decompresses the request body using Zstandard.
 *
 * Performance:
 *  - Small bodies (< 64KB) are decompressed synchronously on the I/O thread.
 *  - Large bodies (>= 64KB) are offloaded to the SGRN worker threadpool.
 */
class DecompressionFilter : public drogon::HttpFilter<DecompressionFilter, false> {
public:
    DecompressionFilter() = default;
    void doFilter(const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_filter_callback,
        drogon::FilterChainCallback&& t_filter_chain_callback) override;
};

} // namespace sgrn::datastore::filters
