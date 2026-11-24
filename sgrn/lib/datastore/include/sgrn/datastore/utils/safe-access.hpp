#pragma once
#include <sgrn/datastore/BackendError.hpp>

#include <drogon/HttpAppFramework.h>
#include <drogon/orm/DbClient.h>
#include <sgrn/Result.hpp>
#include <sgrn/debug.hpp>

#ifdef DEBUG_CORE_DATABASE
#define DB_DEBUG_LOG(msg, ...) SGRN_DEBUG("CoreDB", msg __VA_OPT__(, ) __VA_ARGS__)
#define DB_ERROR_LOG(msg, ...) SGRN_ERROR("CoreDB", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define DB_DEBUG_LOG(...) ((void)0)
#define DB_ERROR_LOG(...) ((void)0)
#endif

namespace sgrn::datastore::core
{
/**
 * @brief Safely retrieve the default database client.
 */
inline Result<drogon::orm::DbClientPtr, BackendError> getDbClient() {
    if (!drogon::app().hasDbClient("default")) {
        DB_ERROR_LOG("Database client is not initialized or unavailable");
        RETURN_ERROR(BackendError(BackendErrorKind::Database, "Database client is not initialized or unavailable"));
    }
    auto db = drogon::app().getDbClient();
    if (!db) {
        DB_ERROR_LOG("Database client is not initialized or unavailable");
        RETURN_ERROR(BackendError(BackendErrorKind::Database, "Database client is not initialized or unavailable"));
    }
    return db;
}

/**
 * @brief Safely retrieve a Drogon plugin by type.
 */
template <typename T>
inline Result<T*, BackendError> getPlugin() {
    auto* p_plugin = drogon::app().getPlugin<T>();
    if (!p_plugin) {
        RETURN_ERROR(BackendError(BackendErrorKind::Runtime, "Required system plugin is not initialized"));
    }
    return p_plugin;
}

} // namespace sgrn::datastore::core

#undef DB_DEBUG_LOG
#undef DB_ERROR_LOG
