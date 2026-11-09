#include <sgrn/datastore/services/storage.hpp>

#include <sgrn/datastore/crypto/key-store.hpp>
#include <sgrn/datastore/plugins/aws/S3Client.hpp>
#include <sgrn/datastore/services/helpers/magic.hpp>
#include <sgrn/datastore/services/helpers/storage.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe-access.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/hashing.hpp>
#include <sgrn/utils/mime.hpp>
#include <sgrn/utils/strings.hpp>
#include <algorithm>
#include <cctype>
#include <format>
#include <orm/models/storage/Formats.h>
#include <orm/models/storage/Objects.h>

namespace sgrn::datastore::services::storage
{
namespace fs = std::filesystem;
using namespace drogon;
using namespace sgrn::datastore::plugins;
using namespace sgrn::datastore::core;

Task<BackendResult<void>> StorageService::deleteFile(drogon::orm::DbClientPtr tsp_db_client, int64_t t_file_id) {
    try {
        auto key_rows = co_await tsp_db_client->execSqlCoro("SELECT key_id, is_encrypted FROM storage.files WHERE id = $1", t_file_id);
        if (!key_rows.empty() && key_rows[0]["is_encrypted"].as<bool>() && !key_rows[0]["key_id"].isNull()) {
            std::string kid = key_rows[0]["key_id"].as<std::string>();
            auto ks = sgrn::datastore::crypto::getKeyStore();
            if (ks) {
                (void)ks->shred(kid);
            }
        }
        co_await tsp_db_client->execSqlCoro("DELETE FROM storage.files WHERE id = $1", t_file_id);
        co_return {};
    } catch (const std::exception& ex) {
        co_return BackendError{"Database", ex.what()};
    }
}

Task<HttpResponsePtr> StorageService::handleUploadFilesBatchRequest(
    Json::Value t_session, std::string t_scope, std::string t_base_path, const std::vector<drogon::HttpFile>& t_files) {
    Json::Value results(Json::arrayValue);
    int success_count = 0;
    int fail_count = 0;

    for (const auto& t_file : t_files) {
        std::string decoded_name = drogon::utils::urlDecode(t_file.getFileName());
        auto sanitized_opt = sgrn::utils::strings::sanitizeRelativeFilename(decoded_name);
        if (!sanitized_opt.has_value()) {
            fail_count++;
            Json::Value err_res;
            err_res["success"] = false;
            err_res["error"] = "Invalid filename: path traversal detected";
            results.append(std::move(err_res));
            continue;
        }
        decoded_name = sanitized_opt.value();

        std::string target_path = t_base_path;
        if (target_path.back() != '/' && decoded_name.front() != '/')
            target_path += "/";
        target_path += decoded_name;

        HttpResponsePtr r = co_await handleUploadFileRequest(t_session, t_scope, target_path, t_file);
        if (r->getStatusCode() >= 200 && r->getStatusCode() < 300) {
            success_count++;
        } else {
            fail_count++;
        }
        results.append(r->getJsonObject() ? *r->getJsonObject() : Json::Value(Json::objectValue));
    }

    Json::Value response(Json::objectValue);
    response["success_count"] = success_count;
    response["fail_count"] = fail_count;
    response["results"] = std::move(results);

    co_return HttpResponse::newHttpJsonResponse(std::move(response));
}

// ============================================================================
// Internal Private Methods (Orchestration)
// ============================================================================

Task<BackendResult<Json::Value>> StorageService::uploadFile(UploadContext t_context, drogon::HttpFile t_file) {
    try {
        BackendResult<drogon::orm::DbClientPtr> db_res = getDbClient();
        CO_RETURN_ERROR_IF(db_res.hasError(), db_res.error());
        drogon::orm::DbClientPtr sp_db_client = db_res.value();

        BackendResult<bool> validation = co_await validateUpload(t_context, t_file.fileLength());
        CO_RETURN_ERROR_IF(validation.hasError(), validation.error());

        // Formats-registry veto (admin-toggleable per extension).
        BackendResult<void> format_ok = co_await enforceFormatAllowed(sp_db_client, t_context.original_filename);
        CO_RETURN_ERROR_IF(format_ok.hasError(), format_ok.error());

        std::string t_mime_type = helpers::inferMimeType(t_file.getFileName());
        const StorageConfig live_cfg = currentStorageConfig();
        UploadThresholds t_thresholds{.compress_ram_bytes = live_cfg.thresholdCompressRamBytes(),
            .compression_size_threshold = live_cfg.compression_size_threshold_kb * 1024};

        FileHash t_original_hash;
        bool process_in_memory = t_thresholds.shouldProcessInMemory(t_file.fileLength());
        std::string file_data_copy;

        if (process_in_memory) {
            file_data_copy = std::string(t_file.fileData(), t_file.fileLength());
            BackendResult<FileHash> hash = helpers::computeHashInMemory(file_data_copy);
            CO_RETURN_ERROR_IF(hash.hasError(), hash.error());
            t_original_hash = std::move(hash.value());
        } else {
            BackendResult<TempFileGuard> temp_result = helpers::saveToTempFile(t_file);
            CO_RETURN_ERROR_IF(temp_result.hasError(), temp_result.error());
            TempFileGuard temp_guard = std::move(*temp_result);
            BackendResult<FileHash> r = co_await helpers::computeHashFromFileAsync(temp_guard.path);
            CO_RETURN_ERROR_IF(r.hasError(), r.error());
            t_original_hash = std::move(r.value());
        }

        drogon::orm::DbClientPtr sp_transaction = co_await sp_db_client->newTransactionCoro();
        BackendResult<void> upload_res;

        if (process_in_memory) {
            upload_res = co_await executeInMemoryUpload(sp_transaction, t_context, t_file, t_mime_type, t_thresholds,
                live_cfg.compression_level, std::move(file_data_copy), t_original_hash);
        } else {
            upload_res = co_await executeStreamingUpload(
                sp_transaction, t_context, t_file, t_mime_type, t_thresholds, live_cfg.compression_level, t_original_hash);
        }
        CO_RETURN_ERROR_IF(upload_res.hasError(), upload_res.error()); // transaction destructs here → rollback
        BackendResult<void> finalize_res = co_await finalizeUpload(sp_transaction, t_context);
        CO_RETURN_ERROR_IF(finalize_res.hasError(), finalize_res.error());
        Json::Value response;
        response["success"] = true;
        if (t_context.file_id.has_value())
            response["file_id"] = Json::Int64(*t_context.file_id);
        if (t_context.object_id.has_value())
            response["object_id"] = Json::Int64(*t_context.object_id);
        response["name"] = t_context.original_filename;
        response["key"] = t_context.identity.hash.key;
        // Debuggability: which S3 path the bytes took, and both sizes — the
        // threshold decision uses the original size, the wire used final.
        response["upload_mode"] = t_context.upload_mode;
        response["part_count"] =
            t_context.part_count.has_value() ? Json::Value(Json::Int64(*t_context.part_count)) : Json::Value(Json::nullValue);
        response["part_size_bytes"] =
            t_context.part_size_bytes.has_value() ? Json::Value(Json::Int64(*t_context.part_size_bytes)) : Json::Value(Json::nullValue);
        response["original_size_bytes"] = Json::UInt64(t_context.identity.original_size);
        response["final_size_bytes"] = Json::UInt64(t_context.identity.final_size);
        co_return BackendResult<Json::Value>(std::move(response));
    } catch (const std::exception& ex) {
        co_return BackendError{"Runtime", std::format("Upload failed: {}", ex.what())};
    }
}

Task<BackendResult<std::string>> StorageService::downloadFile(std::string t_bucket, std::string t_key) {
    BackendResult<plugins::aws::S3Client*> client_res = S3Client();
    CO_RETURN_ERROR_IF(client_res.hasError(), client_res.error());
    co_return co_await client_res.value()->getObjectContent(std::move(t_bucket), std::move(t_key));
}

Task<bool> StorageService::objectExists(std::string t_bucket, std::string t_key) {
    BackendResult<drogon::orm::DbClientPtr> db_res = getDbClient();
    if (db_res.hasError()) {
        co_return false;
    }
    drogon::orm::Result res =
        co_await db_res.value()->execSqlCoro("SELECT 1 FROM storage.objects WHERE bucket = $1 AND key = $2", t_bucket, t_key);
    co_return !res.empty();
}

BackendResult<bool> StorageService::validateFileSize(size_t t_size) {
    if (t_size > currentStorageConfig().maxFileSizeBytes())
        return BackendError{"Filesystem", "File too large"};
    return true;
}

BackendResult<bool> StorageService::validateExtension(std::string_view t_filename) {
    // SEC: Use lowercase comparison to prevent bypass via ".PHP", ".JSP", etc.
    if (!currentStorageConfig().isExtensionAllowed(helpers::extractExtensionLower(t_filename)))
        return BackendError{"Filesystem", "Extension not allowed"};
    return true;
}

Task<BackendResult<bool>> StorageService::validateUpload(const UploadContext& t_context, size_t t_size) {
    BackendResult<bool> r1 = validateFileSize(t_size);
    if (r1.hasError()) {
        co_return r1;
    }
    BackendResult<bool> r2 = validateExtension(t_context.original_filename);
    if (r2.hasError()) {
        co_return r2;
    }
    co_return true;
}

Task<BackendResult<void>> StorageService::enforceFormatAllowed(drogon::orm::DbClientPtr tsp_db_client, std::string_view t_filename) {
    try {
        const std::string ext = helpers::extractExtensionLower(t_filename);
        if (ext.empty()) {
            co_return {};
        }
        drogon::orm::Result res = co_await tsp_db_client->execSqlCoro("SELECT is_allowed FROM storage.formats WHERE extension = $1", ext);
        if (!res.empty() && !res[0]["is_allowed"].isNull() && res[0]["is_allowed"].as<std::string>() != "t") {
            co_return BackendError{"Filesystem", "Extension not allowed"};
        }
        co_return {};
    } catch (const std::exception& ex) {
        co_return BackendError{"Database", std::format("Format check failed: {}", ex.what())};
    }
}

Task<BackendResult<std::pair<FileIdentity, std::string>>> StorageService::processInMemory(const drogon::HttpFile& t_http_file,
    std::string t_mime_type, const UploadThresholds& t_thresholds, uint8_t t_compression_level, std::string t_file_data,
    FileHash t_original_hash) {

    FileIdentity identity;
    identity.hash = std::move(t_original_hash);

    auto [base_ext, comp_ext] = helpers::splitExtension(t_http_file.getFileName());
    identity.extension = std::string(base_ext);
    identity.mime_type = std::move(t_mime_type);
    identity.original_size = t_file_data.size();

    if (!comp_ext.empty()) {
        identity.is_compressed = true;
        identity.compression_algorithm = (comp_ext == "zst") ? "zstd" : std::string(comp_ext);
        identity.final_size = t_file_data.size();
    }

    if (!identity.is_compressed && t_thresholds.shouldCompress(t_file_data.size(), helpers::isCompressibleMimeType(identity.mime_type))) {
        BackendResult<std::string> comp_res = co_await helpers::compressInMemory(std::move(t_file_data), t_compression_level);
        if (comp_res.has_value()) {
            identity.is_compressed = true;
            identity.final_size = comp_res->size();
            identity.compression_algorithm = "zstd";
            identity.compression_level = t_compression_level;
            co_return std::make_pair(std::move(identity), std::move(*comp_res));
        }
    }

    if (!identity.is_compressed) {
        identity.is_compressed = false;
        identity.compression_algorithm = std::nullopt;
        identity.compression_level = std::nullopt;
    }
    identity.final_size = t_file_data.size();
    co_return std::make_pair(std::move(identity), std::move(t_file_data));
}

Task<BackendResult<std::pair<FileIdentity, std::filesystem::path>>> StorageService::processStreaming(const drogon::HttpFile& t_http_file,
    std::string t_mime_type, const UploadThresholds& t_thresholds, uint8_t t_compression_level, FileHash t_original_hash) {

    BackendResult<TempFileGuard> temp_res = helpers::saveToTempFile(t_http_file);
    CO_RETURN_ERROR_IF(temp_res.hasError(), temp_res.error());
    TempFileGuard input_guard = std::move(temp_res.value());

    FileIdentity identity;
    identity.hash = std::move(t_original_hash);

    auto [base_ext, comp_ext] = helpers::splitExtension(t_http_file.getFileName());
    identity.extension = std::string(base_ext);
    identity.mime_type = std::move(t_mime_type);
    identity.original_size = fs::file_size(input_guard.path);

    if (!comp_ext.empty()) {
        identity.is_compressed = true;
        identity.compression_algorithm = (comp_ext == "zst") ? "zstd" : std::string(comp_ext);
        identity.final_size = identity.original_size;
    }

    if (!identity.is_compressed &&
        t_thresholds.shouldCompress(identity.original_size, helpers::isCompressibleMimeType(identity.mime_type))) {
        fs::path out_path = helpers::generateTempPath("comp_");
        BackendResult<fs::path> comp_res = co_await helpers::compressFileAsync(input_guard.path, out_path, t_compression_level);
        if (comp_res.has_value()) {
            identity.is_compressed = true;
            identity.final_size = fs::file_size(out_path);
            identity.compression_algorithm = "zstd";
            identity.compression_level = t_compression_level;
            co_return std::make_pair(std::move(identity), std::move(out_path));
        }
    }

    if (!identity.is_compressed) {
        identity.is_compressed = false;
        identity.compression_algorithm = std::nullopt;
        identity.compression_level = std::nullopt;
    }
    identity.final_size = identity.original_size;
    fs::path p = std::move(input_guard.path);
    input_guard.path.clear();
    co_return std::make_pair(std::move(identity), std::move(p));
}

Task<BackendResult<void>> StorageService::executeInMemoryUpload(drogon::orm::DbClientPtr tsp_transaction, UploadContext& t_context,
    const drogon::HttpFile& t_file, std::string t_mime_type, UploadThresholds t_thresholds, uint8_t t_compression_level,
    std::string t_file_data, FileHash t_original_hash) {

    BackendResult<std::pair<FileIdentity, std::string>> proc_res = co_await processInMemory(
        t_file, std::move(t_mime_type), t_thresholds, t_compression_level, std::move(t_file_data), std::move(t_original_hash));
    CO_RETURN_ERROR_IF(proc_res.hasError(), proc_res.error());

    t_context.identity = std::move(proc_res->first);
    std::string final_data = std::move(proc_res->second);

    BackendResult<std::string> sha256_res = helpers::computeSha256InMemory(final_data);
    CO_RETURN_ERROR_IF(sha256_res.hasError(), std::move(sha256_res).error());
    std::string sha256 = *sha256_res;

    BackendResult<int64_t> obj_res = co_await helpers::insertObject(tsp_transaction, t_context.bucket, t_context.identity.hash.key,
        t_context.identity.final_size, t_context.identity.original_size, std::move(sha256), t_context.identity.is_compressed,
        t_context.identity.compression_algorithm, t_context.identity.compression_level);
    CO_RETURN_ERROR_IF(obj_res.hasError(), obj_res.error());
    t_context.object_id = *obj_res;

    BackendResult<sgrn::datastore::plugins::aws::S3Client*> s3_res = S3Client();
    CO_RETURN_ERROR_IF(s3_res.hasError(), s3_res.error());

    // Same threshold as the streaming path: when the admin lowers it to at or
    // below the RAM threshold (e.g. to debug multipart with small files),
    // in-memory uploads chunk too instead of silently staying single-PUT.
    // Spill to a temp file first — the bytes are already fully in RAM here,
    // so this adds no meaningful memory cost.
    // NOTE: the threshold compares ORIGINAL (pre-compression) sizes — the
    // number the admin sees — not wire bytes. Same basis as max_file_size.
    const StorageConfig live_cfg = currentStorageConfig();
    if (t_context.identity.original_size >= live_cfg.chunkingThresholdBytes()) {
        BackendResult<TempFileGuard> spill_res = helpers::saveToTempFile(final_data);
        CO_RETURN_ERROR_IF(spill_res.hasError(), spill_res.error());
        TempFileGuard spill_guard = std::move(*spill_res);
        t_context.upload_mode = "multipart";
        t_context.part_size_bytes = static_cast<int64_t>(live_cfg.chunkPartSizeBytes());
        BackendResult<int> mp_res = co_await s3_res.value()->uploadFileMultipart(
            t_context.bucket, t_context.identity.hash.key, t_context.identity.mime_type, spill_guard.path, live_cfg.chunkPartSizeBytes());
        CO_RETURN_ERROR_IF(mp_res.hasError(), std::move(mp_res).error());
        t_context.part_count = static_cast<int64_t>(*mp_res);
        co_return co_await helpers::updateObjectChunking(
            tsp_transaction, t_context.object_id.value(), t_context.upload_mode, t_context.part_count, t_context.part_size_bytes);
    }
    t_context.upload_mode = "single";
    co_return co_await s3_res.value()->uploadFromMemory(
        t_context.bucket, t_context.identity.hash.key, std::move(final_data), t_context.identity.mime_type);
}

Task<BackendResult<void>> StorageService::executeStreamingUpload(drogon::orm::DbClientPtr tsp_transaction, UploadContext& t_context,
    const drogon::HttpFile& t_file, std::string t_mime_type, UploadThresholds t_thresholds, uint8_t t_compression_level,
    FileHash t_original_hash) {

    BackendResult<std::pair<FileIdentity, fs::path>> proc_res =
        co_await processStreaming(t_file, std::move(t_mime_type), t_thresholds, t_compression_level, std::move(t_original_hash));
    CO_RETURN_ERROR_IF(proc_res.hasError(), std::move(proc_res).error());

    t_context.identity = std::move(proc_res->first);
    fs::path upload_path = std::move(proc_res->second);
    TempFileGuard guard(upload_path);

    BackendResult<std::string> sha256_res = helpers::computeSha256FromFile(guard.path);
    CO_RETURN_ERROR_IF(sha256_res.hasError(), std::move(sha256_res).error());
    std::string sha256 = *sha256_res;

    BackendResult<int64_t> obj_res = co_await helpers::insertObject(tsp_transaction, t_context.bucket, t_context.identity.hash.key,
        t_context.identity.final_size, t_context.identity.original_size, std::move(sha256), t_context.identity.is_compressed,
        t_context.identity.compression_algorithm, t_context.identity.compression_level);
    CO_RETURN_ERROR_IF(obj_res.hasError(), std::move(obj_res).error());
    t_context.object_id = *obj_res;

    BackendResult<plugins::aws::S3Client*> s3_res = S3Client();
    CO_RETURN_ERROR_IF(s3_res.hasError(), std::move(s3_res).error());

    // Large files go out as real multipart uploads with
    // bounded memory (~one part in RAM); everything else keeps single PUT.
    // NOTE: compared against the ORIGINAL size (what the admin uploaded),
    // not the post-compression wire size — same basis as max_file_size, so
    // the threshold is predictable regardless of compression ratio.
    // Snapshot fresh here (not held across the awaits above) so a dashboard
    // config save mid-upload takes effect on the next upload, never mid-way.
    const StorageConfig live_cfg = currentStorageConfig();
    if (t_context.identity.original_size >= live_cfg.chunkingThresholdBytes()) {
        t_context.upload_mode = "multipart";
        t_context.part_size_bytes = static_cast<int64_t>(live_cfg.chunkPartSizeBytes());
        BackendResult<int> mp_res = co_await s3_res.value()->uploadFileMultipart(
            t_context.bucket, t_context.identity.hash.key, t_context.identity.mime_type, upload_path, live_cfg.chunkPartSizeBytes());
        CO_RETURN_ERROR_IF(mp_res.hasError(), std::move(mp_res).error());
        t_context.part_count = static_cast<int64_t>(*mp_res);
        co_return co_await helpers::updateObjectChunking(
            tsp_transaction, t_context.object_id.value(), t_context.upload_mode, t_context.part_count, t_context.part_size_bytes);
    }
    t_context.upload_mode = "single";
    co_return co_await s3_res.value()->uploadFile(t_context.bucket, t_context.identity.hash.key, upload_path.string());
}

Task<BackendResult<void>> StorageService::finalizeUpload(drogon::orm::DbClientPtr tsp_transaction, UploadContext& t_context) {
    BackendResult<std::optional<int64_t>> dir_res = co_await helpers::resolveDirectoryPath(
        tsp_transaction, t_context.user_id, t_context.automated_service_id, t_context.session_id, t_context.virtual_path, t_context.domain);
    CO_RETURN_ERROR_IF(dir_res.hasError(), std::move(dir_res).error());
    t_context.directory_id = *dir_res;

    // -- Content sniffing: read first 4096 bytes from S3 for magic detection.
    // Skip when the server applied transparent compression: the object on S3
    // carries zstd magic bytes regardless of the declared extension (e.g. json),
    // which is correct and expected — sniffing would produce a false positive.
    std::string declared_ext = t_context.identity.storageExtension();
    if (!t_context.identity.is_compressed) {
        BackendResult<plugins::aws::S3Client*> s3_res = S3Client();
        if (s3_res.has_value()) {
            auto* s3 = s3_res.value();
            BackendResult<std::string> content_res = co_await s3->getObjectContent(t_context.bucket, t_context.identity.hash.key, 4096);
            if (content_res.has_value()) {
                std::string_view data(*content_res);
                std::string preview(data.substr(0, std::min<size_t>(data.size(), 4096)));
                auto sniff_res = ::sgrn::datastore::services::helpers::sniffExtension(preview);
                if (sniff_res.has_value()) {
                    std::string sniffed_ext = *sniff_res;
                    // Normalize both to lowercase for comparison
                    std::string declared_lower = declared_ext;
                    std::transform(declared_lower.begin(), declared_lower.end(), declared_lower.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    std::string sniffed_lower = sniffed_ext;
                    std::transform(sniffed_lower.begin(), sniffed_lower.end(), sniffed_lower.begin(),
                        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    if (!::sgrn::datastore::services::helpers::isCompatibleSniffedFormat(declared_lower, sniffed_lower)) {
                        // Check if sniffed extension is a known format in registry
                        auto format_res = co_await helpers::getFormat(tsp_transaction, sniffed_lower);
                        if (format_res.has_value()) {
                            // Sniffed type is a known format but differs from declared → reject
                            co_return BackendError{BackendErrorKind::Runtime,
                                fmt::format("Content type mismatch: file content appears to be '{}' but was declared as '{}'",
                                    sniffed_lower, declared_lower)};
                        }
                        // Sniffed type unknown → fall back to declared extension (registry will handle)
                    }
                }
            }
        }
    }

    BackendResult<int64_t> file_res = co_await helpers::insertFile(tsp_transaction, t_context.original_filename, *t_context.object_id,
        t_context.user_id, t_context.automated_service_id, t_context.session_id, declared_ext, t_context.directory_id, t_context.domain);
    CO_RETURN_ERROR_IF(file_res.hasError(), std::move(file_res).error());
    t_context.file_id = *file_res;
    co_return {};
}

} // namespace sgrn::datastore::services::storage
