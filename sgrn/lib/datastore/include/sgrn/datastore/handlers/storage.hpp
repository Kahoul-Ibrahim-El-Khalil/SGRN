#pragma once
#include <drogon/HttpController.h>
#include <drogon/drogon.h>
#include <fmt/format.h>
#include <sgrn/datastore/services/storage.hpp>
#include <sgrn/datastore/utils/IHandler.hpp>
#include <sgrn/debug.hpp>
#include <array>
#include <json/json.h>
#include <mutex>
#include <openssl/evp.h>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>
#include <zstd.h>

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
    // File metadata (direct DB query over storage.file_details)
    // =========================================================================
    drogon::Task<drogon::HttpResponsePtr> handleGetFilesMetadata(drogon::HttpRequestPtr tsp_req);

    // =========================================================================
    // File upload / download (supports HTTP Range headers for Resumable Downloads)
    // =========================================================================
    drogon::Task<drogon::HttpResponsePtr> handleFileRequest(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleAutomatedServiceFileRequest(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleAutomatedServiceGetFilesMetadata(drogon::HttpRequestPtr tsp_req);

    // =========================================================================
    // Resumable Chunked Uploads
    // =========================================================================
    drogon::Task<drogon::HttpResponsePtr> handleInitUploadSession(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleUploadChunk(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleGetUploadStatus(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleCompleteUploadSession(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleAbortUploadSession(drogon::HttpRequestPtr tsp_req);

    // =========================================================================
    // Automated Service Object Management
    // =========================================================================
    drogon::Task<drogon::HttpResponsePtr> handleCreateObject(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleListObjects(drogon::HttpRequestPtr tsp_req);
    drogon::Task<drogon::HttpResponsePtr> handleMoveObject(drogon::HttpRequestPtr tsp_req, std::string t_name);
    drogon::Task<drogon::HttpResponsePtr> handleDeleteObject(drogon::HttpRequestPtr tsp_req, std::string t_name);

    // =========================================================================
    // Presigned URLs
    // =========================================================================
    drogon::Task<drogon::HttpResponsePtr> handlePresignedUrl(drogon::HttpRequestPtr tsp_req);

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

    // ── Per-upload streaming pipeline state ─────────────────────────────────
    // Holds the open SHA-256 digest context and zstd compression stream for
    // a single resumable upload session. Created on first chunk, destroyed on
    // complete or abort.
    struct UploadSessionState {
        // SHA-256 over original (pre-compression) bytes — fed every chunk.
        EVP_MD_CTX* hash_ctx{nullptr};
        // Running count of original bytes received (for Content-Length on download).
        int64_t original_bytes{0};

        // zstd streaming compressor — feeds original bytes, emits compressed output.
        // Null when the MIME type is not compressible (e.g. already-compressed
        // formats); in that case chunks are stored verbatim.
        ZSTD_CStream* cstream{nullptr};
        // Compressed bytes not yet forming a full segment.
        std::vector<char> seg_buf;
        // How many complete segments have already been pushed to Garage.
        int32_t next_seg_idx{0};

        // Mime type (needed for S3 content-type on segment PUTs)
        std::string mime_type;
        // True when the server applied zstd compression to the payload.
        // Used by finalize to skip magic-byte sniff (bytes on S3 are zst,
        // not the declared type — that's expected and correct).
        bool server_compressed{false};
        // Uploaded chunks that have been processed into segments.
        int32_t processed_chunks{0};

        UploadSessionState() = default;
        UploadSessionState(const UploadSessionState&) = delete;
        UploadSessionState& operator=(const UploadSessionState&) = delete;
        ~UploadSessionState() {
            if (hash_ctx)
                EVP_MD_CTX_free(hash_ctx);
            if (cstream)
                ZSTD_freeCStream(cstream);
        }
    };

    // Guards upload_states_.
    std::mutex upload_states_mutex_;
    // upload_id -> state
    std::unordered_map<std::string, std::shared_ptr<UploadSessionState>> upload_states_;

    // Helper: get-or-create session state. Returns nullptr on alloc failure.
    std::shared_ptr<UploadSessionState> getOrCreateUploadState(
        const std::string& t_upload_id, const std::string& t_mime_type, uint8_t t_compression_level = 3);

    // Helper: remove session state (called on complete/abort).
    void removeUploadState(const std::string& t_upload_id);

    // Helpers for handleDriveList
    // Builders write directly into pre-allocated JSON array strings
    // (no intermediate Json::Value tree — eliminates per-row heap allocations).

    drogon::Task<::sgrn::datastore::BackendResult<void>> buildVirtualRootListing(std::string& t_folders_json,
        const std::string& t_namespace_prefix, ::sgrn::datastore::services::storage::StorageScope t_scope,
        const std::string& t_organisation, const drogon::orm::DbClientPtr& tsp_db_client);

    drogon::Task<std::pair<int32_t, int32_t>> buildNormalDriveListing(std::string& t_folders_json, std::string& t_files_json,
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

    inline static const std::array<IHandler<StorageApiHandler>::route_config, 20> kRoutes = {
        {{"/api/v1/storage/stats", &StorageApiHandler::handleGetStorageStats, {drogon::Get}, {"sgrn::datastore::filters::UserAuthFilter"}},

            {"/api/v1/storage/files/metadata", &StorageApiHandler::handleGetFilesMetadata, {drogon::Get},
                {"sgrn::datastore::filters::UserAuthFilter"}},

            {"/api/v1/storage/info", &StorageApiHandler::handleGetConstraints, {drogon::Get}, {}},

            {"/api/v1/storage/upload/init", &StorageApiHandler::handleInitUploadSession, {drogon::Post},
                {"sgrn::datastore::filters::UserAuthFilter"}},

            {"/api/v1/storage/upload/chunk", &StorageApiHandler::handleUploadChunk, {drogon::Put, drogon::Post},
                {"sgrn::datastore::filters::UserAuthFilter"}},

            {"/api/v1/storage/upload/status", &StorageApiHandler::handleGetUploadStatus, {drogon::Get},
                {"sgrn::datastore::filters::UserAuthFilter"}},

            {"/api/v1/storage/upload/complete", &StorageApiHandler::handleCompleteUploadSession, {drogon::Post},
                {"sgrn::datastore::filters::UserAuthFilter"}},

            {"/api/v1/storage/upload/abort", &StorageApiHandler::handleAbortUploadSession, {drogon::Delete},
                {"sgrn::datastore::filters::UserAuthFilter"}},

            {"/api/v1/storage/drive/list", &StorageApiHandler::handleDriveList, {drogon::Get},
                {"sgrn::datastore::filters::UserAuthFilter"}},

            {"/api/v1/storage/drive/mkdir", &StorageApiHandler::handleCreateDirectory, {drogon::Post},
                {"sgrn::datastore::filters::UserAuthFilter"}},

            {"/api/v1/storage/drive/move", &StorageApiHandler::handleMove, {drogon::Patch}, {"sgrn::datastore::filters::UserAuthFilter"}},

            {"/api/v1/storage/drive/delete", &StorageApiHandler::handleDelete, {drogon::Delete},
                {"sgrn::datastore::filters::UserAuthFilter"}},

            {"/api/v1/storage/drive/zip", &StorageApiHandler::handleRecursiveDownload, {drogon::Get},
                {"sgrn::datastore::filters::UserAuthFilter"}},

            {"/api/v1/storage/drive/bulk", &StorageApiHandler::handleBulkAction, {drogon::Post},
                {"sgrn::datastore::filters::UserAuthFilter"}},

            {"/api/v1/storage/files/presign", &StorageApiHandler::handlePresignedUrl, {drogon::Get},
                {"sgrn::datastore::filters::UserAuthFilter"}},

            {"/api/v1/automated-service/objects", &StorageApiHandler::handleCreateObject, {drogon::Post},
                {"sgrn::datastore::filters::AutomatedServiceAuthFilter"}},

            {"/api/v1/automated-service/objects", &StorageApiHandler::handleListObjects, {drogon::Get},
                {"sgrn::datastore::filters::AutomatedServiceAuthFilter"}},

            // 2. Wildcard/Greedy routes must come LAST
            {"/api/v1/storage/files", &StorageApiHandler::handleFileRequest, {drogon::Get, drogon::Post},
                {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::DecompressionFilter"}},

            {"/api/v1/storage/automated-service/files", &StorageApiHandler::handleAutomatedServiceFileRequest, {drogon::Get, drogon::Post},
                {"sgrn::datastore::filters::AutomatedServiceAuthFilter", "sgrn::datastore::filters::DecompressionFilter"}},

            {"/api/v1/storage/automated-service/metadata", &StorageApiHandler::handleAutomatedServiceGetFilesMetadata, {drogon::Get},
                {"sgrn::datastore::filters::AutomatedServiceAuthFilter"}}}};

    // Item routes: `{name}` is mapped by Drogon onto the handler's second
    // argument (see IHandler::item_route_config).
    inline static const std::array<IHandler<StorageApiHandler>::item_route_config, 2> kItemRoutes{
        {{"/api/v1/automated-service/objects/{name}", &StorageApiHandler::handleMoveObject, {drogon::Patch},
             {"sgrn::datastore::filters::AutomatedServiceAuthFilter"}},

            {"/api/v1/automated-service/objects/{name}", &StorageApiHandler::handleDeleteObject, {drogon::Delete},
                {"sgrn::datastore::filters::AutomatedServiceAuthFilter"}}}};
};

} // namespace sgrn::datastore::handlers::storage
