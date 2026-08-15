#pragma once
#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <sgrn/datastore/services/storage.hpp>
#include <sgrn/datastore/utils/IHandler.hpp>
#include <sgrn/debug.hpp>
#include <array>
#include <regex>

namespace sgrn::datastore::handlers::storage
{

class StorageApiHandler : public IHandler<StorageApiHandler> {
public:
    StorageApiHandler();

    // =========================================================================
    // Configuration
    // =========================================================================
    drogon::Task<drogon::HttpResponsePtr> handleGetConstraints(drogon::HttpRequestPtr tsp_req);

    // =========================================================================
    // File metadata (PostgREST proxy)
    // =========================================================================
    drogon::Task<drogon::HttpResponsePtr> handleGetFilesMetadata(drogon::HttpRequestPtr tsp_req);

    // =========================================================================
    // File upload / download
    // =========================================================================
    drogon::Task<drogon::HttpResponsePtr> handleFileRequest(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleAutomatedServiceFileRequest(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleAutomatedServiceGetFilesMetadata(drogon::HttpRequestPtr tsp_req);

    // =========================================================================
    // Automated Service Object Management
    // =========================================================================
    drogon::Task<drogon::HttpResponsePtr> handleCreateObject(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleListObjects(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleMoveObject(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleDeleteObject(drogon::HttpRequestPtr tsp_req);

    // =========================================================================
    // Drive directory listing
    // =========================================================================
    drogon::Task<drogon::HttpResponsePtr> handleDriveList(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleGetStorageStats(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleCreateDirectory(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleMove(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleDelete(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleRecursiveDownload(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleBulkAction(drogon::HttpRequestPtr tsp_req);

private:
    ::sgrn::datastore::services::storage::StorageService storage_service_;

    // ── Refactoring Helpers ──────────────────────────────────────────────────

    // Helpers for handleDriveList

    drogon::Task<::sgrn::datastore::BackendResult<void>> buildVirtualRootListing(Json::Value& t_folders_array,
        const std::string& t_namespace_prefix, ::sgrn::datastore::services::storage::StorageScope t_scope,
        const std::string& t_organisation, const drogon::orm::DbClientPtr& tsp_db_client);

    drogon::Task<std::pair<int32_t, int32_t>> buildNormalDriveListing(Json::Value& t_folders_array, Json::Value& t_files_array,
        const std::string& t_namespace_prefix, const std::string& t_current_path, int32_t t_user_id, int32_t t_target_owner_id,
        ::sgrn::datastore::services::storage::StorageScope t_scope, const drogon::orm::DbClientPtr& tsp_db_client, int32_t t_limit,
        int32_t t_page, const std::string& t_search);

    // Helpers for handleMove
    drogon::Task<drogon::HttpResponsePtr> moveFile(const std::shared_ptr<drogon::orm::Transaction>& tsp_transaction, int64_t t_entity_id,
        int32_t t_user_id, bool t_is_admin, std::optional<int64_t> t_target_parent_id, std::optional<std::string> t_target_name);

    drogon::Task<drogon::HttpResponsePtr> moveFolder(const std::shared_ptr<drogon::orm::Transaction>& tsp_transaction, int64_t t_entity_id,
        int32_t t_user_id, bool t_is_admin, std::optional<int64_t> t_target_parent_id, std::optional<std::string> t_target_name);

    drogon::Task<drogon::HttpResponsePtr> deleteFile(
        const drogon::orm::DbClientPtr& tsp_db_client, int64_t t_entity_id, int32_t t_user_id, bool t_is_admin);

    drogon::Task<drogon::HttpResponsePtr> deleteFolder(
        const drogon::orm::DbClientPtr& tsp_db_client, int64_t t_entity_id, int32_t t_user_id, bool t_is_admin);

    inline static const std::array<IHandler<StorageApiHandler>::route_config, 16> kRoutes{{// 1. Static/Exact routes must come FIRST to
                                                                                           // avoid
                                                                                           // being caught by wildcards
        {"/api/v1/storage/stats", &StorageApiHandler::handleGetStorageStats, {drogon::Get}, {"sgrn::datastore::filters::UserAuthFilter"}},

        {"/api/v1/storage/files/metadata", &StorageApiHandler::handleGetFilesMetadata, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter"}},

        {"/api/v1/storage/info", &StorageApiHandler::handleGetConstraints, {drogon::Get}, {}},

        {"/api/v1/storage/drive/list", &StorageApiHandler::handleDriveList, {drogon::Get}, {"sgrn::datastore::filters::UserAuthFilter"}},

        {"/api/v1/storage/drive/mkdir", &StorageApiHandler::handleCreateDirectory, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter"}},

        {"/api/v1/storage/drive/move", &StorageApiHandler::handleMove, {drogon::Patch}, {"sgrn::datastore::filters::UserAuthFilter"}},

        {"/api/v1/storage/drive/delete", &StorageApiHandler::handleDelete, {drogon::Delete}, {"sgrn::datastore::filters::UserAuthFilter"}},

        {"/api/v1/storage/drive/zip", &StorageApiHandler::handleRecursiveDownload, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter"}},

        {"/api/v1/storage/drive/bulk", &StorageApiHandler::handleBulkAction, {drogon::Post}, {"sgrn::datastore::filters::UserAuthFilter"}},

        {"/api/v1/automated-service/objects", &StorageApiHandler::handleCreateObject, {drogon::Post},
            {"sgrn::datastore::filters::AutomatedServiceAuthFilter"}},

        {"/api/v1/automated-service/objects", &StorageApiHandler::handleListObjects, {drogon::Get},
            {"sgrn::datastore::filters::AutomatedServiceAuthFilter"}},

        {"/api/v1/automated-service/objects/{name}", &StorageApiHandler::handleMoveObject, {drogon::Patch},
            {"sgrn::datastore::filters::AutomatedServiceAuthFilter"}},

        {"/api/v1/automated-service/objects/{name}", &StorageApiHandler::handleDeleteObject, {drogon::Delete},
            {"sgrn::datastore::filters::AutomatedServiceAuthFilter"}},

        // 2. Wildcard/Greedy routes must come LAST
        {"/api/v1/storage/files", &StorageApiHandler::handleFileRequest, {drogon::Get, drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::DecompressionFilter"}},

        {"/api/v1/storage/automated-service/files", &StorageApiHandler::handleAutomatedServiceFileRequest, {drogon::Get, drogon::Post},
            {"sgrn::datastore::filters::AutomatedServiceAuthFilter", "sgrn::datastore::filters::DecompressionFilter"}},

        {"/api/v1/storage/automated-service/metadata", &StorageApiHandler::handleAutomatedServiceGetFilesMetadata, {drogon::Get},
            {"sgrn::datastore::filters::AutomatedServiceAuthFilter"}}}};
};

} // namespace sgrn::datastore::handlers::storage
