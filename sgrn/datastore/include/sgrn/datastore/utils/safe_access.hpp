#pragma once
#include <sgrn/datastore/BackendError.hpp>

#include <drogon/HttpAppFramework.h>
#include <drogon/nosql/RedisClient.h>
#include <drogon/orm/DbClient.h>
#include <sgrn/Result.hpp>
#include <sgrn/datastore/plugins/redis/RedisMiddleware.hpp>
#include <sgrn/debug.hpp>

#ifdef DEBUG_CORE_DATABASE
#define DB_DEBUG_LOG(msg, ...) SGRN_DEBUG("CoreDB", msg __VA_OPT__(, ) __VA_ARGS__)
#define DB_ERROR_LOG(msg, ...) SGRN_ERROR("CoreDB", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define DB_DEBUG_LOG(...) ((void)0)
#define DB_ERROR_LOG(...) ((void)0)
#endif

#ifdef DEBUG_CORE_REDIS
#define REDIS_DEBUG_LOG(msg, ...) SGRN_DEBUG("CoreRedis", msg __VA_OPT__(, ) __VA_ARGS__)
#define REDIS_ERROR_LOG(msg, ...) SGRN_ERROR("CoreRedis", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define REDIS_DEBUG_LOG(...) ((void)0)
#define REDIS_ERROR_LOG(...) ((void)0)
#endif

namespace sgrn::datastore::core
{
using ::sgrn::datastore::BackendError;
using ::sgrn::datastore::BackendResult;

using ::sgrn::Result;

/**
 * @brief Safely retrieve the default database client.
 */
inline BackendResult<drogon::orm::DbClientPtr> getDbClient() {
    auto db = drogon::app().getDbClient();
    if (!db) {
        DB_ERROR_LOG("Database client is not initialized or unavailable");
        return BackendResult<drogon::orm::DbClientPtr>::Error(
            BackendError{"Database", "Database client is not initialized or unavailable"});
    }
    return db;
}

/**
 * @brief Safely retrieve the default Redis client.
 */
inline BackendResult<drogon::nosql::RedisClientPtr> getRedisClient() {
    auto redis = drogon::app().getRedisClient();
    if (!redis) {
        REDIS_ERROR_LOG("Redis client is not initialized or unavailable");
        return BackendResult<drogon::nosql::RedisClientPtr>::Error(BackendError{"Redis", "Redis client is not initialized or unavailable"});
    }
    return redis;
}

/**
 * @brief Safely retrieve the RedisMiddleware plugin.
 */
inline BackendResult<plugins::RedisMiddleware*> getRedisMiddleware() {
    auto* p_plugin = drogon::app().getPlugin<plugins::RedisMiddleware>();
    if (!p_plugin) {
        return BackendResult<plugins::RedisMiddleware*>::Error(BackendError{"Runtime", "RedisMiddleware plugin is not initialized"});
    }
    return p_plugin;
}

/**
 * @brief Safely retrieve a Drogon plugin by type.
 */
template <typename T>
inline BackendResult<T*> getPlugin() {
    auto* p_plugin = drogon::app().getPlugin<T>();
    if (!p_plugin) {
        return BackendResult<T*>::Error(BackendError{"Runtime", "Required system plugin is not initialized"});
    }
    return p_plugin;
}

} // namespace sgrn::datastore::core

#undef DB_DEBUG_LOG
#undef DB_ERROR_LOG
#undef REDIS_DEBUG_LOG
#undef REDIS_ERROR_LOG
