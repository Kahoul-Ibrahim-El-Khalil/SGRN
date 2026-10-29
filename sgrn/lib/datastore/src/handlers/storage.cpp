#include <drogon/orm/Exception.h>
#include <fmt/core.h>
#include <sgrn/datastore/core/db.hpp>
#include <sgrn/datastore/error/ApiErrors.hpp>
#include <sgrn/datastore/handlers/storage.hpp>
#include <sgrn/datastore/services/helpers/magic.hpp>
#include <sgrn/datastore/services/storage.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe_access.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/hashing.hpp>
#include <sgrn/utils/strings.hpp>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <json/value.h>
#include <map>
#include <openssl/evp.h>
#include <unordered_map>
#include <unordered_set>
#include <zstd.h>

#ifdef DEBUG_STORAGE_HANDLER
#define DEBUG_LOG(msg, ...) SGRN_DEBUG("StorageHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define INFO_LOG(msg, ...) SGRN_INFO("StorageHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define WARN_LOG(msg, ...) SGRN_WARN("StorageHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define ERROR_LOG(msg, ...) SGRN_ERROR("StorageHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define DEBUG_LOG(...) ((void)0)
#define INFO_LOG(...) ((void)0)
#define WARN_LOG(...) ((void)0)
#define ERROR_LOG(...) ((void)0)
#endif
// storage.cpp — StorageApiHandler construction + generated object CRUD
// route handlers. Cohesive feature groups live in storage_files.cpp,
// storage_drive.cpp and storage_uploads.cpp (same class, split by domain).

namespace sgrn::datastore::handlers::storage
{
using namespace drogon;

using sgrn::datastore::services::storage::StorageScope;
StorageApiHandler::StorageApiHandler()
    : IHandler<StorageApiHandler>(this, kRoutes, kItemRoutes)
    , storage_service_() {
}

Task<HttpResponsePtr> StorageApiHandler::handleGetConstraints(HttpRequestPtr tsp_req) {
    co_return co_await storage_service_.handleGetConstraints();
}

Task<HttpResponsePtr> StorageApiHandler::handleCreateObject(HttpRequestPtr tsp_req) {
    auto json = tsp_req->getJsonObject();
    if (!json) {
        co_return sgrn::createJsonErrorResponse("Invalid JSON", k400BadRequest);
    }
    // The filter has already validated the automated service and injected its ID into attributes.
    (*json)["automated_service_id"] = tsp_req->getAttributes()->get<int32_t>("automated_service_id");
    co_return co_await storage_service_.handleCreateObject(std::move(*json));
}

Task<HttpResponsePtr> StorageApiHandler::handleListObjects(HttpRequestPtr tsp_req) {
    // The filter has already validated the automated service and injected its ID into attributes.
    auto automated_service_id = tsp_req->getAttributes()->get<int32_t>("automated_service_id");
    co_return co_await storage_service_.handleListObjects(automated_service_id);
}

Task<HttpResponsePtr> StorageApiHandler::handleMoveObject(HttpRequestPtr tsp_req, std::string t_name) {
    if (!tsp_req->attributes()->find("session_json") || !tsp_req->attributes()->find("automated_service_id")) {
        co_return createJsonErrorResponse("No session found", k401Unauthorized);
    }
    auto session = tsp_req->getAttributes()->get<Json::Value>("session_json");
    if (!session) {
        co_return createJsonErrorResponse("No session found", k401Unauthorized);
    }

    const int32_t automated_service_id = tsp_req->getAttributes()->get<int32_t>("automated_service_id");
    const std::string name = std::move(t_name);
    auto json = tsp_req->getJsonObject();
    if (name.empty() || !json || !json->isMember("new_name") || !(*json)["new_name"].isString()) {
        co_return createJsonErrorResponse("name and new_name are required", k400BadRequest);
    }

    co_return co_await storage_service_.handleMoveObject(std::move(session), automated_service_id, name, (*json)["new_name"].asString());
}

Task<HttpResponsePtr> StorageApiHandler::handleDeleteObject(HttpRequestPtr tsp_req, std::string t_name) {
    if (!tsp_req->attributes()->find("session_json") || !tsp_req->attributes()->find("automated_service_id")) {
        co_return createJsonErrorResponse("No session found", k401Unauthorized);
    }
    auto session = tsp_req->getAttributes()->get<Json::Value>("session_json");
    if (!session) {
        co_return createJsonErrorResponse("No session found", k401Unauthorized);
    }

    const int32_t automated_service_id = tsp_req->getAttributes()->get<int32_t>("automated_service_id");
    const std::string name = std::move(t_name);
    if (name.empty()) {
        co_return createJsonErrorResponse("name is required", k400BadRequest);
    }

    co_return co_await storage_service_.handleDeleteObject(std::move(session), automated_service_id, name);
}

} // namespace sgrn::datastore::handlers::storage

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
