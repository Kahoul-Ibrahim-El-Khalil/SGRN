#include <drogon/drogon.h>
#include <fmt/core.h>
#include <sgrn/datastore/BackendError.hpp>
#include <sgrn/datastore/DbError.hpp>
#include <sgrn/datastore/audit/AuditLogger.hpp>
#include <sgrn/datastore/core/orm-glaze.hpp>
#include <sgrn/datastore/error/ApiErrors.hpp>
#include <sgrn/datastore/filters/admin.hpp>
#include <sgrn/datastore/session/SessionStore.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe-access.hpp>
#include <sgrn/datastore/wire/auth.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/hashing.hpp>
#include <sgrn/utils/strings.hpp>
#include <stdexcept>
#include <string>

#ifdef DEBUG_AUTH_SERVICE
#define DEBUG_LOG(msg, ...) SGRN_DEBUG("AuthService", msg __VA_OPT__(, ) __VA_ARGS__)
#define INFO_LOG(msg, ...) SGRN_INFO("AuthService", msg __VA_OPT__(, ) __VA_ARGS__)
#define WARN_LOG(msg, ...) SGRN_WARN("AuthService", msg __VA_OPT__(, ) __VA_ARGS__)
#define ERROR_LOG(msg, ...) SGRN_ERROR("AuthService", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define DEBUG_LOG(...) ((void)0)
#define INFO_LOG(...) ((void)0)
#define WARN_LOG(...) ((void)0)
#define ERROR_LOG(...) ((void)0)
#endif

using namespace drogon;
using namespace drogon::orm;
using ::sgrn::datastore::fromDrogonException;
using ::sgrn::datastore::session::SessionStore;

namespace sgrn::datastore::handlers::auth
{

using namespace sgrn::datastore::auth;

namespace
{
struct UserSignInRow {
    int32_t id{0};
    std::string first_name;
    std::string family_name;
    std::string email;
    std::optional<std::string> phone_number;
    std::string created_at;
    std::optional<bool> is_active;
    std::optional<bool> can_read_personal;
    std::optional<bool> can_write_personal;
    std::optional<bool> can_delete_personal;
    std::string status;
    std::optional<std::string> domain;
    std::string role;
    std::optional<bool> is_platform_admin;
    std::optional<std::string> organisation;
    int64_t total_virtual_size{0};
    int64_t total_real_size{0};
    std::optional<int64_t> storage_limit;
    int64_t total_entry_count{0};
    std::optional<int64_t> entry_count_limit;
};

struct ServiceSignInRow {
    int32_t id{0};
    std::string organisation;
    std::string name;
    std::string token;
    std::string status;
    std::optional<std::string> domain;
    std::optional<std::string> metadata;
    std::string created_at;
    std::string updated_at;
    int64_t total_virtual_size{0};
    int64_t total_real_size{0};
    std::optional<int64_t> storage_limit;
    int64_t total_entry_count{0};
    std::optional<int64_t> entry_count_limit;
};

struct IdRow {
    int32_t id{0};
};

struct SessionLookupRow {
    int64_t id{0};
    std::string token;
};

struct SessionTokenRow {
    std::string token;
};

struct SessionIdRow {
    int64_t id{0};
};

static constexpr char kDomainIdQuery[] = "SELECT id FROM core.domains WHERE organisation = $1 AND name = $2";
static constexpr char kUserSessionLookupQuery[] =
    "SELECT id, token FROM core.sessions WHERE user_id = $1 AND terminated_at IS NULL ORDER BY created_at DESC LIMIT 1";
static constexpr char kInsertUserSessionQuery[] =
    "INSERT INTO core.sessions (user_id, token, ip) VALUES ($1, $2::uuid, $3::inet) RETURNING id";
static constexpr char kCheckPasswordQuery[] = "SELECT u.id FROM core.users u WHERE u.email = $1 AND u.password = crypt($2, u.password)";
static constexpr char kActiveSessionTokensQuery[] = "SELECT token FROM core.sessions WHERE user_id = $1 AND terminated_at IS NULL";
static constexpr char kServiceSignInQuery[] =
    "SELECT id, organisation, name, token, status, domain, metadata::text AS metadata, created_at, updated_at, "
    "total_virtual_size, total_real_size, storage_limit, total_entry_count, entry_count_limit "
    "FROM core.authenticate_automated_service($1::uuid, $2)";
static constexpr char kServiceSessionLookupQuery[] = "SELECT id, token FROM core.sessions "
                                                     "WHERE automated_service_id = $1 AND terminated_at IS NULL "
                                                     "ORDER BY created_at DESC";
static constexpr char kInsertServiceSessionQuery[] =
    "INSERT INTO core.sessions (automated_service_id, token, ip) VALUES ($1, $2::uuid, $3::inet) RETURNING id";
} // namespace

Task<HttpResponsePtr> performSignInProcess(HttpRequestPtr tsp_http_req, std::string t_email, std::string t_password) {
    t_email = sgrn::utils::strings::trim(std::move(t_email));
    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return createTypedResponse(BackendErrorResponse{db_res.error()});
    }
    auto db_client = db_res.value();

    const std::string t_secret = app().getCustomConfig()["jwt_secret"].asString();
    if (t_secret.empty()) {
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    }
    const std::string peer_ip = tsp_http_req->getPeerAddr().toIp();

    std::optional<drogon::orm::Result> result_opt;
    try {
        // WP2 sign-in hardening: the SQL predicate excludes suspended /
        // soft-deleted accounts and accounts whose organisation is suspended
        // or archived, matching core.authenticate_user (postgres/functions).
        // F10 (deprecated role-based admin level in the JWT claims) is fixed
        // separately against core.admin_level.
        result_opt = co_await db_client->execSqlCoro("SELECT ud.* "
                                                     "FROM core.user_details ud "
                                                     "JOIN core.users u ON u.id = ud.id "
                                                     "WHERE u.email = $1 AND u.is_active = true AND u.status = 'active' "
                                                     "AND u.deleted_at IS NULL "
                                                     "AND EXISTS (SELECT 1 FROM core.organisations o "
                                                     "            WHERE o.name = u.organisation AND o.status = 'active') "
                                                     "AND u.password = crypt($2, u.password)",
            t_email, t_password);
    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Signin Database Error: {}", e.base().what());
        co_return createTypedResponse(
            BackendErrorResponse{toBackendError(fromDrogonException(e), e.base().what()).setSubCode("Auth.Database")});
    } catch (const std::exception& e) {
        ERROR_LOG("Signin Error: {}", e.what());
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    }

    if (!result_opt || result_opt->empty()) {
        co_await sgrn::datastore::audit::AuditLogger::log(db_client, "system", "user", std::nullopt, t_email, "auth.failed", "user",
            t_email, peer_ip, ::sgrn::json::Value::object_t{}, "failure");
        co_return createTypedResponse(BackendErrorResponse{AuthApiError::InvalidCredentials});
    }

    const auto row = sgrn::orm::fromRowByName<UserSignInRow>((*result_opt)[0]);
    UserDetails user_data;
    ::sgrn::datastore::auth::SessionClaims claims;
    std::string t_token;

    try {
        // 1. Extract user data
        const std::string& role = row.role;
        const int32_t role_code = (role == "admin") ? 0 : 1;
        const int32_t user_id = row.id;

        // 2. Build the reflected user object. Session claims are derived from
        // it so there is one definition of the user shape.
        user_data.id = user_id;
        user_data.first_name = row.first_name;
        user_data.family_name = row.family_name;
        user_data.email = row.email;
        user_data.phone_number = row.phone_number.value_or("");
        user_data.created_at = row.created_at;
        user_data.is_active = row.is_active.value_or(true);
        user_data.can_read_personal = row.can_read_personal.value_or(true);
        user_data.can_write_personal = row.can_write_personal.value_or(true);
        user_data.can_delete_personal = row.can_delete_personal.value_or(true);

        user_data.status = row.status;
        user_data.domain = row.domain.value_or("");
        user_data.role = UserRole{role, role_code};

        const bool is_platform_admin = row.is_platform_admin.value_or(false);
        user_data.is_platform_admin = is_platform_admin;
        user_data.organisation = row.organisation.value_or("");

        // F10: the session's admin level is authoritative from core.admin_level
        // (admin_assignments + is_platform_admin), not the legacy `role` column.
        // Resolve any domain scope so a DOMAIN admin is not flattened to NONE.
        std::optional<int32_t> admin_domain_id;
        const std::string& user_domain = user_data.domain;
        if (!user_domain.empty() && !user_data.organisation.empty()) {
            auto dom = co_await db_client->execSqlCoro(kDomainIdQuery, user_data.organisation, user_domain);
            if (!dom.empty()) {
                admin_domain_id = sgrn::orm::fromRow<IdRow, kDomainIdQuery>(dom[0]).id;
            }
        }
        const sgrn::datastore::filters::AdminLevel resolved_level =
            co_await sgrn::datastore::filters::AdminLevelCache::instance().resolveAdminLevel(
                user_id, user_data.organisation, admin_domain_id, user_data.email);
        int32_t admin_lvl = static_cast<int32_t>(resolved_level);
        if (is_platform_admin && admin_lvl < static_cast<int32_t>(sgrn::datastore::filters::AdminLevel::PLATFORM)) {
            admin_lvl = static_cast<int32_t>(sgrn::datastore::filters::AdminLevel::PLATFORM);
        }
        user_data.admin_level = admin_lvl;
        user_data.admin_level_name =
            sgrn::datastore::filters::adminLevelToString(static_cast<sgrn::datastore::filters::AdminLevel>(admin_lvl));
        user_data.total_virtual_size = row.total_virtual_size;
        user_data.total_real_size = row.total_real_size;
        user_data.storage_limit = row.storage_limit;
        user_data.total_entry_count = row.total_entry_count;
        user_data.entry_count_limit = row.entry_count_limit;

        // Keep the persisted session user shape aligned with UserDetails so
        // existing session consumers see the same fields as before.
        claims.user.id = user_data.id;
        claims.user.first_name = user_data.first_name;
        claims.user.family_name = user_data.family_name;
        claims.user.email = user_data.email;
        claims.user.phone_number = user_data.phone_number;
        claims.user.created_at = user_data.created_at;
        claims.user.is_active = user_data.is_active;
        claims.user.can_read_personal = user_data.can_read_personal;
        claims.user.can_write_personal = user_data.can_write_personal;
        claims.user.can_delete_personal = user_data.can_delete_personal;
        claims.user.status = user_data.status;
        claims.user.domain = user_data.domain;
        claims.user.organisation = user_data.organisation;
        claims.user.role = user_data.role;
        claims.user.total_virtual_size = user_data.total_virtual_size;
        claims.user.total_real_size = user_data.total_real_size;
        claims.user.storage_limit = user_data.storage_limit;
        claims.user.total_entry_count = user_data.total_entry_count;
        claims.user.entry_count_limit = user_data.entry_count_limit;
        claims.user.admin_level = user_data.admin_level;
        claims.user.admin_level_name = user_data.admin_level_name;
        claims.user.is_platform_admin = user_data.is_platform_admin;
        t_token = drogon::utils::getUuid();
    } catch (const std::exception& e) {
        ERROR_LOG("Signin Mapping Error: {}", e.what());
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    }

    try {
        const int32_t user_id = user_data.id;

        // 4. Reuse the most recent session for this user
        auto session_lookup = co_await db_client->execSqlCoro(kUserSessionLookupQuery, user_id);

        int64_t session_id = 0;
        std::string previous_token;
        if (!session_lookup.empty()) {
            const auto existing_session = sgrn::orm::fromRow<SessionLookupRow, kUserSessionLookupQuery>(session_lookup[0]);
            session_id = existing_session.id;
            previous_token = existing_session.token;

            co_await db_client->execSqlCoro("UPDATE core.sessions "
                                            "SET token = $2::uuid, ip = $3::inet, terminated_at = NULL, termination_reason = NULL "
                                            "WHERE id = $1",
                session_id, t_token, peer_ip);
        } else {
            auto session_result = co_await db_client->execSqlCoro(kInsertUserSessionQuery, user_id, t_token, peer_ip);
            if (session_result.empty()) {
                co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
            }
            session_id = sgrn::orm::fromRowByName<IdRow>(session_result[0]).id;
        }

        claims.session_id = session_id;

        if (!previous_token.empty() && previous_token != t_token) {
            co_await SessionStore::instance().revokeSession(previous_token);
        }

        bool stored = co_await SessionStore::instance().storeSession(t_token, claims, user_id, 86400);
        if (!stored) {
            co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
        }
    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Signin Session Database Error: {}", e.base().what());
        co_return createTypedResponse(
            BackendErrorResponse{toBackendError(fromDrogonException(e), e.base().what()).setSubCode("Auth.Database")});
    } catch (const std::exception& e) {
        ERROR_LOG("Signin Session Error: {}", e.what());
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    }

    // 6. Construct the reflected response and let content negotiation choose
    // the wire format (JSON by default, MessagePack/BEVE when requested).
    const UserSignInResponse response{t_token, user_data};

    // Log Audit Event
    co_await sgrn::datastore::audit::AuditLogger::log(db_client, user_data.organisation, "user", user_data.id, user_data.email,
        "auth.login", "user", std::to_string(user_data.id), peer_ip, ::sgrn::json::Value::object_t{}, "success");

    auto resp = createTypedResponse(tsp_http_req, response, k200OK);
    resp->addHeader("Authorization", "Bearer " + t_token);
    co_return resp;
}

Task<HttpResponsePtr> updatePassword(
    HttpRequestPtr tsp_http_req, std::string t_email, std::string t_old_password, std::string t_new_password) {
    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return createTypedResponse(BackendErrorResponse{db_res.error()});
    }
    auto db_client = db_res.value();

    try {
        auto check = co_await db_client->execSqlCoro(kCheckPasswordQuery, t_email, t_old_password);
        if (check.empty()) {
            co_return createTypedResponse(BackendErrorResponse{AuthApiError::InvalidCredentials});
        }

        const int32_t user_id = sgrn::orm::fromRow<IdRow, kCheckPasswordQuery>(check[0]).id;
        co_await db_client->execSqlCoro(
            "UPDATE core.users SET password = crypt($1, gen_salt('bf', 10)), updated_at = NOW() WHERE id = $2", t_new_password, user_id);

        // Terminate active sessions in DB & notify RAM cache
        auto active_sessions = co_await db_client->execSqlCoro(kActiveSessionTokensQuery, user_id);
        for (const auto& srow : sgrn::orm::fromResult<SessionTokenRow, kActiveSessionTokensQuery>(active_sessions)) {
            co_await SessionStore::instance().revokeSession(srow.token);
        }

        co_return sgrn::createTypedResponse(sgrn::MessageResponse{"Password updated successfully"});
    } catch (const std::exception& e) {
        ERROR_LOG("Update password error: {}", e.what());
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    }
}

Task<HttpResponsePtr> performAutomatedServiceSignInProcess(HttpRequestPtr tsp_http_req, std::string t_token, std::string t_secret) {
    const std::string peer_ip = tsp_http_req->getPeerAddr().toIp();
    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return createTypedResponse(BackendErrorResponse{db_res.error()});
    }
    auto db_client = db_res.value();

    try {
        auto result = co_await db_client->execSqlCoro(kServiceSignInQuery, t_token, t_secret);
        if (result.empty()) {
            co_return createTypedResponse(BackendErrorResponse{AuthApiError::InvalidCredentials});
        }

        const auto row = sgrn::orm::fromRow<ServiceSignInRow, kServiceSignInQuery>(result[0]);

        AutomatedServiceDetails automated_service_data;
        automated_service_data.automated_service_id = row.id;
        automated_service_data.organisation = std::move(row.organisation);
        automated_service_data.name = std::move(row.name);
        automated_service_data.token = std::move(row.token);
        automated_service_data.status = row.status;
        automated_service_data.domain = std::move(row.domain).value_or("");
        if (row.metadata) {
            auto metadata = sgrn::json::parseValue(row.metadata.value());
            if (!metadata.hasError()) {
                automated_service_data.metadata = std::move(metadata).value();
            }
        }
        automated_service_data.created_at = std::move(row.created_at);
        automated_service_data.updated_at = std::move(row.updated_at);
        automated_service_data.total_virtual_size = row.total_virtual_size;
        automated_service_data.total_real_size = row.total_real_size;
        automated_service_data.storage_limit = row.storage_limit;
        automated_service_data.total_entry_count = row.total_entry_count;
        automated_service_data.entry_count_limit = row.entry_count_limit;

        automated_service_data.role = UserRole{"automated_service", 1};
        // Services are never browser admins; expose the same claim shape so the
        // dashboard gate can rely on admin_level being present.
        automated_service_data.is_platform_admin = false;
        automated_service_data.admin_level = 0;
        automated_service_data.admin_level_name = "NONE";

        ::sgrn::datastore::auth::SessionClaims claims;
        claims.user.automated_service_id = automated_service_data.automated_service_id;
        claims.user.organisation = std::move(automated_service_data.organisation);
        claims.user.name = automated_service_data.name;
        claims.user.token = std::move(automated_service_data.token);
        claims.user.status = automated_service_data.status;
        claims.user.domain = std::move(automated_service_data.domain);
        claims.user.metadata = automated_service_data.metadata;
        claims.user.created_at = std::move(automated_service_data.created_at);
        claims.user.role = automated_service_data.role;
        claims.user.total_virtual_size = automated_service_data.total_virtual_size;
        claims.user.total_real_size = automated_service_data.total_real_size;
        claims.user.storage_limit = automated_service_data.storage_limit;
        claims.user.total_entry_count = automated_service_data.total_entry_count;
        claims.user.entry_count_limit = automated_service_data.entry_count_limit;
        claims.user.admin_level = 0;
        claims.user.admin_level_name = "NONE";
        claims.user.is_platform_admin = false;
        claims.actor_type = "automated_service";
        claims.ip = "";
        claims.created_at = std::move(automated_service_data.created_at);
        claims.expires_at = "";

        std::string session_token = drogon::utils::getUuid();
        const int32_t automated_service_id = automated_service_data.automated_service_id;

        auto active_sessions = co_await db_client->execSqlCoro(kServiceSessionLookupQuery, automated_service_id);

        const auto session_rows = sgrn::orm::fromResult<SessionLookupRow, kServiceSessionLookupQuery>(active_sessions);
        int64_t session_id = 0;
        std::vector<std::string> old_tokens;

        if (!session_rows.empty()) {
            session_id = session_rows[0].id;
            old_tokens.push_back(session_rows[0].token);

            for (size_t i = 1; i < session_rows.size(); i++) {
                const int64_t sid = session_rows[i].id;
                old_tokens.push_back(session_rows[i].token);
                co_await db_client->execSqlCoro(
                    "UPDATE core.sessions SET terminated_at = NOW(), termination_reason = 'reconnected' WHERE id = $1", sid);
            }

            co_await db_client->execSqlCoro(
                "UPDATE core.sessions SET token = $2::uuid, ip = $3::inet WHERE id = $1", session_id, session_token, peer_ip);
        } else {
            auto ins = co_await db_client->execSqlCoro(kInsertServiceSessionQuery, automated_service_id, session_token, peer_ip);
            session_id = sgrn::orm::fromRowByName<SessionIdRow>(ins[0]).id;
        }

        claims.session_id = session_id;
        co_await SessionStore::instance().storeSession(session_token, claims, automated_service_id, 86400);

        for (const auto& old_token : old_tokens) {
            if (!old_token.empty() && old_token != session_token) {
                co_await SessionStore::instance().revokeSession(old_token);
            }
        }

        const ServiceSignInResponse response{session_token, session_id, automated_service_data};

        auto resp = createTypedResponse(tsp_http_req, response, k200OK);
        resp->addHeader("Authorization", "Bearer " + session_token);
        co_return resp;

    } catch (const std::exception& e) {
        ERROR_LOG("Automated Service Signin Error: {}", e.what());
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    }
}

} // namespace sgrn::datastore::handlers::auth

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
