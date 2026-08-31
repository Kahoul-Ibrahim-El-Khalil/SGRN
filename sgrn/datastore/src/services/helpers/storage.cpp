#include <sgrn/datastore/plugins/threadpool/Threadpool.hpp>
#include <sgrn/datastore/services/helpers/storage.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/safe_access.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <sgrn/utils/hashing.hpp>
#include <sgrn/utils/jsoncpp.hpp>
#include <sgrn/utils/mime.hpp>
#include <sgrn/utils/strings.hpp>
#include <sgrn/utils/time.hpp>
#include <algorithm>
#include <fstream>
#include <orm/models/storage/Formats.h>
#include <orm/models/storage/Objects.h>
#include <random>

namespace sgrn::datastore::services::storage
{
using namespace sgrn::datastore;
using namespace sgrn::utils;
using namespace drogon;
using Formats = drogon_model::sgrn::storage::Formats;
// ============================================================================
// RAII Temp File Guard
// ============================================================================

TempFileGuard::TempFileGuard(fs::path t_path)
    : path(std::move(t_path)) {
}

TempFileGuard::~TempFileGuard() {
    if (!path.empty()) {
        std::error_code error_code;
        fs::remove(path, error_code);
    }
}

TempFileGuard::TempFileGuard(TempFileGuard&& t_other) noexcept
    : path(std::move(t_other.path)) {
    t_other.path.clear();
}

TempFileGuard& TempFileGuard::operator=(TempFileGuard&& t_other) noexcept {
    if (this != &t_other) {
        path = std::move(t_other.path);
        t_other.path.clear();
    }
    return *this;
}

namespace helpers
{

// ============================================================================
// Hash Computation
// ============================================================================

BackendResult<FileHash> computeHashInMemory(std::string_view t_data) {
    FileHash result;
    Result<std::string> hash = utils::computeSha512Data(t_data, utils::HashEncoding::base64url);
    if (!hash.has_value()) {
        return BackendResult<FileHash>::Error(BackendError(BackendErrorKind::Hashing, std::move(hash.error())));
    }
    result.key = std::move(hash.value());
    result.original_size = t_data.size();
    return result;
}

BackendResult<FileHash> computeHashFromFile(const fs::path& t_file_path) {
    FileHash result;
    Result<std::string> hash = utils::computeSha512File(t_file_path, utils::HashEncoding::base64url);
    if (!hash.has_value()) {
        return BackendResult<FileHash>::Error(BackendError(BackendErrorKind::Hashing, std::move(hash.error())));
    }
    result.key = std::move(hash.value());
    result.original_size = fs::file_size(t_file_path);
    return result;
}

Task<BackendResult<FileHash>> computeHashFromFileAsync(fs::path t_file_path) {
    BackendResult<plugins::Threadpool*> tp_res = core::getPlugin<plugins::Threadpool>();
    if (!tp_res) {
        co_return BackendResult<FileHash>::Error(tp_res.error());
    }
    plugins::Threadpool* p_tp = tp_res.value();
    co_return co_await utils::runInPool(p_tp->getPool(), [t_path = std::move(t_file_path)]() -> BackendResult<FileHash> {
        try {
            return computeHashFromFile(t_path);
        } catch (const std::exception& ex) {
            return BackendResult<FileHash>::Error(
                BackendError(BackendErrorKind::Hashing, std::format("Hash computation failed: {}", ex.what())));
        }
    });
}

// ============================================================================
// Compression Utilities
// ============================================================================

Task<BackendResult<std::string>> compressInMemory(std::string&& t_data, uint8_t t_compression_level) {
    BackendResult<plugins::Threadpool*> tp_res = core::getPlugin<plugins::Threadpool>();
    if (!tp_res) {
        co_return BackendResult<std::string>::Error(tp_res.error());
    }
    plugins::Threadpool* p_tp = tp_res.value();
    co_return co_await utils::runInPool(
        p_tp->getPool(), [buffer = std::move(t_data), level = t_compression_level]() -> BackendResult<std::string> {
            Result<std::string> tp_res = utils::compressStringZstd(buffer, level);
            if (!tp_res) {
                return BackendResult<std::string>::Error(BackendError(BackendErrorKind::Compression, std::move(tp_res.error())));
            }
            return std::move(tp_res.value());
        });
}

BackendResult<fs::path> compressFile(const fs::path& t_input_path, const fs::path& t_output_path, uint8_t t_compression_level) {
    Result<size_t> tp_res = sgrn::utils::compression::compressFileStreamingZstd(t_input_path, t_output_path, t_compression_level);
    if (!tp_res.has_value()) {
        return BackendResult<fs::path>::Error(
            BackendError(BackendErrorKind::Runtime, std::format("Compression failed: {}", tp_res.error())));
    }
    return t_output_path;
}

Task<BackendResult<fs::path>> compressFileAsync(fs::path t_input_path, fs::path t_output_path, uint8_t t_compression_level) {
    BackendResult<plugins::Threadpool*> tp_res = core::getPlugin<plugins::Threadpool>();
    if (!tp_res) {
        co_return BackendResult<fs::path>::Error(tp_res.error());
    }

    plugins::Threadpool* p_tp = tp_res.value();
    co_return co_await p_tp->run(
        [in = std::move(t_input_path), out = std::move(t_output_path), level = t_compression_level]() -> BackendResult<fs::path> {
            Result<size_t> tp_res = utils::compression::compressFileStreamingZstd(in, out, level);
            if (tp_res.hasError()) {
                return BackendResult<fs::path>::Error(BackendError(BackendErrorKind::Runtime, std::move(tp_res.error())));
            }
            return out;
        });
}

// ============================================================================
// Path and Extension Utilities
// ============================================================================

std::string_view extractExtension(std::string_view t_filename) {
    return splitExtension(t_filename).first;
}

// SEC: Returns the extension in lowercase for case-insensitive allowlist/blocklist comparison.
// Prevents bypass via mixed-case extensions like ".PHP" or ".JSP".
std::string extractExtensionLower(std::string_view t_filename) {
    std::string_view ext = extractExtension(t_filename);
    std::string lower(ext);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char t_c) { return std::tolower(t_c); });
    return lower;
}

std::pair<std::string_view, std::string_view> splitExtension(std::string_view t_filename) {
    size_t last_dot = t_filename.rfind('.');
    if (last_dot == std::string_view::npos || last_dot == t_filename.length() - 1) {
        return {"", ""};
    }

    std::string_view last_ext = t_filename.substr(last_dot + 1);

    if (last_ext == "zst" || last_ext == "gz" || last_ext == "br" || last_ext == "bz2" || last_ext == "xz" || last_ext == "zip" ||
        last_ext == "7z") {
        size_t second_last_dot = t_filename.substr(0, last_dot).rfind('.');
        if (second_last_dot != std::string_view::npos) {
            std::string_view base_ext = t_filename.substr(second_last_dot + 1, last_dot - second_last_dot - 1);
            return {base_ext, last_ext};
        }
        return {"", last_ext};
    }

    return {last_ext, ""};
}

std::string inferMimeType(std::string_view t_filename) {
    std::string_view ext = extractExtension(t_filename);
    std::optional<sgrn::utils::mime::MimeType> t_mime_type = sgrn::utils::mime::fromExtension(ext);
    return t_mime_type ? std::string(sgrn::utils::mime::toMimeStringOrDefault(*t_mime_type)) : "application/octet-stream";
}

bool isCompressibleMimeType(std::string_view t_mime_type) {
    std::optional<sgrn::utils::mime::MimeType> parsed_mime_type = sgrn::utils::mime::fromMimeString(t_mime_type);
    return parsed_mime_type ? sgrn::utils::mime::isCompressible(*parsed_mime_type) : false;
}

bool verifyCompressionSignature(std::string_view t_data, std::string_view t_algorithm) {
    if (t_algorithm == "zstd") {
        if (t_data.size() < 4)
            return false;
        // Zstd magic number: 0xFD2FB528 (little endian)
        return (uint8_t)t_data[0] == 0x28 && (uint8_t)t_data[1] == 0xB5 && (uint8_t)t_data[2] == 0x2F && (uint8_t)t_data[3] == 0xFD;
    }
    return false;
}

// ============================================================================
// Database Operations
// ============================================================================

Task<BackendResult<int64_t>> insertObject(drogon::orm::DbClientPtr tsp_db_client, std::string t_bucket, std::string t_key, size_t t_size,
    size_t t_original_size, bool t_is_compressed, std::optional<std::string> t_compression_algorithm,
    std::optional<uint8_t> t_compression_level) {
    try {
        drogon::orm::Result result = co_await tsp_db_client->execSqlCoro(
            "SELECT storage.upsert_object($1, $2, $3, $4, 'MINIO', $5, $6, $7) AS id", std::move(t_bucket), std::move(t_key),
            static_cast<int64_t>(t_size), static_cast<int64_t>(t_original_size), t_is_compressed, t_compression_algorithm,
            t_compression_level.has_value() ? std::optional<int32_t>(static_cast<int32_t>(*t_compression_level)) : std::nullopt);

        if (result.empty() || result[0]["id"].isNull()) {
            co_return BackendResult<int64_t>::Error(BackendError(BackendErrorKind::Database, "Failed to upsert object: no ID returned"));
        }
        co_return result[0]["id"].as<int64_t>();
    } catch (const std::exception& ex) {
        co_return BackendResult<int64_t>::Error(
            BackendError(BackendErrorKind::Database, std::format("Failed to upsert object: {}", ex.what())));
    }
}

Task<BackendResult<int64_t>> insertFile(drogon::orm::DbClientPtr tsp_db_client, std::string t_name, int64_t t_object_id,
    std::optional<int32_t> t_user_id, std::optional<int32_t> t_automated_service_id, int32_t t_session_id, std::string t_extension,
    std::optional<int64_t> t_directory_id, std::string t_domain) {
    try {
        // Ensure the extension exists in the formats table (Safe Auto-Registration)
        if (!t_extension.empty()) {
            co_await tsp_db_client->execSqlCoro(
                "INSERT INTO storage.formats (extension, mime_type, description, is_compressed, is_allowed) "
                "VALUES ($1, 'application/octet-stream', 'Automatically registered format', true, true) "
                "ON CONFLICT (extension) DO NOTHING",
                t_extension);
        }

        drogon::orm::Result tp_res = co_await tsp_db_client->execSqlCoro(
            "INSERT INTO storage.files (name, object_id, user_id, automated_service_id, session_id, extension, directory_id, domain) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8) RETURNING id",
            std::move(t_name), t_object_id, t_user_id, t_automated_service_id, t_session_id,
            t_extension.empty() ? std::optional<std::string>(std::nullopt) : std::optional<std::string>(t_extension), t_directory_id,
            t_domain.empty() ? std::optional<std::string>(std::nullopt) : std::optional<std::string>(t_domain));

        if (tp_res.empty()) {
            co_return BackendResult<int64_t>::Error(BackendError(BackendErrorKind::Database, "Failed to insert file"));
        }
        co_return tp_res[0]["id"].as<int64_t>();
    } catch (const std::exception& ex) {
        co_return BackendResult<int64_t>::Error(
            BackendError(BackendErrorKind::Database, std::format("Failed to insert file: {}", ex.what())));
    }
}

Task<BackendResult<std::optional<int64_t>>> resolveDirectoryPath(drogon::orm::DbClientPtr tsp_db_client, std::optional<int32_t> t_user_id,
    std::optional<int32_t> t_automated_service_id, int32_t t_session_id, std::string t_virtual_path, std::string t_domain) {
    try {
        if (t_virtual_path.empty() || t_virtual_path == "/") {
            co_return std::optional<int64_t>(std::nullopt);
        }

        std::string dir_path = fs::path(t_virtual_path).parent_path().string();
        if (dir_path.empty() || dir_path == "." || dir_path == "/") {
            co_return std::optional<int64_t>(std::nullopt);
        }

        drogon::orm::Result tp_res = co_await tsp_db_client->execSqlCoro(
            "SELECT storage.ensure_directory_path($1, $2, $3, $4, $5) AS dir_id", t_user_id, t_automated_service_id, t_session_id, dir_path,
            t_domain.empty() ? std::optional<std::string>(std::nullopt) : std::optional<std::string>(t_domain));

        if (tp_res.empty() || tp_res[0]["dir_id"].isNull()) {
            co_return std::optional<int64_t>(std::nullopt);
        }
        co_return tp_res[0]["dir_id"].as<int64_t>();
    } catch (const std::exception& ex) {
        co_return BackendResult<std::optional<int64_t>>::Error(
            BackendError(BackendErrorKind::Database, std::format("Failed to resolve directory: {}", ex.what())));
    }
}

Task<BackendResult<std::optional<int64_t>>> ensureDirectoryPath(drogon::orm::DbClientPtr tsp_db_client, std::optional<int32_t> t_user_id,
    std::optional<int32_t> t_automated_service_id, int32_t t_session_id, std::string t_virtual_path, std::string t_domain) {
    try {
        if (t_virtual_path.empty() || t_virtual_path == "/") {
            co_return std::optional<int64_t>(std::nullopt);
        }

        drogon::orm::Result tp_res = co_await tsp_db_client->execSqlCoro(
            "SELECT storage.ensure_directory_path($1, $2, $3, $4, $5) AS dir_id", t_user_id, t_automated_service_id, t_session_id,
            t_virtual_path, t_domain.empty() ? std::optional<std::string>(std::nullopt) : std::optional<std::string>(t_domain));

        if (tp_res.empty() || tp_res[0]["dir_id"].isNull()) {
            co_return std::optional<int64_t>(std::nullopt);
        }
        co_return tp_res[0]["dir_id"].as<int64_t>();
    } catch (const std::exception& ex) {
        co_return BackendResult<std::optional<int64_t>>::Error(
            BackendError(BackendErrorKind::Database, std::format("Failed to ensure directory: {}", ex.what())));
    }
}

Task<std::optional<UserFileRecord>> getFile(drogon::orm::DbClientPtr tsp_db_client, int64_t t_file_id) {
    try {
        drogon::orm::Result tp_res = co_await tsp_db_client->execSqlCoro(
            "SELECT f.id, f.name, f.full_path, f.object_id, f.user_id, f.automated_service_id, f.session_id, f.extension, "
            "f.directory_id, f.created_at, so.is_compressed, so.compression_algorithm, so.compression_level "
            "FROM storage.files f JOIN storage.objects so ON f.object_id = so.id WHERE f.id = $1",
            t_file_id);

        if (tp_res.empty())
            co_return std::nullopt;
        const drogon::orm::Row& row = tp_res[0];
        UserFileRecord rec;
        rec.id = row["id"].as<int64_t>();
        rec.name = row["name"].as<std::string>();
        rec.full_path = row["full_path"].as<std::string>();
        rec.t_object_id = row["object_id"].as<int64_t>();
        if (!row["user_id"].isNull())
            rec.user_id = row["user_id"].as<int32_t>();
        if (!row["automated_service_id"].isNull())
            rec.automated_service_id = row["automated_service_id"].as<int32_t>();
        rec.session_id = row["session_id"].as<int32_t>();
        rec.extension = row["extension"].as<std::string>();
        if (!row["directory_id"].isNull())
            rec.directory_id = row["directory_id"].as<int64_t>();
        rec.created_at = row["created_at"].as<std::string>();
        rec.is_compressed_ = row["is_compressed"].as<bool>();
        if (!row["compression_algorithm"].isNull())
            rec.compression_algorithm = row["compression_algorithm"].as<std::string>();
        if (!row["compression_level"].isNull())
            rec.compression_level = static_cast<uint8_t>(row["compression_level"].as<int32_t>());
        co_return rec;
    } catch (...) {
        co_return std::nullopt;
    }
}

Task<std::optional<UserFileRecord>> findFileByPath(drogon::orm::DbClientPtr tsp_db_client, const ScopeContext& t_ctx) {
    try {
        const std::string query = t_ctx.is_automated_service
                                      ? "SELECT f.id, f.name, f.full_path, f.object_id, f.user_id, f.automated_service_id, f.session_id, "
                                        "f.extension, f.directory_id, f.created_at, so.is_compressed, so.compression_algorithm, "
                                        "so.compression_level FROM storage.files f JOIN storage.objects so ON f.object_id = so.id "
                                        "WHERE f.automated_service_id = $1 AND f.full_path = $2"
                                      : "SELECT f.id, f.name, f.full_path, f.object_id, f.user_id, f.automated_service_id, f.session_id, "
                                        "f.extension, f.directory_id, f.created_at, so.is_compressed, so.compression_algorithm, "
                                        "so.compression_level FROM storage.files f JOIN storage.objects so ON f.object_id = so.id "
                                        "WHERE f.user_id = $1 AND f.full_path = $2";

        drogon::orm::Result tp_res = co_await tsp_db_client->execSqlCoro(query, t_ctx.owner_id, t_ctx.actual_path);
        if (tp_res.empty())
            co_return std::nullopt;
        const drogon::orm::Row& row = tp_res[0];
        UserFileRecord rec;
        rec.id = row["id"].as<int64_t>();
        rec.name = row["name"].as<std::string>();
        rec.full_path = row["full_path"].as<std::string>();
        rec.t_object_id = row["object_id"].as<int64_t>();
        if (!row["user_id"].isNull())
            rec.user_id = row["user_id"].as<int32_t>();
        if (!row["automated_service_id"].isNull())
            rec.automated_service_id = row["automated_service_id"].as<int32_t>();
        rec.session_id = row["session_id"].as<int32_t>();
        rec.extension = row["extension"].as<std::string>();
        if (!row["directory_id"].isNull())
            rec.directory_id = row["directory_id"].as<int64_t>();
        rec.created_at = row["created_at"].as<std::string>();
        rec.is_compressed_ = row["is_compressed"].as<bool>();
        if (!row["compression_algorithm"].isNull())
            rec.compression_algorithm = row["compression_algorithm"].as<std::string>();
        if (!row["compression_level"].isNull())
            rec.compression_level = static_cast<uint8_t>(row["compression_level"].as<int32_t>());
        co_return rec;
    } catch (...) {
        co_return std::nullopt;
    }
}

Task<std::optional<std::string>> getObjectKey(drogon::orm::DbClientPtr tsp_db_client, int64_t t_object_id) {
    try {
        drogon::orm::Result tp_res = co_await tsp_db_client->execSqlCoro("SELECT key FROM storage.objects WHERE id = $1", t_object_id);
        if (tp_res.empty())
            co_return std::nullopt;
        co_return tp_res[0]["key"].as<std::string>();
    } catch (...) {
        co_return std::nullopt;
    }
}

Task<std::optional<Formats>> getFormat(drogon::orm::DbClientPtr tsp_db_client, std::string t_extension) {
    try {
        drogon::orm::Result tp_res = co_await tsp_db_client->execSqlCoro(
            "SELECT id, extension, mime_type, description FROM storage.formats WHERE extension = $1", t_extension);
        if (tp_res.empty())
            co_return std::nullopt;
        Formats fmt;
        fmt.setId(tp_res[0]["id"].as<int32_t>());
        fmt.setExtension(tp_res[0]["extension"].as<std::string>());
        fmt.setMimeType(tp_res[0]["mime_type"].as<std::string>());
        if (!tp_res[0]["description"].isNull()) {
            fmt.setDescription(tp_res[0]["description"].as<std::string>());
        }
        co_return fmt;
    } catch (...) {
        co_return std::nullopt;
    }
}

// ============================================================================
// Temp File Management
// ============================================================================

fs::path generateTempPath(std::string_view t_prefix) {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(100000, 999999);

    fs::path temp_dir = fs::temp_directory_path();
    std::string unique_name = std::format("{}{}_{}", t_prefix, std::chrono::system_clock::now().time_since_epoch().count(), dis(gen));
    return temp_dir / unique_name;
}

BackendResult<TempFileGuard> saveToTempFile(const drogon::HttpFile& t_file) {
    try {
        fs::path temp_path = generateTempPath("upload_");
        std::ofstream output(temp_path, std::ios::binary);
        if (!output) {
            return BackendResult<TempFileGuard>::Error(
                BackendError(BackendErrorKind::Filesystem, std::format("Failed to create temp file: {}", temp_path.string())));
        }
        output.write(t_file.fileData(), t_file.fileLength());
        output.close();
        return TempFileGuard(temp_path);
    } catch (const std::exception& ex) {
        return BackendResult<TempFileGuard>::Error(
            BackendError(BackendErrorKind::Filesystem, std::format("Failed to save temp file: {}", ex.what())));
    }
}

BackendResult<TempFileGuard> saveToTempFile(const std::string& t_data) {
    try {
        fs::path temp_path = generateTempPath("download_");
        std::ofstream output(temp_path, std::ios::binary);
        if (!output) {
            return BackendResult<TempFileGuard>::Error(
                BackendError(BackendErrorKind::Filesystem, std::format("Failed to create temp file: {}", temp_path.string())));
        }
        output.write(t_data.data(), t_data.size());
        output.close();
        return TempFileGuard(temp_path);
    } catch (const std::exception& ex) {
        return BackendResult<TempFileGuard>::Error(
            BackendError(BackendErrorKind::Filesystem, std::format("Failed to save temp file: {}", ex.what())));
    }
}

// ============================================================================
// Sub-coroutines
// ============================================================================

Task<BackendResult<UserFileRecord>> fetchFileRecord(drogon::orm::DbClientPtr tsp_db_client, const ScopeContext& t_ctx) {
    std::optional<UserFileRecord> opt = co_await findFileByPath(tsp_db_client, t_ctx);
    if (!opt.has_value()) {
        co_return BackendResult<UserFileRecord>::Error(BackendError(BackendErrorKind::Database, "File not found or access denied"));
    }
    co_return std::move(*opt);
}

Task<BackendResult<std::string>> resolveObjectKey(drogon::orm::DbClientPtr tsp_db_client, int64_t t_object_id) {
    std::optional<std::string> opt_key = co_await getObjectKey(tsp_db_client, t_object_id);
    if (!opt_key.has_value()) {
        co_return BackendResult<std::string>::Error(BackendError(BackendErrorKind::Database, "Object key not found in database"));
    }
    co_return std::move(opt_key.value());
}

Task<BackendResult<ScopeContext>> resolveScopeSession(
    drogon::orm::DbClientPtr tsp_db_client, const Json::Value& t_session, StorageScope t_scope, const std::string& t_path) {
    ScopeContext t_ctx;
    t_ctx.t_session_id = t_session["session_id"].asInt();
    t_ctx.actual_path = t_path;
    t_ctx.is_virtual_root = false;

    if (t_scope == StorageScope::Personal) {
        if (t_session["user"].isMember("automated_service_id")) {
            t_ctx.owner_id = t_session["user"]["automated_service_id"].asInt();
            t_ctx.is_automated_service = true;
            t_ctx.t_automated_service_id = t_ctx.owner_id;
            // M2M automated services default to Write-Only (no deletion allowed) for safety!
            t_ctx.can_read = true;
            t_ctx.can_write = true;
            t_ctx.can_delete = false;
        } else {
            t_ctx.owner_id = t_session["user"]["id"].asInt();
            t_ctx.is_user = true;
            t_ctx.user_id = t_ctx.owner_id;
            // Load personal workspace capability flags from the session JWT
            t_ctx.can_read = t_session["user"].isMember("can_read_personal") ? t_session["user"]["can_read_personal"].asBool() : true;
            t_ctx.can_write = t_session["user"].isMember("can_write_personal") ? t_session["user"]["can_write_personal"].asBool() : true;
            t_ctx.can_delete = t_session["user"].isMember("can_delete_personal") ? t_session["user"]["can_delete_personal"].asBool() : true;
        }
        co_return t_ctx;
    }

    if (t_scope == StorageScope::Domain) {
        if (t_path == "/" || t_path.empty() || t_path == "/domains" || t_path == "/domains/") {
            t_ctx.is_virtual_root = true;
            t_ctx.can_write = false;
            t_ctx.can_delete = false;
            co_return t_ctx;
        }
        // Resolve target domain name from VFS path: /domains/<domain_name>/users/<email>/...
        std::string path_no_slash = (t_path.front() == '/') ? t_path.substr(1) : t_path;
        std::string target_domain = "";

        if (path_no_slash.rfind("domains/", 0) == 0) {
            std::string rem = path_no_slash.substr(8);
            std::string::size_type next_slash = rem.find('/');
            if (next_slash == std::string::npos) {
                target_domain = rem;
                t_ctx.actual_path = "/";
            } else {
                target_domain = rem.substr(0, next_slash);
                t_ctx.actual_path = rem.substr(next_slash);
            }
        } else {
            std::string::size_type next_slash = path_no_slash.find('/');
            if (next_slash == std::string::npos) {
                target_domain = path_no_slash;
                t_ctx.actual_path = "/";
            } else {
                target_domain = path_no_slash.substr(0, next_slash);
                t_ctx.actual_path = path_no_slash.substr(next_slash);
            }
        }

        t_ctx.domain = target_domain;

        std::string actor_domain = t_session["user"].isMember("domain") ? t_session["user"]["domain"].asString() : "";
        std::string actor_role = t_session["user"]["role"]["name"].asString();

        // Zero-Trust verification: enforce operational boundaries
        if (actor_role != "global_admin" && actor_role != "admin") {
            if (actor_domain != target_domain) {
                co_return BackendResult<ScopeContext>::Error(
                    BackendError(BackendErrorKind::Auth, "Access Denied: Actor domain does not match target operational domain space."));
            }
        }

        if (t_session["user"].isMember("automated_service_id")) {
            t_ctx.owner_id = t_session["user"]["automated_service_id"].asInt();
            t_ctx.is_automated_service = true;
            t_ctx.t_automated_service_id = t_ctx.owner_id;
        } else {
            t_ctx.owner_id = t_session["user"]["id"].asInt();
            t_ctx.is_user = true;
            t_ctx.user_id = t_ctx.owner_id;
        }

        // Fetch Domain-specific capabilities for non-administrators
        if (actor_role != "global_admin" && actor_role != "admin") {
            auto perm_res = co_await tsp_db_client->execSqlCoro("SELECT allowed_subpath, can_read, can_write, can_delete "
                                                                "FROM core.user_domain_permissions WHERE user_id = $1 AND domain = $2",
                t_ctx.user_id.value(), target_domain);
            if (perm_res.empty()) {
                // Zero-Trust Default: access denied if no capability is registered
                co_return BackendResult<ScopeContext>::Error(
                    BackendError(BackendErrorKind::Auth, "Access Denied: No domain capability permissions configured for this user."));
            } else {
                t_ctx.allowed_subpath = perm_res[0]["allowed_subpath"].as<std::string>();
                t_ctx.can_read = perm_res[0]["can_read"].as<bool>();
                t_ctx.can_write = perm_res[0]["can_write"].as<bool>();
                t_ctx.can_delete = perm_res[0]["can_delete"].as<bool>();
            }

            // Sandbox boundary check: ensure traverse is constrained inside the allowed subpath
            if (t_ctx.allowed_subpath != "/") {
                std::string check_path = t_ctx.actual_path;
                if (check_path.front() != '/')
                    check_path = "/" + check_path;
                std::string rule_path = t_ctx.allowed_subpath;
                if (rule_path.front() != '/')
                    rule_path = "/" + rule_path;

                if (check_path.rfind(rule_path, 0) != 0) {
                    co_return BackendResult<ScopeContext>::Error(BackendError(
                        BackendErrorKind::Auth, "Access Denied: Path is outside your operational sandbox boundary (" + rule_path + ")."));
                }
            }
        }
        co_return t_ctx;
    }

    const std::string role = t_session["user"]["role"]["name"].asString();
    if (role != "admin") {
        co_return BackendResult<ScopeContext>::Error(BackendError(BackendErrorKind::Auth, "Admin access required for this scope"));
    }

    if (t_path == "/" || t_path.empty()) {
        t_ctx.is_virtual_root = true;
        t_ctx.can_write = false;
        t_ctx.can_delete = false;
        co_return t_ctx;
    }

    const std::string organisation = t_session["user"]["organisation"].asString();

    std::string path_no_slash = (t_path.front() == '/') ? t_path.substr(1) : t_path;
    std::string::size_type first_slash_pos = path_no_slash.find('/');

    std::string target_identifier;
    if (first_slash_pos == std::string::npos) {
        target_identifier = path_no_slash;
        t_ctx.actual_path = "/";
    } else {
        target_identifier = path_no_slash.substr(0, first_slash_pos);
        t_ctx.actual_path = path_no_slash.substr(first_slash_pos);
    }

    std::optional<drogon::orm::Result> tp_res;
    try {
        if (t_scope == StorageScope::AutomatedServices) {
            t_ctx.is_automated_service = true;
            tp_res = co_await tsp_db_client->execSqlCoro(
                "SELECT a.id AS owner_id, a.name AS display_name, s.id AS session_id FROM core.automated_services a "
                "LEFT JOIN core.sessions s ON s.automated_service_id = a.id AND s.terminated_at IS NULL "
                "WHERE a.token = $1 AND a.organisation = $2 ORDER BY s.created_at DESC NULLS LAST LIMIT 1",
                target_identifier, organisation);
            if (tp_res->empty()) {
                tp_res = co_await tsp_db_client->execSqlCoro(
                    "SELECT a.id AS owner_id, a.name AS display_name, s.id AS session_id FROM core.automated_services a "
                    "LEFT JOIN core.sessions s ON s.automated_service_id = a.id WHERE a.token = $1 AND a.organisation = $2 ORDER BY "
                    "s.created_at DESC NULLS LAST "
                    "LIMIT 1",
                    target_identifier, organisation);
            }
        } else if (t_scope == StorageScope::Users) {
            t_ctx.is_user = true;
            drogon::orm::Result res_tmp = co_await tsp_db_client->execSqlCoro(
                "SELECT u.id AS owner_id, s.id AS session_id FROM core.users u "
                "LEFT JOIN core.sessions s ON s.user_id = u.id AND s.terminated_at IS NULL "
                "WHERE u.email = $1 AND u.organisation = $2 ORDER BY s.created_at DESC NULLS LAST LIMIT 1",
                target_identifier, organisation);
            tp_res = std::move(res_tmp);
            if (tp_res->empty()) {
                res_tmp = co_await tsp_db_client->execSqlCoro("SELECT u.id AS owner_id, s.id AS session_id FROM core.users u "
                                                              "LEFT JOIN core.sessions s ON s.user_id = u.id WHERE u.email = $1 AND "
                                                              "u.organisation = $2 ORDER BY s.created_at DESC NULLS LAST LIMIT 1",
                    target_identifier, organisation);
                tp_res = std::move(res_tmp);
            }
        }

        if (!tp_res.has_value() || tp_res->empty()) {
            co_return BackendResult<ScopeContext>::Error(BackendError(BackendErrorKind::Database, "Target namespace not found"));
        }

        t_ctx.owner_id = (*tp_res)[0]["owner_id"].as<int32_t>();
        if (t_scope == StorageScope::AutomatedServices && !(*tp_res)[0]["display_name"].isNull()) {
            t_ctx.display_name = (*tp_res)[0]["display_name"].as<std::string>();
        }
        if (t_scope == StorageScope::AutomatedServices) {
            t_ctx.is_automated_service = true;
            t_ctx.t_automated_service_id = t_ctx.owner_id;
        } else {
            t_ctx.is_user = true;
            t_ctx.user_id = t_ctx.owner_id;
        }
        t_ctx.t_session_id = (*tp_res)[0]["session_id"].isNull() ? 0 : (*tp_res)[0]["session_id"].as<int32_t>();
        co_return t_ctx;
    } catch (const std::exception& e) {
        co_return BackendResult<ScopeContext>::Error(
            BackendError(BackendErrorKind::Database, std::format("Database error during scope resolution: {}", e.what())));
    }
}

} // namespace helpers

} // namespace sgrn::datastore::services::storage
