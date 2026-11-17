#pragma once
// admin-common.hpp — shared includes and logging for AdminApiHandler TUs.

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpTypes.h>
#include <drogon/orm/CoroMapper.h>
#include <drogon/utils/Utilities.h>
#include <drogon/utils/coroutine.h>
#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/datastore/audit/AuditLogger.hpp>
#include <sgrn/datastore/session/SessionStore.hpp>
#include <sgrn/debug.hpp>
#include <json/json.h>
#include <stdexcept>
#include <string>
#include <vector>

#include <sgrn/datastore/core/db.hpp>
#include <sgrn/datastore/core/orm-glaze.hpp>
#include <sgrn/datastore/error/ApiErrors.hpp>
#include <sgrn/datastore/filters/admin.hpp>
#include <sgrn/datastore/handlers/admin.hpp>
#include <sgrn/datastore/services/WebhookService.hpp>
#include <sgrn/datastore/services/admin-repository.hpp>
#include <sgrn/datastore/services/admin.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe-access.hpp>
#include <sgrn/utils/strings.hpp>
#include <orm/models/core/Users.h>

#ifdef DEBUG_ADMIN_HANDLER
#define ADMIN_DEBUG_LOG(msg, ...) SGRN_DEBUG("AdminHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define ADMIN_DEBUG_LOG(...) ((void)0)
#endif

#define ADMIN_INFO_LOG(msg, ...) SGRN_INFO("AdminHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define ADMIN_WARN_LOG(msg, ...) SGRN_WARN("AdminHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define ADMIN_ERROR_LOG(msg, ...) SGRN_ERROR("AdminHandler", msg __VA_OPT__(, ) __VA_ARGS__)

namespace sgrn::datastore::handlers::admin::detail
{
struct RequestContext {
    std::string organisation;
    int32_t actor_id{};
    std::string actor_name;
};

inline RequestContext requestContext(const drogon::HttpRequestPtr& t_req) {
    if (t_req && t_req->attributes()->find("auth_context")) {
        const auto& auth = t_req->attributes()->get<filters::AuthContext>("auth_context");
        return {
            auth.user.organisation,
            auth.user.id,
            auth.user.email,
        };
    }
    if (t_req && t_req->attributes()->find("session_claims")) {
        const auto& user = t_req->attributes()->get<::sgrn::datastore::auth::SessionClaims>("session_claims").user;
        return {user.organisation, user.id, user.email};
    }
    return {};
}
} // namespace sgrn::datastore::handlers::admin::detail
