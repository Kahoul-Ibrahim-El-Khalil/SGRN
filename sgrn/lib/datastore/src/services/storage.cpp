// src/sgrn/services/storage/service.cpp
#include <sgrn/datastore/core/orm-glaze.hpp>
#include <sgrn/datastore/crypto/key-store.hpp>
#include <sgrn/datastore/services/helpers/magic.hpp>
#include <sgrn/datastore/services/helpers/storage.hpp>
#include <sgrn/datastore/services/storage.hpp>
#include <sgrn/utils/crypto/stream.hpp>

#include <drogon/HttpAppFramework.h>
#include <drogon/orm/DbClient.h>
#include <sgrn/datastore/plugins/aws/S3Client.hpp>
#include <sgrn/datastore/plugins/threadpool/Threadpool.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe-access.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/hashing.hpp>
#include <sgrn/utils/mime.hpp>
#include <mutex>
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

namespace
{
struct StorageConstraintsResponse {
    std::uint64_t max_file_size_mb{0};
    std::uint64_t max_file_size_bytes{0};
    bool encryption_supported{false};
};

struct ServiceObjectResponse {
    int64_t id{0};
    std::string name;
    std::string full_path;
    std::string extension;
    std::string created_at;
    int64_t size{0};
    int64_t original_size{0};
    bool is_compressed{false};
    std::optional<std::string> compression_algorithm;
};
} // namespace
namespace fs = std::filesystem;
using namespace drogon;
using namespace sgrn::datastore::plugins;
using namespace sgrn::datastore::core;

// ============================================================================
// StorageService Implementation
// ============================================================================

StorageService::StorageService() {
    publishStorageConfig(StorageConfig::loadFromConfig());
}

BackendResult<plugins::aws::S3Client*> StorageService::S3Client() const {
    return getPlugin<plugins::aws::S3Client>();
}

BackendResult<drogon::orm::DbClientPtr> StorageService::getDbClient() const {
    return drogon::app().getDbClient();
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

Task<HttpResponsePtr> StorageService::handleDownloadFileRequest(
    Json::Value t_session, std::string t_scope, std::string t_file_path, std::string t_range_header) {
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

    auto part_rows =
        co_await sp_db_client->execSqlCoro("SELECT o.key FROM storage.file_objects fo JOIN storage.objects o ON o.id = fo.object_id "
                                           "WHERE fo.file_id = $1 ORDER BY fo.part_index ASC",
            record_res->id);

    if (part_rows.empty()) {
        co_return createJsonErrorResponse("File storage object not found", drogon::k404NotFound);
    }

    std::string file_data;
    const std::string bucket = currentStorageConfig().default_bucket;
    for (const auto& prow : part_rows) {
        std::string key = prow["key"].as<std::string>();
        BackendResult<std::string> chunk_res = co_await downloadFile(bucket, key);
        if (chunk_res.hasError()) {
            co_return sgrn::createJsonResponse(std::move(chunk_res));
        }
        file_data.append(chunk_res.value());
    }

    std::string download_name = record_res->name;

    // SEC: Strip CR, LF, NUL, and double-quote to prevent HTTP header injection
    download_name.erase(std::remove_if(download_name.begin(), download_name.end(),
                            [](char t_c) { return t_c == '\r' || t_c == '\n' || t_c == '\0' || t_c == '"'; }),
        download_name.end());

    HttpResponsePtr sp_resp = HttpResponse::newHttpResponse();
    sp_resp->addHeader("Accept-Ranges", "bytes");

    if (record_res->is_encrypted) {
        if (!record_res->key_id.has_value() || record_res->key_id->empty()) {
            co_return createJsonErrorResponse("File is marked encrypted but has no key", drogon::k500InternalServerError);
        }
        auto key_store = sgrn::datastore::crypto::getKeyStore();
        if (!key_store) {
            co_return createJsonErrorResponse("Key store unavailable", drogon::k500InternalServerError);
        }
        auto key_res = key_store->get(*record_res->key_id);
        if (key_res.hasError()) {
            co_return createJsonErrorResponse("Failed to retrieve file decryption key", drogon::k500InternalServerError);
        }
        const auto& key_rec = key_res.value();
        std::span<const uint8_t> ct_span(reinterpret_cast<const uint8_t*>(file_data.data()), file_data.size());

        if (!t_range_header.empty() && t_range_header.starts_with("bytes=")) {
            std::string range_spec = t_range_header.substr(6);
            auto dash_pos = range_spec.find('-');
            if (dash_pos != std::string::npos) {
                size_t start_byte = 0;
                size_t end_byte = 0;
                try {
                    start_byte = std::stoull(range_spec.substr(0, dash_pos));
                } catch (...) {
                }
                std::string end_str = range_spec.substr(dash_pos + 1);

                size_t hdr_len = 0;
                auto hdr_res = sgrn::utils::crypto::StreamHeader::deserialize(ct_span, hdr_len);
                if (hdr_res.hasError()) {
                    co_return createJsonErrorResponse("Corrupted encrypted stream header", drogon::k500InternalServerError);
                }
                uint64_t total_plain_size = hdr_res.value().total_plaintext_size;
                if (!end_str.empty()) {
                    try {
                        end_byte = std::stoull(end_str);
                    } catch (...) {
                    }
                } else {
                    end_byte = total_plain_size > 0 ? total_plain_size - 1 : 0;
                }
                if (start_byte < total_plain_size) {
                    if (end_byte >= total_plain_size)
                        end_byte = total_plain_size - 1;
                    size_t content_length = end_byte - start_byte + 1;
                    auto range_res =
                        sgrn::utils::crypto::decryptRange(ct_span, key_rec.dek, start_byte, content_length, *record_res->key_id);
                    if (range_res.hasError()) {
                        co_return createJsonErrorResponse("Range decryption failed", drogon::k500InternalServerError);
                    }
                    std::string chunk(reinterpret_cast<const char*>(range_res.value().data()), range_res.value().size());
                    sp_resp->setStatusCode(k206PartialContent);
                    sp_resp->addHeader("Content-Range", fmt::format("bytes {}-{}/{}", start_byte, end_byte, total_plain_size));
                    // NOTE: no manual Content-Length — Drogon derives it from the
                    // response body; adding it by hand yields a duplicate header
                    // (nginx rejects upstream responses with double
                    // Content-Length as "duplicate ... header" → 502/truncated).
                    if (record_res->is_compressed_) {
                        sp_resp->setContentTypeString("application/zstd");
                        sp_resp->addHeader("X-Compressed", "true");
                    } else {
                        sp_resp->setContentTypeString(helpers::inferMimeType(record_res->name));
                    }
                    sp_resp->addHeader("Content-Disposition", fmt::format("attachment; filename=\"{}\"", download_name));
                    sp_resp->addHeader("X-Original-Size", std::to_string(total_plain_size));
                    sp_resp->setBody(std::move(chunk));
                    co_return sp_resp;
                }
            }
        }

        // Full file decryption
        auto dec_res = sgrn::utils::crypto::decryptBuffer(ct_span, key_rec.dek, *record_res->key_id);
        if (dec_res.hasError()) {
            co_return createJsonErrorResponse("Decryption failed", drogon::k500InternalServerError);
        }
        file_data = std::string(reinterpret_cast<const char*>(dec_res.value().data()), dec_res.value().size());
    }

    if (!t_range_header.empty() && t_range_header.starts_with("bytes=")) {
        std::string range_spec = t_range_header.substr(6);
        size_t total_size = file_data.size();
        size_t start_byte = 0;
        size_t end_byte = total_size > 0 ? total_size - 1 : 0;

        auto dash_pos = range_spec.find('-');
        if (dash_pos != std::string::npos) {
            std::string start_str = range_spec.substr(0, dash_pos);
            std::string end_str = range_spec.substr(dash_pos + 1);
            if (!start_str.empty()) {
                try {
                    start_byte = std::stoull(start_str);
                } catch (...) {
                }
            }
            if (!end_str.empty()) {
                try {
                    end_byte = std::stoull(end_str);
                } catch (...) {
                }
            }
        }

        if (start_byte < total_size) {
            if (end_byte >= total_size)
                end_byte = total_size - 1;
            size_t content_length = end_byte - start_byte + 1;
            std::string chunk = file_data.substr(start_byte, content_length);

            sp_resp->setStatusCode(k206PartialContent);
            sp_resp->addHeader("Content-Range", fmt::format("bytes {}-{}/{}", start_byte, end_byte, total_size));
            // No manual Content-Length — see the encrypted branch above.
            if (record_res->is_compressed_) {
                sp_resp->setContentTypeString("application/zstd");
                sp_resp->addHeader("X-Compressed", "true");
            } else {
                sp_resp->setContentTypeString(helpers::inferMimeType(record_res->name));
            }
            sp_resp->addHeader("Content-Disposition", fmt::format("attachment; filename=\"{}\"", download_name));
            sp_resp->addHeader("X-Original-Size", std::to_string(total_size));
            sp_resp->setBody(std::move(chunk));
            co_return sp_resp;
        }
    }

    sp_resp->setStatusCode(k200OK);
    if (record_res->is_compressed_) {
        sp_resp->setContentTypeString("application/zstd");
        sp_resp->addHeader("X-Compressed", "true");
    } else {
        sp_resp->setContentTypeString(helpers::inferMimeType(record_res->name));
    }
    sp_resp->addHeader("Content-Disposition", fmt::format("attachment; filename=\"{}\"", download_name));
    // Total size of the entity after decryption (i.e. the file's stored bytes).
    sp_resp->addHeader("X-Original-Size", std::to_string(file_data.size()));
    sp_resp->setBody(std::move(file_data));
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

    // The single-shot endpoint has no encryption pipeline. Never let it
    // bypass a domain's required-encryption policy.
    if (!scope_res->domain.empty()) {
        const std::string organisation = t_session["user"].isMember("organisation") ? t_session["user"]["organisation"].asString() : "";
        auto db_res = getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto policy = co_await db_res.value()->execSqlCoro(
            "SELECT encryption_policy FROM core.domains WHERE organisation = $1 AND name = $2", organisation, scope_res->domain);
        if (policy.empty())
            co_return createJsonErrorResponse("Unknown domain", drogon::k404NotFound);
        if (policy[0]["encryption_policy"].as<std::string>() == "required") {
            co_return createJsonErrorResponse(
                "This domain requires encryption: use the chunked upload endpoint.", drogon::k428PreconditionRequired);
        }
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
        .bucket = currentStorageConfig().default_bucket};

    BackendResult<UploadResponse> r = co_await uploadFile(std::move(t_context), t_http_file);
    if (r.hasError()) {
        co_return sgrn::createJsonResponse(r);
    }
    co_return sgrn::createTypedJsonResponse(r.value(), k200OK);
}

Task<HttpResponsePtr> StorageService::handleGetConstraints() {
    const StorageConfig cfg = currentStorageConfig();
    co_return sgrn::createTypedJsonResponse(
        StorageConstraintsResponse{
            cfg.max_file_size_mb,
            cfg.maxFileSizeBytes(),
            sgrn::datastore::crypto::isKeyStoreInitialized(),
        },
        k200OK);
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

    auto response = sgrn::JsonObjectBuilder(64).boolean("success", true);
    if (r->has_value()) {
        response.int64("directory_id", **r);
    } else {
        response.null("directory_id");
    }
    co_return sgrn::createJsonResponse(std::move(response).finish());
}

Task<HttpResponsePtr> StorageService::handleCreateObject(const Json::Value& t_json) {
    // This is a lower-level API for creating objects directly in the DB
    BackendResult<drogon::orm::DbClientPtr> db_res = getDbClient();
    if (db_res.hasError()) {
        co_return sgrn::createJsonResponse(db_res);
    }
    drogon::orm::DbClientPtr sp_db_client = db_res.value();

    std::string sha256 = t_json.isMember("sha256") ? t_json["sha256"].asString() : "";
    BackendResult<int64_t> r = co_await helpers::insertObject(sp_db_client, t_json["bucket"].asString(), t_json["key"].asString(),
        t_json["size"].asInt64(), t_json["original_size"].asInt64(), sha256, t_json["is_compressed"].asBool(),
        t_json.isMember("compression_algorithm") ? std::optional<std::string>(t_json["compression_algorithm"].asString()) : std::nullopt);

    if (!r.has_value())
        co_return sgrn::createJsonResponse(r);
    auto response = sgrn::JsonObjectBuilder(48).boolean("success", true).int64("id", *r);
    co_return sgrn::createJsonResponse(std::move(response).finish());
}

Task<HttpResponsePtr> StorageService::handleListObjects(int32_t t_automated_service_id) {
    BackendResult<drogon::orm::DbClientPtr> db_res = getDbClient();
    if (!db_res.has_value())
        co_return sgrn::createJsonResponse(db_res);
    drogon::orm::DbClientPtr sp_db_client = db_res.value();

    try {
        static constexpr char kQuery[] = "SELECT f.id, f.name, f.full_path, f.extension, f.created_at, "
                                         "       so.size, so.original_size, "
                                         "       so.is_compressed, so.compression_algorithm "
                                         "FROM storage.files f "
                                         "JOIN storage.objects so ON so.id = f.object_id "
                                         "WHERE f.automated_service_id = $1 "
                                         "ORDER BY f.created_at DESC";

        auto result = co_await sp_db_client->execSqlCoro(kQuery, t_automated_service_id);

        auto objects = sgrn::orm::fromResult<ServiceObjectResponse, kQuery>(result);
        auto json_result = sgrn::json::write(objects);
        if (json_result.hasError()) {
            co_return createErrorResponse("Failed to serialize object list", k500InternalServerError, "StorageService");
        }
        std::string response_json = std::move(json_result).value();
        co_return createJsonResponse(std::move(response_json), k200OK);
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

        auto response = sgrn::JsonObjectBuilder(192)
                            .boolean("success", true)
                            .int64("id", t_file_id)
                            .string("name", t_new_name)
                            .string("full_path", new_full_path)
                            .string("old_name", t_name);
        co_return sgrn::createJsonResponse(std::move(response).finish());
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

        // 4. Crypto-shred if encrypted, then delete file row
        auto key_rows = co_await tsp_db_client->execSqlCoro("SELECT key_id, is_encrypted FROM storage.files WHERE id = $1", t_file_id);
        if (!key_rows.empty() && key_rows[0]["is_encrypted"].as<bool>() && !key_rows[0]["key_id"].isNull()) {
            std::string kid = key_rows[0]["key_id"].as<std::string>();
            auto ks = sgrn::datastore::crypto::getKeyStore();
            if (ks) {
                (void)ks->shred(kid);
            }
        }

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

        auto response = sgrn::JsonObjectBuilder(96).boolean("success", true).string("name", t_name);
        co_return sgrn::createJsonResponse(std::move(response).finish());
    } catch (const std::exception& ex) {
        co_return createJsonErrorResponse(std::format("Database error: {}", ex.what()), k500InternalServerError);
    }
}
} // namespace sgrn::datastore::services::storage
