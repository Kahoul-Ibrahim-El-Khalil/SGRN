#pragma once
#include <drogon/HttpFilter.h>
#include <sgrn/datastore/filters/types.hpp>

namespace sgrn::datastore::filters
{
/**
 * AutomatedServiceAuthFilter
 *
 * Drop-in companion to AuthFilter for routes that are exclusively
 * intended for machine/automated service callers.
 *
 * Pipeline:
 *   1. Extract Bearer UUID Token
 *   2. Look up session in Redis
 *   3. Assert the session belongs to an automated service, not a human user
 *      — checked via session_json["user"]["automated_service_id"] presence
 *   4. Hydrate request attributes:
 *        "session_json"           → full Json::Value session payload
 *        "automated_service_id"   → int32_t
 *
 * Using a dedicated filter (rather than reusing AuthFilter + AdminFilter)
 * gives clean separation of concerns: user routes never accept automated service tokens
 * and automated service routes never accept user tokens.
 */
class AutomatedServiceAuthFilter : public drogon::HttpFilter<AutomatedServiceAuthFilter, false> {
public:
    void doFilter(const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_filter_callback,
        drogon::FilterChainCallback&& t_filter_chain_callback) override;
};

class UserAuthFilter : public drogon::HttpFilter<UserAuthFilter, false> {
public:
    void doFilter(const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_cb, drogon::FilterChainCallback&& t_chain) override;
};

} // namespace sgrn::datastore::filters
