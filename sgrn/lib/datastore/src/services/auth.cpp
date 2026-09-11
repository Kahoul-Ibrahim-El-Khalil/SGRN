#include <drogon/drogon.h>
#include <fmt/core.h>
#include <sgrn/datastore/BackendError.hpp>
#include <sgrn/datastore/DbError.hpp>
#include <sgrn/datastore/plugins/postgrest/PostgrestClient.hpp>
#include <sgrn/datastore/plugins/redis/RedisError.hpp>
#include <sgrn/datastore/plugins/redis/RedisMiddleware.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe_access.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/hashing.hpp>
#include <sgrn/utils/strings.hpp>

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
using ::sgrn::datastore::plugins::RedisError;

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
    sgrn::datastore::plugins::RedisMiddleware* p_redis = nullptr;

    try {
        // 1. Extract user data
        std::string role = row["role"].as<std::string>();
        int32_t role_code = (role == "admin") ? 0 : 1;
        int32_t user_id = row["id"].as<int32_t>();

        // 2. Build claims for JWT
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

        auto redis_res = sgrn::datastore::core::getRedisMiddleware();
        if (redis_res.hasError()) {
            co_return sgrn::createJsonResponse(redis_res);
        }
        p_redis = redis_res.value();
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
            auto del_res = co_await p_redis->deleteSession(previous_token);
            if (del_res.hasError()) {
                ERROR_LOG("Failed to delete previous session: {}", del_res.error());
            }
        }

        auto store_res = co_await p_redis->storeSession(t_token, claims, 3600);
        if (store_res.hasError()) {
            co_return sgrn::createJsonResponse(
                toBackendError(RedisError::CommandFailed, "Redis store session error: " + store_res.error().message_)
                    .setSubCode("Auth.Redis"));
        }
        auto cache_res = co_await p_redis->storeUserCache(t_email, t_token, 3600);
        if (cache_res.hasError()) {
            ERROR_LOG("Failed to store user cache: {}", cache_res.error());
        }
    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Signin Session Database Error: {}", e.base().what());
        co_return sgrn::createJsonResponse(toBackendError(fromDrogonException(e), e.base().what()).setSubCode("Auth.Database"));
    } catch (const std::exception& e) {
        ERROR_LOG("Signin Redis/Session Error: {}", e.what());
        co_return sgrn::createJsonResponse(
            toBackendError(RedisError::CommandFailed, std::string("Redis/Session error: ") + e.what()).setSubCode("Auth.Redis"));
    }

    // 6. Construct response
    Json::Value response;
    response["token"] = t_token;
    response["user"] = user_data;

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

    int32_t user_id;
    try {
        // 1. Verify old password and fetch user id in one query
        auto result = co_await db_client->execSqlCoro("SELECT id FROM core.users WHERE email = $1 AND password = crypt($2, "
                                                      "password)",
            t_email, t_old_password);

        if (result.empty()) {
            co_return sgrn::createJsonResponse(BackendError(BackendErrorKind::Auth, "Invalid old password").setSubCode("Auth.Credentials"));
        }

        user_id = result[0]["id"].as<int32_t>();
    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Update Password DB Error: {}", e.base().what());
        co_return sgrn::createJsonResponse(toBackendError(fromDrogonException(e), e.base().what()).setSubCode("Auth.Database"));
    } catch (const std::exception& e) {
        ERROR_LOG("Update Password Mapping Error: {}", e.what());
        co_return sgrn::createJsonResponse(
            makeBackendError(BackendErrorKind::Runtime, std::string("Mapping error verifying password: ") + e.what())
                .setSubCode("Auth.DataMapping"));
    }

    std::optional<drogon::orm::Result> terminated;
    try {
        // 2. Update password — trg_hash_password will bcrypt it automatically
        co_await db_client->execSqlCoro("UPDATE core.users SET password = $1 WHERE id = $2", t_new_password, user_id);

        // 3. Terminate all active sessions for this user.
        //    Any JWT still in Redis will be stale — the auth filter must revalidate
        //    against core.sessions.terminated_at on each request.
        terminated = co_await db_client->execSqlCoro("UPDATE core.sessions "
                                                     "SET    terminated_at      = NOW(), "
                                                     "       termination_reason = 'password_changed' "
                                                     "WHERE  user_id        = $1 "
                                                     "  AND  terminated_at  IS NULL "
                                                     "RETURNING token",
            user_id);
    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Update Password Session DB Error: {}", e.base().what());
        co_return sgrn::createJsonResponse(toBackendError(fromDrogonException(e), e.base().what()).setSubCode("Auth.Database"));
    } catch (const std::exception& e) {
        ERROR_LOG("Update Password Session Error: {}", e.what());
        co_return sgrn::createJsonResponse(
            makeBackendError(BackendErrorKind::Runtime, std::string("Internal error updating password/sessions: ") + e.what())
                .setSubCode("Auth.Internal"));
    }

    try {
        // 4. Evict every invalidated token from Redis so the cache stays consistent
        auto redis_res = sgrn::datastore::core::getRedisMiddleware();
        if (redis_res.hasError() == false) {
            auto p_redis = redis_res.value();
            for (const auto& terminated_row : *terminated) {
                std::string stale_token = terminated_row["token"].as<std::string>();
                // fire-and-forget — cache eviction failure is non-fatal
                [p_redis, stale_token]() -> drogon::AsyncTask { co_await p_redis->deleteSession(stale_token); }();
            }
        }
    } catch (const std::exception& e) {
        // Log but do not fail the request if redis eviction fails
        ERROR_LOG("Update Password Redis Eviction Error: {}", e.what());
    }

    Json::Value response;
    response["message"] = "Password updated successfully. Please sign in again.";
    response["sessions_terminated"] = static_cast<int>(terminated->size());
    co_return drogon::HttpResponse::newHttpJsonResponse(std::move(response));
}

Task<HttpResponsePtr> performAutomatedServiceSignInProcess(HttpRequestPtr tsp_http_req, std::string t_token, std::string t_secret) {
    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return sgrn::createJsonResponse(db_res);
    }
    auto db_client = db_res.value();

    const std::string peer_ip = tsp_http_req->getPeerAddr().toIp();

    try {
        // 1. Convert t_token to UUID for Postgres validation
        //    Postgres will throw if it's not a valid UUID string.
        auto result = co_await db_client->execSqlCoro("SELECT * FROM core.authenticate_automated_service($1::uuid, $2)", t_token, t_secret);
        if (result.empty()) {
            co_return sgrn::createJsonResponse(
                BackendError(BackendErrorKind::Auth, "Invalid token or secret").setSubCode("Auth.Credentials"));
        }

        const auto& row = result[0];

        // 2. Build claims for Redis (mirroring user data where possible)
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

        // Mirror role for sifting compatibility if needed
        automated_service_data["role"] = Json::Value(Json::objectValue);
        automated_service_data["role"]["name"] = "automated_service";
        automated_service_data["role"]["code"] = 1; // Standard non-admin code

        Json::Value claims;
        claims["user"] = automated_service_data; // Legacy naming "user" for session info compatibility

        // 3. Generate Opaque Token (UUID) for this session
        std::string session_token = drogon::utils::getUuid();

        // 4. Manage Automated Service Sessions in DB
        const int32_t automated_service_id = row["id"].as<int32_t>();

        // Reuse existing session if possible
        auto active_sessions = co_await db_client->execSqlCoro("SELECT id, token FROM core.sessions "
                                                               "WHERE automated_service_id = $1 AND terminated_at IS NULL "
                                                               "ORDER BY created_at DESC",
            automated_service_id);

        int64_t session_id = 0;
        std::vector<std::string> old_tokens;

        if (!active_sessions.empty()) {
            session_id = active_sessions[0]["id"].as<int64_t>();
            old_tokens.push_back(active_sessions[0]["token"].as<std::string>());

            // Terminate others
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

        // 5. Cache in Redis
        auto redis_res = sgrn::datastore::core::getRedisMiddleware();
        if (redis_res.hasError()) {
            co_return sgrn::createJsonResponse(redis_res);
        }
        auto p_redis = redis_res.value();

        claims["session_id"] = Json::Value(session_id);
        auto store_res = co_await p_redis->storeSession(session_token, claims, 3600);
        if (store_res.hasError()) {
            co_return sgrn::createJsonResponse(
                toBackendError(RedisError::CommandFailed, "Redis store session error: " + store_res.error().message_)
                    .setSubCode("Auth.Redis"));
        }
        // Evict old sessions from Redis
        for (const auto& old_token : old_tokens) {
            if (!old_token.empty() && old_token != session_token) {
                auto del_res = co_await p_redis->deleteSession(old_token);
                if (del_res.hasError()) {
                    ERROR_LOG("Failed to delete old token: {}", del_res.error());
                }
            }
        }

        // 6. Construct response
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
