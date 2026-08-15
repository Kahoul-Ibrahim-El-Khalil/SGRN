#include <sgrn/datastore/filters/admin.hpp>

#include <sgrn/datastore/filters/types.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/debug.hpp>
#include <string>

namespace sgrn::datastore::filters
{

inline std::string getEmailFromSession(const drogon::HttpRequestPtr& tsp_req) {
    return tsp_req->attributes()->get<Json::Value>("session_json")["user"]["email"].asString();
}

void AdminFilter::doFilter(const drogon::HttpRequestPtr& tsp_req, drogon::FilterCallback&& tsp_filter_callback,
    drogon::FilterChainCallback&& t_filter_chain_callback) {
    // 1. Retrieve the role code hydrated by AuthFilter
    // Note: get<T>() returns the value directly (not a pointer)
    // We need to check if it exists first using find()
    SGRN_INFO_LOG("Checking admin privileges for user : {}", getEmailFromSession(tsp_req));
    if (!tsp_req->attributes()->find("user_role_code")) {
        SGRN_WARN_LOG("Access denied: user role code not found");
        respondWithError("Forbidden: Auth Context Missing or Incomplete", drogon::k403Forbidden, std::move(tsp_filter_callback));
        return;
    }

    const UserRoleEnum user_role = tsp_req->attributes()->get<UserRoleEnum>("user_role_code");
    SGRN_INFO_LOG("Checking admin privileges for user with role code: {}", user_role);

    // 2. Check if user is admin
    if (user_role != UserRoleEnum::ADMIN) {
        SGRN_WARN_LOG("Access denied: user role code {} is not ADMIN", user_role);
        respondWithError("Forbidden: Admin Privileges Required", drogon::k403Forbidden, std::move(tsp_filter_callback));
        return;
    }
    SGRN_INFO_LOG("User:{} is admin", getEmailFromSession(tsp_req));

    // 3. Access granted
    t_filter_chain_callback();
}

} // namespace sgrn::datastore::filters