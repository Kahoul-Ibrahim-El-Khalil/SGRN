#pragma once
#include <drogon/HttpAppFramework.h>
#include <drogon/orm/DbClient.h>
#include <drogon/utils/coroutine.h>
#include <fmt/format.h>
#include <sgrn/datastore/BackendError.hpp>
#include <sgrn/datastore/plugins/aws/S3Client.hpp>
#include <sgrn/datastore/wire/auth.hpp>
#include <sgrn/utils/filesystem.hpp>
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
constexpr size_t kChunkingThresholdMb = 64;
constexpr size_t kChunkPartSizeMb = 12;
constexpr size_t kMinPartSizeMb = 5; // S3 hard minimum, all parts but the last
} // namespace defaults

// ============================================================================
// Data Structures
// ============================================================================

struct FileHash {
    std::string key;
    // SHA-512 (lowercase hex) of the ORIGINAL bytes (plaintext, uncompressed).
    // `key` is a SHA-512/base64url storage key and must never be used as a
    // version content hash: storage.file_versions.content_sha512 is char(128).
    std::string sha512_hex;
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
    int64_t object_id;
    std::optional<int32_t> user_id;
    std::optional<int32_t> automated_service_id;
    int64_t session_id;
    std::string extension;
    std::optional<int64_t> directory_id;
    std::optional<std::string> created_at;
    std::optional<std::string> key_id;
    bool is_encrypted{false};
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

    // Fast path: emit JSON directly without an intermediate Json::Value tree.
    // Use this for HTTP responses; toJson() is kept for callers that need
    // a dynamic JSON value for compatibility-boundary payloads.
    std::string toJsonString() const {
        std::string comp_fields;
        if (compression_algorithm.has_value()) {
            comp_fields = fmt::format(R"(,"compression_algorithm":"{}","compression_level":{})", *compression_algorithm,
                compression_level.has_value() ? std::to_string(*compression_level) : "null");
        }
        return fmt::format(
            R"({{"key":"{}","hash":"{}","extension":"{}","mime_type":"{}","is_compressed":{},"original_size":{},"final_size":{},"storage_extension":"{}"{}}})",
            hash.key, hash.key, extension, mime_type, is_compressed ? "true" : "false", original_size, final_size, extension, comp_fields);
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

    // Set by the upload strategies: "single" or "multipart". Returned in the
    // upload response so admins can verify which S3 path a file took.
    std::string upload_mode = "single";
    // Multipart trace (meaningful only when upload_mode == "multipart"):
    // how many parts were assembled + the configured part size in bytes.
    // Persisted per object so chunking stays traceable after the fact.
    std::optional<int64_t> part_count = std::nullopt;
    std::optional<int64_t> part_size_bytes = std::nullopt;

    std::optional<int64_t> directory_id = std::nullopt;
    std::optional<int64_t> object_id = std::nullopt;
    std::optional<int64_t> file_id = std::nullopt;
};

struct ProcessingResult {
    UploadContext context_;
    bool already_exists_ = false;
};

enum class StorageScope : uint8_t { Personal, AutomatedServices, Users, Domain };

struct ScopeContext {
    int64_t t_session_id{0};
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
// Helper Functions (Namespace: helpers)
// ============================================================================

namespace helpers
{

// Hash Computation
Result<FileHash, BackendError> computeHashInMemory(std::string_view t_data);
Result<FileHash, BackendError> computeHashFromFile(const fs::path& t_file_path);
drogon::Task<Result<FileHash, BackendError>> computeHashFromFileAsync(fs::path t_file_path);

// SHA-512 (hex) for checksum/ETag
Result<std::string, BackendError> computeSha512InMemory(std::string_view t_data);
Result<std::string, BackendError> computeSha512FromFile(const fs::path& t_file_path);
drogon::Task<Result<std::string, BackendError>> computeSha512FromFileAsync(fs::path t_file_path);

// Compression Utilities
drogon::Task<Result<std::string, BackendError>> compressInMemory(std::string&& t_data, uint8_t t_compression_level);
Result<fs::path, BackendError> compressFile(const fs::path& t_input_path, const fs::path& t_output_path, uint8_t t_compression_level);
drogon::Task<Result<fs::path, BackendError>> compressFileAsync(fs::path t_input_path, fs::path t_output_path, uint8_t t_compression_level);

// Database Operations
drogon::Task<Result<int64_t, BackendError>> insertObject(drogon::orm::DbClientPtr tsp_db_client, std::string t_bucket, std::string t_key,
    size_t t_size, size_t t_original_size, std::string t_sha512, bool t_is_compressed = false,
    std::optional<std::string> t_compression_algorithm = std::nullopt, std::optional<uint8_t> t_compression_level = std::nullopt);

// Chunking trace refresh after a successful multipart upload. The object row
// is inserted (defaults: single/NULLs) before the S3 upload; the multipart
// branch stamps the actual mode/part count afterwards, inside the same
// transaction, so the row always describes the bytes in the store.
drogon::Task<Result<void, BackendError>> updateObjectChunking(drogon::orm::DbClientPtr tsp_db_client, int64_t t_object_id,
    std::string_view t_upload_mode, std::optional<int64_t> t_part_count, std::optional<int64_t> t_part_size_bytes);

drogon::Task<Result<int64_t, BackendError>> insertFile(drogon::orm::DbClientPtr tsp_db_client, std::string t_name, int64_t t_object_id,
    std::optional<int32_t> t_user_id, std::optional<int32_t> t_automated_service_id, int64_t t_session_id, std::string t_extension,
    std::optional<int64_t> t_directory_id = std::nullopt, std::string t_domain = "", std::optional<std::string> t_key_id = std::nullopt,
    bool t_is_encrypted = false, bool t_versioning_enabled = true);

drogon::Task<Result<std::optional<int64_t>, BackendError>> resolveDirectoryPath(drogon::orm::DbClientPtr tsp_db_client,
    std::optional<int32_t> t_user_id, std::optional<int32_t> t_automated_service_id, int64_t t_session_id, std::string t_virtual_path,
    std::string t_domain = "");

drogon::Task<Result<std::optional<int64_t>, BackendError>> ensureDirectoryPath(drogon::orm::DbClientPtr tsp_db_client,
    std::optional<int32_t> t_user_id, std::optional<int32_t> t_automated_service_id, int64_t t_session_id, std::string t_virtual_path,
    std::string t_domain = "");

drogon::Task<std::optional<UserFileRecord>> getFile(drogon::orm::DbClientPtr tsp_db_client, int64_t t_file_id);
drogon::Task<std::optional<UserFileRecord>> findFileByPath(drogon::orm::DbClientPtr tsp_db_client, const ScopeContext& t_ctx);
drogon::Task<std::optional<std::string>> getObjectKey(drogon::orm::DbClientPtr tsp_db_client, int64_t t_object_id);
drogon::Task<std::optional<drogon_model::sgrn::storage::Formats>> getFormat(
    drogon::orm::DbClientPtr tsp_db_client, std::string t_extension);

// Temp File Management
Result<sgrn::utils::filesystem::TempFileGuard, BackendError> saveToTempFile(const drogon::HttpFile& t_file);
Result<sgrn::utils::filesystem::TempFileGuard, BackendError> saveToTempFile(const std::string& t_data);

drogon::Task<Result<ScopeContext, BackendError>> resolveScopeSession(drogon::orm::DbClientPtr tsp_db_client,
    const ::sgrn::datastore::auth::SessionClaims& t_session, StorageScope t_scope, const std::string& t_path);

drogon::Task<Result<UserFileRecord, BackendError>> fetchFileRecord(drogon::orm::DbClientPtr tsp_db_client, const ScopeContext& t_ctx);
drogon::Task<Result<std::string, BackendError>> resolveObjectKey(drogon::orm::DbClientPtr tsp_db_client, int64_t t_object_id);

} // namespace helpers

} // namespace sgrn::datastore::services::storage
