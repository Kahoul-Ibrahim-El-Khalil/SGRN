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
#include <algorithm>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace sgrn::datastore::services::storage
{

struct StorageConfig {
    std::string default_bucket = "sgrn-uploads";
    size_t threshold_compress_ram_mb = defaults::kThresholdCompressRamMb;
    size_t max_file_size_mb = defaults::kMaxFileSizeMb;
    uint8_t compression_level = defaults::kCompressionLevel;
    size_t compression_size_threshold_kb = defaults::kMinCompressSize;
    size_t chunking_threshold_mb = defaults::kChunkingThresholdMb;
    size_t chunk_part_size_mb = defaults::kChunkPartSizeMb;
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
    [[nodiscard]] size_t chunkingThresholdBytes() const {
        return chunking_threshold_mb * defaults::kBytesPerMb;
    }
    [[nodiscard]] size_t chunkPartSizeBytes() const {
        // clamp defensively even though loadFromConfig() also validates this
        return std::max(chunk_part_size_mb, defaults::kMinPartSizeMb) * defaults::kBytesPerMb;
    }

    // Serialize the live-effective `s3` block (used by the admin
    // system-config GET so the dashboard can show what is actually in force,
    // not just what the file says).
    [[nodiscard]] Json::Value toJson() const {
        Json::Value s3(Json::objectValue);
        s3["default_bucket"] = default_bucket;
        s3["threshold_compress_ram_mb"] = Json::UInt64(threshold_compress_ram_mb);
        s3["max_file_size_mb"] = Json::UInt64(max_file_size_mb);
        s3["compression_level"] = static_cast<Json::UInt>(compression_level);
        s3["compression_size_threshold_kb"] = Json::UInt64(compression_size_threshold_kb);
        s3["chunking_threshold_mb"] = Json::UInt64(chunking_threshold_mb);
        s3["chunk_part_size_mb"] = Json::UInt64(chunk_part_size_mb);
        Json::Value allowed(Json::arrayValue);
        for (const auto& ext : allowed_extensions) {
            allowed.append(ext);
        }
        s3["allowed_extensions"] = std::move(allowed);
        Json::Value prohibited(Json::arrayValue);
        for (const auto& ext : prohibited_extensions) {
            prohibited.append(ext);
        }
        s3["prohibited_extensions"] = std::move(prohibited);
        return s3;
    }

    static StorageConfig loadFromConfig();
    // Pure parse of an `s3` block (no Drogon singleton): shared by
    // loadFromConfig() and the admin config hot-apply path. Unit-testable.
    static StorageConfig fromS3Json(const Json::Value& t_s3);
};

// Process-wide live storage config. StorageService publishes at construction;
// the admin system-config endpoint republishes after a validated save, which
// is what makes `s3.*` dashboard edits take effect without a restart.
// Readers take a cheap copy (a few sizes + two tiny sets) — never hold the
// returned value across co_await points; snapshot again after resuming.
void publishStorageConfig(StorageConfig t_cfg);
[[nodiscard]] StorageConfig currentStorageConfig();

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
    ::sgrn::datastore::BackendResult<drogon::orm::DbClientPtr> getDbClient() const;

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
    // Formats-registry veto: an explicitly disallowed format (is_allowed=false
    // in storage.formats, tunable from the admin dashboard) blocks the upload
    // even when the config allowlists would permit it. Unknown extensions (no
    // registry row) fall through to the config lists. Single + batch uploads
    // both funnel through uploadFile(), so one check covers every entry path.
    drogon::Task<::sgrn::datastore::BackendResult<void>> enforceFormatAllowed(
        drogon::orm::DbClientPtr tsp_db_client, std::string_view t_filename);

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
};
} // namespace sgrn::datastore::services::storage
