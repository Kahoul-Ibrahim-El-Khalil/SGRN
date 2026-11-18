#include <drogon/orm/Exception.h>
#include <fmt/core.h>
#include <sgrn/datastore/core/db.hpp>
#include <sgrn/datastore/error/ApiErrors.hpp>
#include <sgrn/datastore/handlers/storage.hpp>
#include <sgrn/datastore/services/storage.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe-access.hpp>
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
// route handlers. Cohesive feature groups live in storage-files.cpp,
// storage-drive.cpp and storage-uploads.cpp (same class, split by domain).

namespace sgrn::datastore::handlers::storage
{
using namespace drogon;

using sgrn::datastore::services::storage::StorageScope;
StorageApiHandler::StorageApiHandler()
    : IHandler<StorageApiHandler>(this, kRoutes, kItemRoutes)
    , storage_service_() {
    services::storage::startVersionCompactor(storage_service_);
}

Task<HttpResponsePtr> StorageApiHandler::handleGetConstraints(HttpRequestPtr tsp_req) {
    co_return co_await storage_service_.handleGetConstraints(tsp_req);
}

Task<HttpResponsePtr> StorageApiHandler::handleCreateObject(HttpRequestPtr tsp_req) {
    auto req_res = parseTypedRequest<services::storage::CreateObjectRequest>(tsp_req);
    if (req_res.hasError()) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{"Invalid JSON", k400BadRequest});
    }
    auto req = std::move(req_res.value());
    req.automated_service_id = tsp_req->getAttributes()->get<int32_t>("automated_service_id");
    co_return co_await storage_service_.handleCreateObject(req);
}

Task<HttpResponsePtr> StorageApiHandler::handleListObjects(HttpRequestPtr tsp_req) {
    // The filter has already validated the automated service and injected its ID into attributes.
    auto automated_service_id = tsp_req->getAttributes()->get<int32_t>("automated_service_id");
    co_return co_await storage_service_.handleListObjects(automated_service_id);
}

Task<HttpResponsePtr> StorageApiHandler::handleMoveObject(HttpRequestPtr tsp_req, std::string t_name) {
    if (!tsp_req->attributes()->find("session_claims") || !tsp_req->attributes()->find("automated_service_id")) {
        co_return createTypedResponse(BackendErrorResponse{"No session found", k401Unauthorized});
    }
    auto session = tsp_req->getAttributes()->get<sgrn::datastore::auth::SessionClaims>("session_claims");

    const int32_t automated_service_id = tsp_req->getAttributes()->get<int32_t>("automated_service_id");
    const std::string name = std::move(t_name);
    auto req_res = parseTypedRequest<services::storage::MoveObjectRequest>(tsp_req);
    if (name.empty() || req_res.hasError() || req_res->new_name.empty()) {
        co_return createTypedResponse(BackendErrorResponse{"name and new_name are required", k400BadRequest});
    }

    co_return co_await storage_service_.handleMoveObject(std::move(session), automated_service_id, name, req_res->new_name);
}

Task<HttpResponsePtr> StorageApiHandler::handleDeleteObject(HttpRequestPtr tsp_req, std::string t_name) {
    if (!tsp_req->attributes()->find("session_claims") || !tsp_req->attributes()->find("automated_service_id")) {
        co_return createTypedResponse(BackendErrorResponse{"No session found", k401Unauthorized});
    }
    auto session = tsp_req->getAttributes()->get<sgrn::datastore::auth::SessionClaims>("session_claims");

    const int32_t automated_service_id = tsp_req->getAttributes()->get<int32_t>("automated_service_id");
    const std::string name = std::move(t_name);
    if (name.empty()) {
        co_return createTypedResponse(BackendErrorResponse{"name is required", k400BadRequest});
    }

    co_return co_await storage_service_.handleDeleteObject(std::move(session), automated_service_id, name);
}

} // namespace sgrn::datastore::handlers::storage

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
