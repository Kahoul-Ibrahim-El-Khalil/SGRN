// src/sgrn/services/storage/service.hpp
#pragma once
#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/MultiPart.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <sgrn/datastore/BackendError.hpp>
#include <sgrn/datastore/services/helpers/storage.hpp>
#include <sgrn/debug.hpp>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sgrn::datastore::services::storage
{

struct StorageConfig {
    size_t threshold_compress_ram_mb = defaults::kThresholdCompressRamMb;
    size_t max_file_size_mb = defaults::kMaxFileSizeMb;
    uint8_t compression_level = defaults::kCompressionLevel;
    size_t compression_size_threshold_kb = defaults::kMinCompressSize;
    std::set<std::string> allowed_extensions = {"*"};
    std::set<std::string> prohibited_extensions;

    [[nodiscard]] bool isExtensionAllowed(std::string_view t_ext) const {
        if (prohibited_extensions.contains(std::string(t_ext)))
            return false;
        if (allowed_extensions.contains("*"))
            return true;
        return allowed_extensions.contains(std::string(t_ext));
    }

    [[nodiscard]] size_t thresholdCompressRamBytes() const {
        return threshold_compress_ram_mb * defaults::kBytesPerMb;
    }
    [[nodiscard]] size_t maxFileSizeBytes() const {
        return max_file_size_mb * defaults::kBytesPerMb;
    }

    static StorageConfig loadFromConfig();
};

class StorageService {
public:
    StorageService();

    drogon::Task<drogon::HttpResponsePtr> handleDownloadFileRequest(Json::Value t_session, std::string t_scope, std::string t_file_path);
    drogon::Task<drogon::HttpResponsePtr> handleUploadFileRequest(
        Json::Value t_session, std::string t_scope, std::string t_file_path, const drogon::HttpFile& t_http_file);
    drogon::Task<drogon::HttpResponsePtr> handleGetConstraints();
    drogon::Task<drogon::HttpResponsePtr> handleCreateDirectoryRequest(Json::Value t_session, std::string t_scope, std::string t_path);
    drogon::Task<drogon::HttpResponsePtr> handleCreateObject(const Json::Value& t_json);
    drogon::Task<drogon::HttpResponsePtr> handleListObjects(int32_t t_automated_service_id);
    drogon::Task<drogon::HttpResponsePtr> handleMoveObject(
        Json::Value t_session, int32_t t_automated_service_id, std::string t_name, std::string t_new_name);
    drogon::Task<drogon::HttpResponsePtr> handleDeleteObject(Json::Value t_session, int32_t t_automated_service_id, std::string t_name);
    drogon::Task<::sgrn::datastore::BackendResult<void>> deleteFile(drogon::orm::DbClientPtr tsp_db_client, int64_t t_file_id);
    drogon::Task<drogon::HttpResponsePtr> handleUploadFilesBatchRequest(
        Json::Value t_session, std::string t_scope, std::string t_base_path, const std::vector<drogon::HttpFile>& t_files);

    ::sgrn::datastore::BackendResult<sgrn::datastore::plugins::aws::S3Client*> S3Client() const;
    ::sgrn::datastore::BackendResult<sgrn::datastore::plugins::PostgrestClient*> PostgrestClient() const;
    ::sgrn::datastore::BackendResult<drogon::orm::DbClientPtr> getDbClient() const;

private:
    std::string default_bucket_;
    const StorageConfig& getConfig();

public:
    // Required by handlers that need to resolve scope manually (like listDrive)
    StorageScope parseScope(const std::string& t_scope_str);
    drogon::Task<::sgrn::datastore::BackendResult<ScopeContext>> resolveScopeSession(
        const Json::Value& t_session, StorageScope t_scope, const std::string& t_path);

private:
    drogon::Task<::sgrn::datastore::BackendResult<Json::Value>> uploadFile(UploadContext t_context, drogon::HttpFile t_file);
    drogon::Task<::sgrn::datastore::BackendResult<std::string>> downloadFile(std::string t_bucket, std::string t_key);
    drogon::Task<bool> objectExists(std::string t_bucket, std::string t_key);
    ::sgrn::datastore::BackendResult<bool> validateFileSize(size_t t_size);
    ::sgrn::datastore::BackendResult<bool> validateExtension(std::string_view t_filename);
    drogon::Task<::sgrn::datastore::BackendResult<bool>> validateUpload(const UploadContext& t_context, size_t t_size);

    // Upload strategies
    drogon::Task<::sgrn::datastore::BackendResult<std::pair<FileIdentity, std::string>>> processInMemory(
        const drogon::HttpFile& t_http_file, std::string t_mime_type, const UploadThresholds& t_thresholds, uint8_t t_compression_level,
        std::string t_file_data, FileHash t_original_hash);
    drogon::Task<::sgrn::datastore::BackendResult<std::pair<FileIdentity, std::filesystem::path>>> processStreaming(
        const drogon::HttpFile& t_http_file, std::string t_mime_type, const UploadThresholds& t_thresholds, uint8_t t_compression_level,
        FileHash t_original_hash);

    drogon::Task<::sgrn::datastore::BackendResult<void>> executeInMemoryUpload(drogon::orm::DbClientPtr tsp_transaction,
        UploadContext& t_context, const drogon::HttpFile& t_file, std::string t_mime_type, UploadThresholds t_thresholds,
        uint8_t t_compression_level, std::string t_file_data, FileHash t_original_hash);
    drogon::Task<::sgrn::datastore::BackendResult<void>> executeStreamingUpload(drogon::orm::DbClientPtr tsp_transaction,
        UploadContext& t_context, const drogon::HttpFile& t_file, std::string t_mime_type, UploadThresholds t_thresholds,
        uint8_t t_compression_level, FileHash t_original_hash);
    drogon::Task<::sgrn::datastore::BackendResult<void>> finalizeUpload(drogon::orm::DbClientPtr tsp_transaction, UploadContext& t_context);

    StorageConfig config_;
};
} // namespace sgrn::datastore::services::storage
