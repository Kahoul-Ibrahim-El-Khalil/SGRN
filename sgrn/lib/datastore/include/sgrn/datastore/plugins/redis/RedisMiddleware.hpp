#pragma once

#include <drogon/HttpAppFramework.h>
#include <drogon/nosql/RedisClient.h>
#include <drogon/plugins/Plugin.h>
#include <sgrn/datastore/BackendError.hpp>
#include <json/json.h>
#include <optional>
#include <string>

namespace sgrn::datastore::plugins
{

/**
 * @brief RedisMiddleware Plugin
 *
 * Centralizes Redis access and enforces key prefixing: "sgrn:service:<key>".
 * Provides coroutine-friendly methods for common operations.
 */
class RedisMiddleware : public drogon::Plugin<RedisMiddleware> {
public:
    RedisMiddleware() = default;
    void initAndStart(const Json::Value& t_config) override;
    void shutdown() override;

    // --- Core Operations (Coroutine-friendly) ---

    drogon::Task<void> set(const std::string& t_key, const std::string& t_val, int t_ttl = 0);
    drogon::Task<void> setJson(const std::string& t_key, const Json::Value& t_val, int t_ttl = 0);
    drogon::Task<BackendResult<std::string>> get(const std::string& t_key);
    drogon::Task<BackendResult<Json::Value>> getJson(const std::string& t_key);

    // --- Synchronous Operations (for Filters) ---
    BackendResult<std::string> getSync(const std::string& t_key);
    BackendResult<Json::Value> getJsonSync(const std::string& t_key);

    drogon::Task<void> del(const std::string& t_key);
    drogon::Task<bool> exists(const std::string& t_key);
    drogon::Task<void> expire(const std::string& t_key, int t_ttl);

    // --- Authentication & Session Management ---

    /**
     * @brief Stores the Unified Session Data (User, Role, Context) in Redis.
     * Key: session:{token} -> JSON
     */
    drogon::Task<BackendResult<void>> storeSession(const std::string& t_token, const Json::Value& t_session_data, int t_ttl_seconds);

    /**
     * @brief Retrieves the Unified Session Data.
     */
    drogon::Task<BackendResult<Json::Value>> getSession(const std::string& t_token);
    BackendResult<Json::Value> getSessionSync(const std::string& t_token);

    /**
     * @brief Deletes the session and associated upload keys.
     */
    drogon::Task<BackendResult<void>> deleteSession(const std::string& t_token);

    /**
     * @brief Maps an Email to the current active Session Token.
     */
    drogon::Task<BackendResult<void>> storeUserCache(const std::string& t_email, const std::string& t_token, int t_ttl_seconds);

    /**
     * @brief Gets the active token for an email.
     */
    drogon::Task<BackendResult<std::string>> getTokenFromUserCache(const std::string& t_email);

    /**
     * @brief Deletes the email->token mapping.
     */
    drogon::Task<BackendResult<void>> deleteUserCache(const std::string& t_email);

    /**
     * @brief Updates the TTL for the session.
     */
    drogon::Task<void> refreshSession(const std::string& t_token, int t_ttl_seconds);

    /**
     * @brief Gets the TTL for a session.
     */
    drogon::Task<int64_t> getSessionTtl(const std::string& t_token);

    // --- Download Management ---

    /**
     * @brief Stores a download token in Redis.
     */
    drogon::Task<void> storeDownloadToken(const std::string& t_token, const std::string& t_file_path, int t_ttl_seconds);

    /**
     * @brief Retrieves the file path associated with a download token.
     */
    drogon::Task<BackendResult<std::string>> getFilePathFromToken(const std::string& t_token);

    /**
     * @brief Removes a download token.
     */
    drogon::Task<void> removeDownloadToken(const std::string& t_token);

    /**
     * @brief Get cached presigned URL for an S3 object.
     */
    drogon::Task<BackendResult<std::string>> getPresignedUrl(const std::string& t_object_key);

    /**
     * @brief Cache presigned URL with TTL.
     */
    drogon::Task<void> cachePresignedUrl(const std::string& t_object_key, const std::string& t_url, int t_ttl_seconds);

    /**
     * @brief Invalidate presigned URL cache entry.
     */
    drogon::Task<void> invalidatePresignedUrl(const std::string& t_object_key);

    // --- GPAO Cache Management ---

    std::string prefix(const std::string& t_key) {
        return "sgrn:service:" + t_key;
    }
};

} // namespace sgrn::datastore::plugins
