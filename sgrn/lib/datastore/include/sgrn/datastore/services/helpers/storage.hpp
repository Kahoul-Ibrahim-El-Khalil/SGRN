#pragma once
#include <drogon/HttpAppFramework.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <sgrn/datastore/BackendError.hpp>
#include <sgrn/datastore/plugins/aws/S3Client.hpp>
#include <sgrn/datastore/plugins/postgrest/PostgrestClient.hpp>
#include <sgrn/utils/hashing.hpp>
#include <filesystem>
#include <optional>
#include <orm/models/storage/Formats.h>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sgrn::datastore::services::storage
{

namespace fs = std::filesystem;
namespace defaults
{
constexpr size_t kThresholdCompressRamMb = 1;
constexpr size_t kMaxFileSizeMb = 100;
constexpr uint8_t kCompressionLevel = 3;
constexpr size_t kBytesPerMb = 1024ULL * 1024ULL;
constexpr size_t kBytesPerKb = 1024ULL;
constexpr size_t kMinCompressSize = 1;
} // namespace defaults

// ============================================================================
// Data Structures
// ============================================================================

struct FileHash {
    std::string key;
    size_t original_size = 0;

    [[nodiscard]] const std::string& hex() const {
        return key;
    }
};

struct UploadThresholds {
    size_t compress_ram_bytes;
    size_t compression_size_threshold;

    [[nodiscard]] bool shouldCompress(size_t t_file_size, bool t_is_compressible_mime) const {
        return t_is_compressible_mime && t_file_size >= compression_size_threshold;
    }

    [[nodiscard]] bool shouldProcessInMemory(size_t t_file_size) const {
        return t_file_size < compress_ram_bytes;
    }
};

struct UserFileRecord {
    int64_t id;
    std::string name;
    std::string full_path;
    int64_t t_object_id;
    std::optional<int32_t> user_id;
    std::optional<int32_t> automated_service_id;
    int32_t session_id;
    std::string extension;
    std::optional<int64_t> directory_id;
    std::optional<std::string> created_at;
    bool is_compressed_ = false;
    std::optional<std::string> compression_algorithm;
    std::optional<uint8_t> compression_level;
};

struct FileIdentity {
    FileHash hash;
    bool is_compressed = false;
    size_t original_size = 0;
    size_t final_size = 0;
    std::string extension = "";
    std::string mime_type = "";
    std::optional<std::string> compression_algorithm;
    std::optional<uint8_t> compression_level;

    std::string storageExtension() const {
        return extension;
    }

    Json::Value toJson() const {
        Json::Value json;
        json["key"] = hash.key;
        json["hash"] = hash.key;
        json["extension"] = extension;
        json["mime_type"] = mime_type;
        json["is_compressed"] = is_compressed;
        json["original_size"] = Json::UInt64(original_size);
        json["final_size"] = Json::UInt64(final_size);
        json["storage_extension"] = storageExtension();
        if (compression_algorithm.has_value()) {
            json["compression_algorithm"] = compression_algorithm.value();
        }
        if (compression_level.has_value()) {
            json["compression_level"] = compression_level.value();
        }
        return json;
    }
};

struct UploadContext {
    std::string original_filename = "";
    std::string virtual_path = "";
    int64_t session_id = 0;
    std::string domain = "";

    std::optional<int32_t> user_id;
    std::optional<int32_t> automated_service_id;
    std::string organisation;

    std::string bucket = "";
    FileIdentity identity = {};

    std::optional<int64_t> directory_id = std::nullopt;
    std::optional<int64_t> object_id = std::nullopt;
    std::optional<int64_t> file_id = std::nullopt;

    Json::Value toJson() const {
        Json::Value json;
        json["original_filename"] = original_filename;
        json["virtual_path"] = virtual_path;
        json["session_id"] = session_id;
        json["domain"] = domain;
        if (user_id.has_value())
            json["user_id"] = *user_id;
        if (automated_service_id.has_value())
            json["automated_service_id"] = *automated_service_id;

        json["organisation"] = organisation;
        json["bucket"] = bucket;
        json["identity"] = identity.toJson();

        if (directory_id.has_value())
            json["directory_id"] = Json::Int64(directory_id.value());
        if (object_id.has_value())
            json["object_id"] = Json::Int64(object_id.value());
        if (file_id.has_value())
            json["file_id"] = Json::Int64(file_id.value());

        return json;
    }
};

struct ProcessingResult {
    UploadContext context_;
    bool already_exists_ = false;

    Json::Value toJson() const {
        Json::Value json = context_.toJson();
        json["already_exists"] = already_exists_;
        json["deduplicated"] = already_exists_;
        return json;
    }
};

enum class StorageScope : uint8_t { Personal, AutomatedServices, Users, Domain };

struct ScopeContext {
    int32_t t_session_id{0};
    int32_t owner_id{0};
    std::optional<int32_t> user_id;
    std::optional<int32_t> t_automated_service_id;
    std::string display_name;
    bool is_automated_service{false};
    bool is_user{false};
    std::string actual_path;
    bool is_virtual_root{false};
    std::string domain;

    // VFS Capabilities
    bool can_read{true};
    bool can_write{true};
    bool can_delete{true};
    std::string allowed_subpath{"/"};
};

// ============================================================================
// RAII Temp File Guard
// ============================================================================

struct TempFileGuard {
    std::filesystem::path path;

    explicit TempFileGuard(std::filesystem::path t_path);
    ~TempFileGuard();

    TempFileGuard(const TempFileGuard&) = delete;
    TempFileGuard& operator=(const TempFileGuard&) = delete;

    TempFileGuard(TempFileGuard&& t_other) noexcept;
    TempFileGuard& operator=(TempFileGuard&& t_other) noexcept;
};

// ============================================================================
// Helper Functions (Namespace: helpers)
// ============================================================================

namespace helpers
{

// Hash Computation
::sgrn::datastore::BackendResult<FileHash> computeHashInMemory(std::string_view t_data);
::sgrn::datastore::BackendResult<FileHash> computeHashFromFile(const fs::path& t_file_path);
drogon::Task<::sgrn::datastore::BackendResult<FileHash>> computeHashFromFileAsync(fs::path t_file_path);

// Compression Utilities
drogon::Task<::sgrn::datastore::BackendResult<std::string>> compressInMemory(std::string&& t_data, uint8_t t_compression_level);
::sgrn::datastore::BackendResult<fs::path> compressFile(
    const fs::path& t_input_path, const fs::path& t_output_path, uint8_t t_compression_level);
drogon::Task<::sgrn::datastore::BackendResult<fs::path>> compressFileAsync(
    fs::path t_input_path, fs::path t_output_path, uint8_t t_compression_level);

// Path and Extension Utilities
std::string_view extractExtension(std::string_view t_filename);
std::string extractExtensionLower(std::string_view t_filename); // SEC: lowercase, for case-insensitive allowlist comparison
std::pair<std::string_view, std::string_view> splitExtension(std::string_view t_filename);
std::string inferMimeType(std::string_view t_filename);
bool isCompressibleMimeType(std::string_view t_mime_type);
bool verifyCompressionSignature(std::string_view t_data, std::string_view t_algorithm);

// Database Operations
drogon::Task<::sgrn::datastore::BackendResult<int64_t>> insertObject(drogon::orm::DbClientPtr tsp_db_client, std::string t_bucket,
    std::string t_key, size_t t_size, size_t t_original_size, bool t_is_compressed = false,
    std::optional<std::string> t_compression_algorithm = std::nullopt, std::optional<uint8_t> t_compression_level = std::nullopt);

drogon::Task<::sgrn::datastore::BackendResult<int64_t>> insertFile(drogon::orm::DbClientPtr tsp_db_client, std::string t_name,
    int64_t t_object_id, std::optional<int32_t> t_user_id, std::optional<int32_t> t_automated_service_id, int32_t t_session_id,
    std::string t_extension, std::optional<int64_t> t_directory_id = std::nullopt, std::string t_domain = "");

drogon::Task<::sgrn::datastore::BackendResult<std::optional<int64_t>>> resolveDirectoryPath(drogon::orm::DbClientPtr tsp_db_client,
    std::optional<int32_t> t_user_id, std::optional<int32_t> t_automated_service_id, int32_t t_session_id, std::string t_virtual_path,
    std::string t_domain = "");

drogon::Task<::sgrn::datastore::BackendResult<std::optional<int64_t>>> ensureDirectoryPath(drogon::orm::DbClientPtr tsp_db_client,
    std::optional<int32_t> t_user_id, std::optional<int32_t> t_automated_service_id, int32_t t_session_id, std::string t_virtual_path,
    std::string t_domain = "");

drogon::Task<std::optional<UserFileRecord>> getFile(drogon::orm::DbClientPtr tsp_db_client, int64_t t_file_id);
drogon::Task<std::optional<UserFileRecord>> findFileByPath(drogon::orm::DbClientPtr tsp_db_client, const ScopeContext& t_ctx);
drogon::Task<std::optional<std::string>> getObjectKey(drogon::orm::DbClientPtr tsp_db_client, int64_t t_object_id);
drogon::Task<std::optional<drogon_model::sgrn::storage::Formats>> getFormat(
    drogon::orm::DbClientPtr tsp_db_client, std::string t_extension);

// Temp File Management
fs::path generateTempPath(std::string_view t_prefix = "temp_");
::sgrn::datastore::BackendResult<TempFileGuard> saveToTempFile(const drogon::HttpFile& t_file);
::sgrn::datastore::BackendResult<TempFileGuard> saveToTempFile(const std::string& t_data);

drogon::Task<::sgrn::datastore::BackendResult<ScopeContext>> resolveScopeSession(
    drogon::orm::DbClientPtr tsp_db_client, const Json::Value& t_session, StorageScope t_scope, const std::string& t_path);

drogon::Task<::sgrn::datastore::BackendResult<UserFileRecord>> fetchFileRecord(
    drogon::orm::DbClientPtr tsp_db_client, const ScopeContext& t_ctx);
drogon::Task<::sgrn::datastore::BackendResult<std::string>> resolveObjectKey(drogon::orm::DbClientPtr tsp_db_client, int64_t t_object_id);

} // namespace helpers

} // namespace sgrn::datastore::services::storage
