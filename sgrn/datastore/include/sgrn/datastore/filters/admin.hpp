#pragma once
#include <drogon/HttpFilter.h>

namespace sgrn::datastore::filters
{

class AdminFilter : public drogon::HttpFilter<AdminFilter, false> {
public:
    void doFilter(const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_filter_callback,
        drogon::FilterChainCallback&& t_filter_chain_callback) override;
};

} // namespace sgrn::datastore::filters
