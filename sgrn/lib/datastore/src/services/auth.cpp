#include <drogon/drogon.h>
#include <fmt/core.h>
#include <sgrn/datastore/BackendError.hpp>
#include <sgrn/datastore/DbError.hpp>
#include <sgrn/datastore/audit/AuditLogger.hpp>
#include <sgrn/datastore/session/SessionStore.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe_access.hpp>
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
using ::sgrn::datastore::BackendError;
using ::sgrn::datastore::BackendErrorKind;
using ::sgrn::datastore::fromDrogonException;
using ::sgrn::datastore::makeBackendError;
using ::sgrn::datastore::session::SessionStore;

namespace sgrn::datastore::handlers::auth
{

Task<HttpResponsePtr> performSignInProcess(HttpRequestPtr tsp_http_req, std::string t_email, std::string t_password) {
    t_email = sgrn::utils::strings::trim(std::move(t_email));
    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return sgrn::createJsonResponse(db_res);
    }
    auto db_client = db_res.value();

    const std::string t_secret = app().getCustomConfig()["jwt_secret"].asString();
    if (t_secret.empty()) {
        co_return sgrn::createJsonResponse(
            makeBackendError(BackendErrorKind::Runtime, "Auth system initialization failed (Secret missing)").setSubCode("Auth.Config"));
    }
    const std::string peer_ip = tsp_http_req->getPeerAddr().toIp();

    std::optional<drogon::orm::Result> result_opt;
    try {
        result_opt = co_await db_client->execSqlCoro("SELECT ud.* "
                                                     "FROM core.user_details ud "
                                                     "JOIN core.users u ON u.id = ud.id "
                                                     "WHERE u.email = $1 AND u.is_active = true "
                                                     "AND u.password = crypt($2, u.password)",
            t_email, t_password);
    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Signin Database Error: {}", e.base().what());
        co_return sgrn::createJsonResponse(toBackendError(fromDrogonException(e), e.base().what()).setSubCode("Auth.Database"));
    } catch (const std::exception& e) {
        ERROR_LOG("Signin Error: {}", e.what());
        co_return sgrn::createJsonResponse(
            makeBackendError(BackendErrorKind::Runtime, std::string("Internal Error: ") + e.what()).setSubCode("Auth.Internal"));
    }

    if (!result_opt || result_opt->empty()) {
        co_return sgrn::createJsonResponse(
            BackendError(BackendErrorKind::Auth, "Invalid email or password").setSubCode("Auth.Credentials"));
    }

    const auto& row = (*result_opt)[0];
    Json::Value user_data;
    Json::Value claims;
    std::string t_token;

    try {
        // 1. Extract user data
        std::string role = row["role"].as<std::string>();
        int32_t role_code = (role == "admin") ? 0 : 1;
        int32_t user_id = row["id"].as<int32_t>();

        // 2. Build claims
        user_data["id"] = user_id;
        user_data["first_name"] = row["first_name"].as<std::string>();
        user_data["family_name"] = row["family_name"].as<std::string>();
        user_data["email"] = row["email"].as<std::string>();
        user_data["phone_number"] = row["phone_number"].isNull() ? "" : row["phone_number"].as<std::string>();
        user_data["created_at"] = row["created_at"].as<std::string>();
        user_data["is_active"] = row["is_active"].isNull() ? true : row["is_active"].as<bool>();
        user_data["can_read_personal"] = row["can_read_personal"].isNull() ? true : row["can_read_personal"].as<bool>();
        user_data["can_write_personal"] = row["can_write_personal"].isNull() ? true : row["can_write_personal"].as<bool>();
        user_data["can_delete_personal"] = row["can_delete_personal"].isNull() ? true : row["can_delete_personal"].as<bool>();

        user_data["status"] = row["status"].as<std::string>();
        user_data["domain"] = row["domain"].isNull() ? "" : row["domain"].as<std::string>();

        user_data["role"] = Json::Value(Json::objectValue);
        user_data["role"]["name"] = role;
        user_data["role"]["code"] = role_code;

        user_data["organisation"] = row["organisation"].isNull() ? "" : row["organisation"].as<std::string>();
        user_data["total_virtual_size"] = row["total_virtual_size"].as<int64_t>();
        user_data["total_real_size"] = row["total_real_size"].as<int64_t>();
        user_data["storage_limit"] = row["storage_limit"].isNull() ? Json::Value::null : Json::Value(row["storage_limit"].as<int64_t>());
        user_data["total_entry_count"] = row["total_entry_count"].as<int64_t>();
        user_data["entry_count_limit"] =
            row["entry_count_limit"].isNull() ? Json::Value::null : Json::Value(row["entry_count_limit"].as<int64_t>());

        claims["user"] = user_data;
        t_token = drogon::utils::getUuid();
    } catch (const std::exception& e) {
        ERROR_LOG("Signin Mapping Error: {}", e.what());
        co_return sgrn::createJsonResponse(makeBackendError(BackendErrorKind::Runtime, std::string("Failed to map user data: ") + e.what())
                .setSubCode("Auth.DataMapping"));
    }

    try {
        const int32_t user_id = user_data["id"].asInt();

        // 4. Reuse the most recent session for this user
        auto session_lookup = co_await db_client->execSqlCoro(
            "SELECT id, token FROM core.sessions WHERE user_id = $1 AND terminated_at IS NULL ORDER BY created_at DESC LIMIT 1", user_id);

        int64_t session_id = 0;
        std::string previous_token;
        if (!session_lookup.empty()) {
            session_id = session_lookup[0]["id"].as<int64_t>();
            previous_token = session_lookup[0]["token"].as<std::string>();

            co_await db_client->execSqlCoro("UPDATE core.sessions "
                                            "SET token = $2::uuid, ip = $3::inet, terminated_at = NULL, termination_reason = NULL "
                                            "WHERE id = $1",
                session_id, t_token, peer_ip);
        } else {
            auto session_result = co_await db_client->execSqlCoro(
                "INSERT INTO core.sessions (user_id, token, ip) VALUES ($1, $2::uuid, $3::inet) RETURNING id", user_id, t_token, peer_ip);
            if (session_result.empty()) {
                co_return sgrn::createJsonResponse(
                    makeBackendError(BackendErrorKind::Database, "Sign-in failed: session could not be created")
                        .setSubCode("Auth.Database"));
            }
            session_id = session_result[0]["id"].as<int64_t>();
        }

        claims["session_id"] = Json::Value(session_id);

        if (!previous_token.empty() && previous_token != t_token) {
            co_await SessionStore::instance().revokeSession(previous_token);
        }

        bool stored = co_await SessionStore::instance().storeSession(t_token, claims, user_id, 86400);
        if (!stored) {
            co_return sgrn::createJsonResponse(
                makeBackendError(BackendErrorKind::Runtime, "Failed to store user session").setSubCode("Auth.SessionStore"));
        }
    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Signin Session Database Error: {}", e.base().what());
        co_return sgrn::createJsonResponse(toBackendError(fromDrogonException(e), e.base().what()).setSubCode("Auth.Database"));
    } catch (const std::exception& e) {
        ERROR_LOG("Signin Session Error: {}", e.what());
        co_return sgrn::createJsonResponse(
            makeBackendError(BackendErrorKind::Runtime, std::string("Session error: ") + e.what()).setSubCode("Auth.Internal"));
    }

    // 6. Construct response
    Json::Value response;
    response["token"] = t_token;
    response["user"] = user_data;

    // Log Audit Event
    const std::string org = user_data.isMember("organisation") ? user_data["organisation"].asString() : "";
    co_await sgrn::datastore::audit::AuditLogger::log(db_client, org, "user", user_data["id"].asInt(), user_data["email"].asString(),
        "auth.login", "user", std::to_string(user_data["id"].asInt()), peer_ip, Json::objectValue, "success");

    auto resp = HttpResponse::newHttpJsonResponse(std::move(response));
    resp->addHeader("Authorization", "Bearer " + t_token);
    co_return resp;
}

Task<HttpResponsePtr> updatePassword(
    HttpRequestPtr tsp_http_req, std::string t_email, std::string t_old_password, std::string t_new_password) {
    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return sgrn::createJsonResponse(db_res);
    }
    auto db_client = db_res.value();

    try {
        auto check = co_await db_client->execSqlCoro(
            "SELECT u.id FROM core.users u WHERE u.email = $1 AND u.password = crypt($2, u.password)", t_email, t_old_password);
        if (check.empty()) {
            co_return sgrn::createJsonResponse(
                BackendError(BackendErrorKind::Auth, "Current password incorrect").setSubCode("Auth.Credentials"));
        }

        const int32_t user_id = check[0]["id"].as<int32_t>();
        co_await db_client->execSqlCoro(
            "UPDATE core.users SET password = crypt($1, gen_salt('bf', 10)), updated_at = NOW() WHERE id = $2", t_new_password, user_id);

        // Terminate active sessions in DB & notify RAM cache
        auto active_sessions =
            co_await db_client->execSqlCoro("SELECT token FROM core.sessions WHERE user_id = $1 AND terminated_at IS NULL", user_id);
        for (const auto& srow : active_sessions) {
            std::string tok = srow["token"].as<std::string>();
            co_await SessionStore::instance().revokeSession(tok);
        }

        Json::Value resp;
        resp["message"] = "Password updated successfully";
        co_return HttpResponse::newHttpJsonResponse(resp);
    } catch (const std::exception& e) {
        ERROR_LOG("Update password error: {}", e.what());
        co_return sgrn::createJsonResponse(
            makeBackendError(BackendErrorKind::Runtime, std::string("Failed to update password: ") + e.what()).setSubCode("Auth.Internal"));
    }
}

Task<HttpResponsePtr> performAutomatedServiceSignInProcess(HttpRequestPtr tsp_http_req, std::string t_token, std::string t_secret) {
    const std::string peer_ip = tsp_http_req->getPeerAddr().toIp();
    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return sgrn::createJsonResponse(db_res);
    }
    auto db_client = db_res.value();

    try {
        auto result = co_await db_client->execSqlCoro("SELECT * FROM core.authenticate_automated_service($1::uuid, $2)", t_token, t_secret);
        if (result.empty()) {
            co_return sgrn::createJsonResponse(
                BackendError(BackendErrorKind::Auth, "Invalid token or secret").setSubCode("Auth.Credentials"));
        }

        const auto& row = result[0];

        Json::Value automated_service_data;
        automated_service_data["automated_service_id"] = row["id"].as<int32_t>();
        automated_service_data["organisation"] = row["organisation"].as<std::string>();
        automated_service_data["name"] = row["name"].as<std::string>();
        automated_service_data["token"] = row["token"].as<std::string>();
        automated_service_data["status"] = row["status"].as<std::string>();
        automated_service_data["domain"] = row["domain"].isNull() ? "" : row["domain"].as<std::string>();
        automated_service_data["metadata"] = row["metadata"].isNull() ? Json::Value(Json::objectValue) : row["metadata"].as<Json::Value>();
        automated_service_data["created_at"] = row["created_at"].as<std::string>();
        automated_service_data["updated_at"] = row["updated_at"].as<std::string>();
        automated_service_data["total_virtual_size"] = row["total_virtual_size"].as<int64_t>();
        automated_service_data["total_real_size"] = row["total_real_size"].as<int64_t>();
        automated_service_data["storage_limit"] =
            row["storage_limit"].isNull() ? Json::Value::null : Json::Value(row["storage_limit"].as<int64_t>());
        automated_service_data["total_entry_count"] = row["total_entry_count"].as<int64_t>();
        automated_service_data["entry_count_limit"] =
            row["entry_count_limit"].isNull() ? Json::Value::null : Json::Value(row["entry_count_limit"].as<int64_t>());

        automated_service_data["role"] = Json::Value(Json::objectValue);
        automated_service_data["role"]["name"] = "automated_service";
        automated_service_data["role"]["code"] = 1;

        Json::Value claims;
        claims["user"] = automated_service_data;

        std::string session_token = drogon::utils::getUuid();
        const int32_t automated_service_id = row["id"].as<int32_t>();

        auto active_sessions = co_await db_client->execSqlCoro("SELECT id, token FROM core.sessions "
                                                               "WHERE automated_service_id = $1 AND terminated_at IS NULL "
                                                               "ORDER BY created_at DESC",
            automated_service_id);

        int64_t session_id = 0;
        std::vector<std::string> old_tokens;

        if (!active_sessions.empty()) {
            session_id = active_sessions[0]["id"].as<int64_t>();
            old_tokens.push_back(active_sessions[0]["token"].as<std::string>());

            for (size_t i = 1; i < active_sessions.size(); i++) {
                const int64_t sid = active_sessions[i]["id"].as<int64_t>();
                old_tokens.push_back(active_sessions[i]["token"].as<std::string>());
                co_await db_client->execSqlCoro(
                    "UPDATE core.sessions SET terminated_at = NOW(), termination_reason = 'reconnected' WHERE id = $1", sid);
            }

            co_await db_client->execSqlCoro(
                "UPDATE core.sessions SET token = $2::uuid, ip = $3::inet WHERE id = $1", session_id, session_token, peer_ip);
        } else {
            auto ins = co_await db_client->execSqlCoro(
                "INSERT INTO core.sessions (automated_service_id, token, ip) VALUES ($1, $2::uuid, $3::inet) RETURNING id",
                automated_service_id, session_token, peer_ip);
            session_id = ins[0]["id"].as<int64_t>();
        }

        claims["session_id"] = Json::Value(session_id);
        co_await SessionStore::instance().storeSession(session_token, claims, automated_service_id, 86400);

        for (const auto& old_token : old_tokens) {
            if (!old_token.empty() && old_token != session_token) {
                co_await SessionStore::instance().revokeSession(old_token);
            }
        }

        Json::Value response;
        response["token"] = session_token;
        response["session_id"] = Json::Value(session_id);
        response["automated_service"] = automated_service_data;

        auto resp = HttpResponse::newHttpJsonResponse(std::move(response));
        resp->addHeader("Authorization", "Bearer " + session_token);
        co_return resp;

    } catch (const std::exception& e) {
        ERROR_LOG("Automated Service Signin Error: {}", e.what());
        co_return sgrn::createJsonResponse(
            makeBackendError(BackendErrorKind::Runtime, std::string("Internal Server Error: ") + e.what()).setSubCode("Auth.Internal"));
    }
}

} // namespace sgrn::datastore::handlers::auth

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
