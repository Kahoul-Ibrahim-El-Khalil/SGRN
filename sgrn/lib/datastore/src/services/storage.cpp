// src/sgrn/services/storage/service.cpp
#include <sgrn/datastore/services/helpers/storage.hpp>
#include <sgrn/datastore/services/storage.hpp>

#include <drogon/HttpAppFramework.h>
#include <drogon/orm/DbClient.h>
#include <sgrn/datastore/plugins/aws/S3Client.hpp>
#include <sgrn/datastore/plugins/threadpool/Threadpool.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe_access.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/hashing.hpp>
#include <sgrn/utils/mime.hpp>
#include <orm/models/storage/Formats.h>
#include <orm/models/storage/Objects.h>

#ifdef DEBUG_STORAGE_SERVICE
#define DEBUG_LOG(msg, ...) SGRN_DEBUG("StorageService", msg __VA_OPT__(, ) __VA_ARGS__)
#define INFO_LOG(msg, ...) SGRN_INFO("StorageService", msg __VA_OPT__(, ) __VA_ARGS__)
#define WARN_LOG(msg, ...) SGRN_WARN("StorageService", msg __VA_OPT__(, ) __VA_ARGS__)
#define ERROR_LOG(msg, ...) SGRN_ERROR("StorageService", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define DEBUG_LOG(...) ((void)0)
#define INFO_LOG(...) ((void)0)
#define WARN_LOG(...) ((void)0)
#define ERROR_LOG(...) ((void)0)
#endif

namespace sgrn::datastore::services::storage
{
namespace fs = std::filesystem;
using namespace drogon;
using namespace sgrn::datastore::plugins;
using namespace sgrn::datastore::core;

// ============================================================================
// StorageConfig Implementation
// ============================================================================

StorageConfig StorageConfig::loadFromConfig() {
    StorageConfig cfg;
    try {
        const Json::Value& custom_config = drogon::app().getCustomConfig();
        if (custom_config.isMember("s3")) {
            const Json::Value& storage_cfg = custom_config["s3"];
            if (storage_cfg.isMember("threshold_compress_ram_mb")) {
                cfg.threshold_compress_ram_mb = storage_cfg["threshold_compress_ram_mb"].asUInt64();
            }
            if (storage_cfg.isMember("max_file_size_mb")) {
                cfg.max_file_size_mb = storage_cfg["max_file_size_mb"].asUInt64();
            }
            if (storage_cfg.isMember("compression_level")) {
                cfg.compression_level = static_cast<uint8_t>(storage_cfg["compression_level"].asUInt());
            }
            if (storage_cfg.isMember("compression_size_threshold_kb")) {
                cfg.compression_size_threshold_kb = storage_cfg["compression_size_threshold_kb"].asUInt64();
            }
            if (storage_cfg.isMember("allowed_extensions") && storage_cfg["allowed_extensions"].isArray()) {
                cfg.allowed_extensions.clear();
                for (const auto& ext : storage_cfg["allowed_extensions"]) {
                    cfg.allowed_extensions.insert(ext.asString());
                }
            }
            if (storage_cfg.isMember("prohibited_extensions") && storage_cfg["prohibited_extensions"].isArray()) {
                cfg.prohibited_extensions.clear();
                for (const auto& ext : storage_cfg["prohibited_extensions"]) {
                    cfg.prohibited_extensions.insert(ext.asString());
                }
            }
            INFO_LOG("Loaded storage config: compress_ram={}MB, max_size={}MB, compression_level={}, allowed_exts={}",
                cfg.threshold_compress_ram_mb_, cfg.max_file_size_mb_, static_cast<int>(cfg.compression_level_),
                cfg.allowed_extensions.size());
        }
    } catch (const std::exception& ex) {
        WARN_LOG("Failed to load config, using defaults: {}", ex.what());
    }
    return cfg;
}

// ============================================================================
// StorageService Implementation
// ============================================================================

StorageService::StorageService() {
    config_ = StorageConfig::loadFromConfig();
    Json::Value config_app = drogon::app().getCustomConfig();
    if (config_app.isMember("s3") && config_app["s3"].isMember("default_bucket")) {
        default_bucket_ = config_app["s3"]["default_bucket"].asString();
    } else {
        default_bucket_ = std::string("sgrn-uploads");
    }
}

BackendResult<plugins::aws::S3Client*> StorageService::S3Client() const {
    return getPlugin<plugins::aws::S3Client>();
}

BackendResult<plugins::PostgrestClient*> StorageService::PostgrestClient() const {
    return getPlugin<plugins::PostgrestClient>();
}

BackendResult<drogon::orm::DbClientPtr> StorageService::getDbClient() const {
    return drogon::app().getDbClient();
}

const StorageConfig& StorageService::getConfig() {
    return config_;
}

StorageScope StorageService::parseScope(const std::string& t_scope_str) {
    if (t_scope_str == "automated-services" || t_scope_str == "automated_services")
        return StorageScope::AutomatedServices;
    if (t_scope_str == "users")
        return StorageScope::Users;
    if (t_scope_str == "domain" || t_scope_str == "domains")
        return StorageScope::Domain;
    return StorageScope::Personal;
}

Task<BackendResult<ScopeContext>> StorageService::resolveScopeSession(
    const Json::Value& t_session, StorageScope t_scope, const std::string& t_path) {
    BackendResult<drogon::orm::DbClientPtr> db_res = getDbClient();
    if (!db_res.has_value())
        co_return db_res.error();
    co_return co_await helpers::resolveScopeSession(db_res.value(), t_session, t_scope, t_path);
}

// ============================================================================
// High-Level Handlers
// ============================================================================

Task<HttpResponsePtr> StorageService::handleDownloadFileRequest(Json::Value t_session, std::string t_scope, std::string t_file_path) {
    BackendResult<drogon::orm::DbClientPtr> db_res = getDbClient();
    if (!db_res.has_value())
        co_return sgrn::createJsonResponse(std::move(db_res));
    drogon::orm::DbClientPtr sp_db_client = db_res.value();

    StorageScope parsed_scope = parseScope(t_scope);
    BackendResult<ScopeContext> scope_res = co_await resolveScopeSession(t_session, parsed_scope, t_file_path);
    if (scope_res.hasError()) {
        co_return sgrn::createJsonResponse(std::move(scope_res));
    }

    if (!scope_res->can_read) {
        co_return createJsonErrorResponse("Download Denied: Read privileges missing for this scope space.", drogon::k403Forbidden);
    }

    if (scope_res->is_virtual_root) {
        co_return createJsonErrorResponse("Cannot download a directory", drogon::k400BadRequest);
    }

    BackendResult<UserFileRecord> record_res = co_await helpers::fetchFileRecord(sp_db_client, *scope_res);
    if (record_res.hasError()) {
        co_return sgrn::createJsonResponse(std::move(record_res));
    }

    BackendResult<std::string> key_res = co_await helpers::resolveObjectKey(sp_db_client, record_res->t_object_id);
    if (key_res.hasError()) {
        co_return sgrn::createJsonResponse(std::move(key_res));
    }

    BackendResult<std::string> data_res = co_await downloadFile(default_bucket_, key_res.value());
    if (data_res.hasError()) {
        co_return sgrn::createJsonResponse(std::move(data_res));
    }

    std::string download_name = record_res->name;
    if (record_res->is_compressed_) {
        std::string suffix = (record_res->compression_algorithm == "zstd") ? ".zst" : "";
        if (!suffix.empty() && download_name.find(suffix) == std::string::npos) {
            download_name += suffix;
        }
    }

    // SEC: Strip CR, LF, NUL, and double-quote to prevent HTTP header injection
    download_name.erase(std::remove_if(download_name.begin(), download_name.end(),
                            [](char t_c) { return t_c == '\r' || t_c == '\n' || t_c == '\0' || t_c == '"'; }),
        download_name.end());

    HttpResponsePtr sp_resp = HttpResponse::newHttpResponse();
    sp_resp->setStatusCode(k200OK);
    sp_resp->setContentTypeString(record_res->is_compressed_ ? "application/zstd" : helpers::inferMimeType(record_res->name));
    sp_resp->addHeader("Content-Disposition", fmt::format("attachment; filename=\"{}\"", download_name));
    sp_resp->setBody(std::move(data_res.value()));
    co_return sp_resp;
}

Task<HttpResponsePtr> StorageService::handleUploadFileRequest(
    Json::Value t_session, std::string t_scope, std::string t_file_path, const drogon::HttpFile& t_http_file) {
    StorageScope parsed_scope = parseScope(t_scope);
    BackendResult<ScopeContext> scope_res = co_await resolveScopeSession(t_session, parsed_scope, t_file_path);
    if (scope_res.hasError()) {
        co_return sgrn::createJsonResponse(scope_res);
    }

    if (!scope_res->can_write) {
        co_return createJsonErrorResponse("Upload Denied: Write privileges missing for this scope space.", drogon::k403Forbidden);
    }

    if (scope_res->t_session_id <= 0 || scope_res->owner_id <= 0) {
        co_return createJsonErrorResponse("Upload session is missing or invalid", drogon::k401Unauthorized);
    }

    std::string decoded_name = drogon::utils::urlDecode(t_http_file.getFileName());
    std::string base_name = std::filesystem::path(decoded_name).filename().string();

    UploadContext t_context{.original_filename = base_name,
        .virtual_path = std::move(scope_res->actual_path),
        .session_id = scope_res->t_session_id,
        .domain = scope_res->domain,
        .user_id = scope_res->is_automated_service ? std::nullopt : std::optional<int32_t>(scope_res->owner_id),
        .automated_service_id = scope_res->is_automated_service ? std::optional<int32_t>(scope_res->owner_id) : std::nullopt,
        .organisation = t_session["user"].isMember("organisation") ? t_session["user"]["organisation"].asString() : "",
        .bucket = default_bucket_};

    BackendResult<Json::Value> r = co_await uploadFile(std::move(t_context), t_http_file);
    if (r.hasError()) {
        co_return sgrn::createJsonResponse(r);
    }
    co_return HttpResponse::newHttpJsonResponse(std::move(r.value()));
}

Task<HttpResponsePtr> StorageService::handleGetConstraints() {
    const auto& cfg = getConfig();
    Json::Value constraints;
    constraints["max_file_size_mb"] = Json::Value::UInt64(cfg.max_file_size_mb);
    constraints["max_file_size_bytes"] = Json::Value::UInt64(config_.maxFileSizeBytes());
    co_return HttpResponse::newHttpJsonResponse(std::move(constraints));
}

Task<HttpResponsePtr> StorageService::handleCreateDirectoryRequest(Json::Value t_session, std::string t_scope, std::string t_path) {
    BackendResult<drogon::orm::DbClientPtr> db_res = getDbClient();
    if (db_res.hasError()) {
        co_return sgrn::createJsonResponse(db_res);
    }
    drogon::orm::DbClientPtr sp_db_client = db_res.value();

    StorageScope parsed_scope = parseScope(t_scope);
    BackendResult<ScopeContext> scope_res = co_await resolveScopeSession(t_session, parsed_scope, t_path);
    if (scope_res.hasError()) {
        co_return sgrn::createJsonResponse(scope_res);
    }

    if (!scope_res->can_write) {
        co_return createJsonErrorResponse(
            "Directory Creation Denied: Write privileges missing for this scope space.", drogon::k403Forbidden);
    }

    BackendResult<std::optional<int64_t>> r = co_await helpers::ensureDirectoryPath(sp_db_client, scope_res->user_id,
        scope_res->t_automated_service_id, scope_res->t_session_id, scope_res->actual_path, scope_res.value().domain);
    if (r.hasError()) {
        co_return sgrn::createJsonResponse(r);
    }

    Json::Value sp_resp;
    sp_resp["success"] = true;
    sp_resp["directory_id"] = r->has_value() ? Json::Int64(**r) : Json::Value::null;
    co_return HttpResponse::newHttpJsonResponse(std::move(sp_resp));
}

Task<HttpResponsePtr> StorageService::handleCreateObject(const Json::Value& t_json) {
    // This is a lower-level API for creating objects directly in the DB
    BackendResult<drogon::orm::DbClientPtr> db_res = getDbClient();
    if (db_res.hasError()) {
        co_return sgrn::createJsonResponse(db_res);
    }
    drogon::orm::DbClientPtr sp_db_client = db_res.value();

    BackendResult<int64_t> r = co_await helpers::insertObject(sp_db_client, t_json["bucket"].asString(), t_json["key"].asString(),
        t_json["size"].asInt64(), t_json["original_size"].asInt64(), t_json["is_compressed"].asBool(),
        t_json.isMember("compression_algorithm") ? std::optional<std::string>(t_json["compression_algorithm"].asString()) : std::nullopt);

    if (!r.has_value())
        co_return sgrn::createJsonResponse(r);
    Json::Value sp_resp;
    sp_resp["success"] = true;
    sp_resp["id"] = Json::Int64(*r);
    co_return HttpResponse::newHttpJsonResponse(std::move(sp_resp));
}

Task<HttpResponsePtr> StorageService::handleListObjects(int32_t t_automated_service_id) {
    BackendResult<drogon::orm::DbClientPtr> db_res = getDbClient();
    if (!db_res.has_value())
        co_return sgrn::createJsonResponse(db_res);
    drogon::orm::DbClientPtr sp_db_client = db_res.value();

    try {
        auto result = co_await sp_db_client->execSqlCoro("SELECT f.id, f.name, f.full_path, f.extension, f.created_at, "
                                                         "       so.size AS compressed_size, so.original_size, "
                                                         "       so.is_compressed, so.compression_algorithm "
                                                         "FROM storage.files f "
                                                         "JOIN storage.objects so ON so.id = f.object_id "
                                                         "WHERE f.automated_service_id = $1 "
                                                         "ORDER BY f.created_at DESC",
            t_automated_service_id);

        Json::Value arr(Json::arrayValue);
        for (const auto& row : result) {
            Json::Value item;
            item["id"] = Json::Int64(row["id"].as<int64_t>());
            item["name"] = row["name"].as<std::string>();
            item["full_path"] = row["full_path"].as<std::string>();
            item["extension"] = row["extension"].isNull() ? "" : row["extension"].as<std::string>();
            item["created_at"] = row["created_at"].as<std::string>();
            item["size"] = Json::Int64(row["compressed_size"].as<int64_t>());
            item["original_size"] = Json::Int64(row["original_size"].as<int64_t>());
            item["is_compressed"] = row["is_compressed"].as<bool>();
            item["compression_algorithm"] =
                row["compression_algorithm"].isNull() ? Json::Value::null : Json::Value(row["compression_algorithm"].as<std::string>());
            arr.append(std::move(item));
        }
        co_return HttpResponse::newHttpJsonResponse(std::move(arr));
    } catch (const std::exception& ex) {
        co_return createJsonErrorResponse(std::format("Database error: {}", ex.what()), k500InternalServerError);
    }
}

Task<HttpResponsePtr> StorageService::handleMoveObject(
    Json::Value t_session, int32_t t_automated_service_id, std::string t_name, std::string t_new_name) {
    BackendResult<drogon::orm::DbClientPtr> db_res = getDbClient();
    if (!db_res.has_value())
        co_return sgrn::createJsonResponse(db_res);
    drogon::orm::DbClientPtr sp_db_client = db_res.value();

    try {
        // 1. Fetch the file to move
        auto file_res = co_await sp_db_client->execSqlCoro("SELECT id, full_path FROM storage.files "
                                                           "WHERE automated_service_id = $1 AND name = $2 "
                                                           "ORDER BY created_at DESC LIMIT 1",
            t_automated_service_id, t_name);

        if (file_res.empty()) {
            co_return createJsonErrorResponse("Object not found", k404NotFound);
        }

        const int64_t t_file_id = file_res[0]["id"].as<int64_t>();
        const std::string full_path = file_res[0]["full_path"].as<std::string>();

        // 2. Derive new full_path by replacing the filename component
        const std::string::size_type last_slash = full_path.rfind('/');
        const std::string new_full_path = (last_slash == std::string::npos) ? t_new_name : full_path.substr(0, last_slash + 1) + t_new_name;

        // 3. Check for name collision
        auto collision_res = co_await sp_db_client->execSqlCoro("SELECT 1 FROM storage.files "
                                                                "WHERE automated_service_id = $1 AND name = $2 AND id <> $3",
            t_automated_service_id, t_new_name, t_file_id);
        if (!collision_res.empty()) {
            co_return createJsonErrorResponse("An object with that name already exists", k409Conflict);
        }

        // 4. Perform the rename
        auto update_res = co_await sp_db_client->execSqlCoro(
            "UPDATE storage.files SET name = $1, full_path = $2 WHERE id = $3", t_new_name, new_full_path, t_file_id);
        if (update_res.affectedRows() == 0) {
            co_return createJsonErrorResponse("Rename failed", k500InternalServerError);
        }

        Json::Value response;
        response["success"] = true;
        response["id"] = Json::Int64(t_file_id);
        response["name"] = t_new_name;
        response["full_path"] = new_full_path;
        response["old_name"] = t_name;
        co_return HttpResponse::newHttpJsonResponse(std::move(response));
    } catch (const std::exception& ex) {
        co_return createJsonErrorResponse(std::format("Database error: {}", ex.what()), k500InternalServerError);
    }
}

Task<HttpResponsePtr> StorageService::handleDeleteObject(Json::Value t_session, int32_t t_automated_service_id, std::string t_name) {
    BackendResult<drogon::orm::DbClientPtr> db_res = getDbClient();
    if (db_res.hasError()) {
        co_return sgrn::createJsonResponse(db_res);
    }

    auto tsp_db_client = db_res.value();

    try {
        // 1. Fetch metadata
        auto meta_res = co_await tsp_db_client->execSqlCoro("SELECT f.id, "
                                                            "       o.id AS object_id, "
                                                            "       o.bucket, "
                                                            "       o.key "
                                                            "FROM storage.files f "
                                                            "JOIN storage.objects o ON o.id = f.object_id "
                                                            "WHERE f.automated_service_id = $1 "
                                                            "  AND f.name = $2 "
                                                            "ORDER BY f.created_at DESC "
                                                            "LIMIT 1",
            t_automated_service_id, t_name);

        if (meta_res.empty()) {
            co_return createJsonErrorResponse("Object not found", k404NotFound);
        }

        const int64_t t_file_id = meta_res[0]["id"].as<int64_t>();
        const int64_t object_id = meta_res[0]["object_id"].as<int64_t>();
        const std::string t_bucket = meta_res[0]["bucket"].as<std::string>();
        const std::string t_key = meta_res[0]["key"].as<std::string>();

        // 2. Check whether deleting this file will orphan the object
        auto ref_res = co_await tsp_db_client->execSqlCoro("SELECT COUNT(*) AS cnt "
                                                           "FROM storage.files "
                                                           "WHERE object_id = $1",
            object_id);

        const int64_t ref_count = ref_res[0]["cnt"].as<int64_t>();

        const bool will_be_orphaned = (ref_count == 1);

        // 3. If orphaned, delete from storage first
        if (will_be_orphaned) {
            auto s3 = drogon::app().getPlugin<sgrn::datastore::plugins::aws::S3Client>();

            if (!s3) {
                co_return createJsonErrorResponse("Storage backend unavailable", drogon::k503ServiceUnavailable);
            }

            auto s3_res = co_await s3->deleteFile(t_bucket, t_key);

            if (!s3_res.has_value()) {
                ERROR_LOG("[handleDeleteObject] S3 cleanup failed for {}/{}: {}", t_bucket, t_key, s3_res.error().message);

                co_return createJsonErrorResponse(
                    std::format("Storage deletion failed: {}", s3_res.error().message_), drogon::k500InternalServerError);
            }
        }

        // 4. Delete file row
        auto del_res = co_await tsp_db_client->execSqlCoro("DELETE FROM storage.files WHERE id = $1", t_file_id);

        if (del_res.affectedRows() == 0) {
            co_return createJsonErrorResponse("Deletion failed", k500InternalServerError);
        }

        // 5. Remove orphaned object row if your DB no longer does this automatically
        if (will_be_orphaned) {
            co_await tsp_db_client->execSqlCoro("DELETE FROM storage.objects "
                                                "WHERE id = $1",
                object_id);
        }

        Json::Value response;
        response["success"] = true;
        response["name"] = t_name;
        co_return HttpResponse::newHttpJsonResponse(std::move(response));
    } catch (const std::exception& ex) {
        co_return createJsonErrorResponse(std::format("Database error: {}", ex.what()), k500InternalServerError);
    }
}
Task<BackendResult<void>> StorageService::deleteFile(drogon::orm::DbClientPtr tsp_db_client, int64_t t_file_id) {
    try {
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
        const auto& cfg = getConfig();
        BackendResult<drogon::orm::DbClientPtr> db_res = getDbClient();
        if (db_res.hasError()) {
            co_return db_res.error();
        }
        drogon::orm::DbClientPtr sp_db_client = db_res.value();

        BackendResult<bool> validation = co_await validateUpload(t_context, t_file.fileLength());
        if (validation.hasError()) {
            co_return validation.error();
        }

        std::string t_mime_type = helpers::inferMimeType(t_file.getFileName());
        UploadThresholds t_thresholds{.compress_ram_bytes = config_.thresholdCompressRamBytes(),
            .compression_size_threshold = config_.compression_size_threshold_kb * 1024};

        FileHash t_original_hash;
        bool process_in_memory = t_thresholds.shouldProcessInMemory(t_file.fileLength());
        std::string file_data_copy;

        if (process_in_memory) {
            file_data_copy = std::string(t_file.fileData(), t_file.fileLength());
            BackendResult<FileHash> hash = helpers::computeHashInMemory(file_data_copy);
            if (hash.hasError()) {
                co_return hash.error();
            }
            t_original_hash = std::move(hash.value());
        } else {
            BackendResult<TempFileGuard> temp_result = helpers::saveToTempFile(t_file);
            if (temp_result.hasError()) {
                co_return temp_result.error();
            }
            TempFileGuard temp_guard = std::move(*temp_result);
            BackendResult<FileHash> r = co_await helpers::computeHashFromFileAsync(temp_guard.path);
            if (r.hasError()) {
                co_return r.error();
            }
            t_original_hash = std::move(r.value());
        }

        drogon::orm::DbClientPtr sp_transaction = co_await sp_db_client->newTransactionCoro();
        BackendResult<void> upload_res;

        if (process_in_memory) {
            upload_res = co_await executeInMemoryUpload(sp_transaction, t_context, t_file, t_mime_type, t_thresholds,
                config_.compression_level, std::move(file_data_copy), t_original_hash);
        } else {
            upload_res = co_await executeStreamingUpload(
                sp_transaction, t_context, t_file, t_mime_type, t_thresholds, config_.compression_level, t_original_hash);
        }
        if (upload_res.hasError()) {
            co_return upload_res.error(); // transaction destructs here → rollback
        }
        BackendResult<void> finalize_res = co_await finalizeUpload(sp_transaction, t_context);
        if (finalize_res.hasError()) {
            co_return finalize_res.error();
        }
        Json::Value response;
        response["success"] = true;
        if (t_context.file_id.has_value())
            response["file_id"] = Json::Int64(*t_context.file_id);
        if (t_context.object_id.has_value())
            response["object_id"] = Json::Int64(*t_context.object_id);
        response["name"] = t_context.original_filename;
        response["key"] = t_context.identity.hash.key;
        co_return BackendResult<Json::Value>(std::move(response));
    } catch (const std::exception& ex) {
        co_return BackendError{"Runtime", std::format("Upload failed: {}", ex.what())};
    }
}

Task<BackendResult<std::string>> StorageService::downloadFile(std::string t_bucket, std::string t_key) {
    BackendResult<plugins::aws::S3Client*> client_res = S3Client();
    if (client_res.hasError()) {
        co_return client_res.error();
    }
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
    if (t_size > getConfig().maxFileSizeBytes())
        return BackendError{"Filesystem", "File too large"};
    return true;
}

BackendResult<bool> StorageService::validateExtension(std::string_view t_filename) {
    // SEC: Use lowercase comparison to prevent bypass via ".PHP", ".JSP", etc.
    if (!getConfig().isExtensionAllowed(helpers::extractExtensionLower(t_filename)))
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
    if (temp_res.hasError()) {
        co_return temp_res.error();
    }
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
    if (proc_res.hasError()) {
        co_return proc_res.error();
    }

    t_context.identity = std::move(proc_res->first);
    std::string final_data = std::move(proc_res->second);

    BackendResult<int64_t> obj_res = co_await helpers::insertObject(tsp_transaction, t_context.bucket, t_context.identity.hash.key,
        t_context.identity.final_size, t_context.identity.original_size, t_context.identity.is_compressed,
        t_context.identity.compression_algorithm, t_context.identity.compression_level);
    if (obj_res.hasError()) {
        co_return obj_res.error();
    }
    t_context.object_id = *obj_res;

    BackendResult<sgrn::datastore::plugins::aws::S3Client*> s3_res = S3Client();
    if (s3_res.hasError()) {
        co_return s3_res.error();
    }
    co_return co_await s3_res.value()->uploadFromMemory(
        t_context.bucket, t_context.identity.hash.key, std::move(final_data), t_context.identity.mime_type);
}

Task<BackendResult<void>> StorageService::executeStreamingUpload(drogon::orm::DbClientPtr tsp_transaction, UploadContext& t_context,
    const drogon::HttpFile& t_file, std::string t_mime_type, UploadThresholds t_thresholds, uint8_t t_compression_level,
    FileHash t_original_hash) {

    BackendResult<std::pair<FileIdentity, fs::path>> proc_res =
        co_await processStreaming(t_file, std::move(t_mime_type), t_thresholds, t_compression_level, std::move(t_original_hash));
    if (proc_res.hasError()) {
        co_return std::move(proc_res).error();
    }

    t_context.identity = std::move(proc_res->first);
    fs::path upload_path = std::move(proc_res->second);
    TempFileGuard guard(upload_path);

    BackendResult<int64_t> obj_res = co_await helpers::insertObject(tsp_transaction, t_context.bucket, t_context.identity.hash.key,
        t_context.identity.final_size, t_context.identity.original_size, t_context.identity.is_compressed,
        t_context.identity.compression_algorithm, t_context.identity.compression_level);
    if (obj_res.hasError()) {
        co_return std::move(obj_res).error();
    }
    t_context.object_id = *obj_res;

    BackendResult<plugins::aws::S3Client*> s3_res = S3Client();
    if (s3_res.hasError()) {
        co_return std::move(s3_res).error();
    }
    co_return co_await s3_res.value()->uploadFile(t_context.bucket, t_context.identity.hash.key, upload_path.string());
}

Task<BackendResult<void>> StorageService::finalizeUpload(drogon::orm::DbClientPtr tsp_transaction, UploadContext& t_context) {
    BackendResult<std::optional<int64_t>> dir_res = co_await helpers::resolveDirectoryPath(
        tsp_transaction, t_context.user_id, t_context.automated_service_id, t_context.session_id, t_context.virtual_path, t_context.domain);
    if (dir_res.hasError()) {
        co_return std::move(dir_res).error();
    }
    t_context.directory_id = *dir_res;

    BackendResult<int64_t> file_res = co_await helpers::insertFile(tsp_transaction, t_context.original_filename, *t_context.object_id,
        t_context.user_id, t_context.automated_service_id, t_context.session_id, t_context.identity.storageExtension(),
        t_context.directory_id, t_context.domain);
    if (file_res.hasError()) {
        co_return std::move(file_res).error();
    }
    t_context.file_id = *file_res;
    co_return {};
}

} // namespace sgrn::datastore::services::storage
