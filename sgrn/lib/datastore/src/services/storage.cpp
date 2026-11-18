// src/sgrn/services/storage/service.cpp
#include <sgrn/datastore/core/orm-glaze.hpp>
#include <sgrn/datastore/crypto/key-store.hpp>
#include <sgrn/datastore/services/helpers/storage.hpp>
#include <sgrn/datastore/services/storage.hpp>
#include <sgrn/datastore/wire/storage.hpp>
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
struct FilePathRow {
    int64_t id{0};
    std::string full_path;
};

struct ObjectKeyRow {
    std::string key;
};

struct FileObjectRow {
    int64_t id{0};
    int64_t object_id{0};
    std::string bucket;
    std::string key;
};

struct ReferenceCountRow {
    int64_t cnt{0};
};

struct FileEncryptionRow {
    std::optional<std::string> key_id;
    bool is_encrypted{false};
};

struct EncryptionPolicyRow {
    std::string encryption_policy;
};

using StorageConstraintsResponse = ::sgrn::datastore::storage::StorageConstraints;

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

struct CreateDirectoryResponse {
    std::optional<int64_t> directory_id;
};

struct CreateObjectResponse {
    int64_t id{0};
};

struct RenameFileResponse {
    int64_t id{0};
    std::string name;
    std::string full_path;
    std::string old_name;
};

struct DeleteFileServiceResponse {
    std::string name;
};

static constexpr char kPartObjectKeysQuery[] = "SELECT o.key FROM storage.file_objects fo JOIN storage.objects o ON o.id = fo.object_id "
                                               "WHERE fo.file_id = $1 ORDER BY fo.part_index ASC";
static constexpr char kEncryptionPolicyQuery[] = "SELECT encryption_policy FROM core.domains WHERE organisation = $1 AND name = $2";
static constexpr char kFilePathQuery[] = "SELECT id, full_path FROM storage.files "
                                         "WHERE automated_service_id = $1 AND name = $2 "
                                         "ORDER BY created_at DESC LIMIT 1";
static constexpr char kFileObjectQuery[] = "SELECT f.id, "
                                           "       o.id AS object_id, "
                                           "       o.bucket, "
                                           "       o.key "
                                           "FROM storage.files f "
                                           "JOIN storage.file_objects fo ON fo.file_id = f.id AND fo.part_index = 0 "
                                           "JOIN storage.objects o ON o.id = f.object_id "
                                           "WHERE f.automated_service_id = $1 "
                                           "  AND f.name = $2 "
                                           "ORDER BY f.created_at DESC "
                                           "LIMIT 1";
static constexpr char kReferenceCountQuery[] = "SELECT COUNT(*) AS cnt "
                                               "FROM storage.files "
                                               "WHERE object_id = $1";
static constexpr char kFileEncryptionQuery[] = "SELECT key_id, is_encrypted FROM storage.files WHERE id = $1";

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

Result<sgrn::datastore::plugins::aws::S3Client*, BackendError> StorageService::S3Client() const {
    return getPlugin<plugins::aws::S3Client>();
}

Result<drogon::orm::DbClientPtr, BackendError> StorageService::getDbClient() const {
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

Task<Result<ScopeContext, BackendError>> StorageService::resolveScopeSession(
    const ::sgrn::datastore::auth::SessionClaims& t_session, StorageScope t_scope, const std::string& t_path) {
    auto db_res = getDbClient();
    CO_RETURN_IF(!db_res.has_value(), db_res.error());

    co_return co_await helpers::resolveScopeSession(db_res.value(), t_session, t_scope, t_path);
}

// ============================================================================
// High-Level Handlers
// ============================================================================

Task<HttpResponsePtr> StorageService::handleDownloadFileRequest(
    ::sgrn::datastore::auth::SessionClaims t_session, std::string t_scope, std::string t_file_path, std::string t_range_header) {
    auto db_res = getDbClient();
    if (!db_res.has_value())
        co_return sgrn::createTypedResponse(BackendErrorResponse{db_res.error()});
    drogon::orm::DbClientPtr sp_db_client = db_res.value();

    StorageScope parsed_scope = parseScope(t_scope);
    auto scope_res = co_await resolveScopeSession(t_session, parsed_scope, t_file_path);
    if (scope_res.hasError()) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{scope_res.error()});
    }

    if (!scope_res->can_read) {
        co_return createTypedResponse(
            BackendErrorResponse{"Download Denied: Read privileges missing for this scope space.", drogon::k403Forbidden});
    }

    if (scope_res->is_virtual_root) {
        co_return createTypedResponse(BackendErrorResponse{"Cannot download a directory", drogon::k400BadRequest});
    }

    auto record_res = co_await helpers::fetchFileRecord(sp_db_client, *scope_res);
    if (record_res.hasError()) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{record_res.error()});
    }

    auto part_rows = co_await sp_db_client->execSqlCoro(kPartObjectKeysQuery, record_res->id);

    if (part_rows.empty()) {
        co_return createTypedResponse(BackendErrorResponse{"File storage object not found", drogon::k404NotFound});
    }

    std::string file_data;
    const std::string bucket = currentStorageConfig().default_bucket;
    for (const auto& prow : sgrn::orm::fromResult<ObjectKeyRow, kPartObjectKeysQuery>(part_rows)) {
        std::string key = prow.key;
        auto chunk_res = co_await downloadFile(bucket, key);
        if (chunk_res.hasError()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{chunk_res.error()});
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
            co_return createTypedResponse(BackendErrorResponse{"File is marked encrypted but has no key", drogon::k500InternalServerError});
        }
        auto key_store = sgrn::datastore::crypto::getKeyStore();
        if (!key_store) {
            co_return createTypedResponse(BackendErrorResponse{"Key store unavailable", drogon::k500InternalServerError});
        }
        auto key_res = key_store->get(*record_res->key_id);
        if (key_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{"Failed to retrieve file decryption key", drogon::k500InternalServerError});
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
                    co_return createTypedResponse(
                        BackendErrorResponse{"Corrupted encrypted stream header", drogon::k500InternalServerError});
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
                        co_return createTypedResponse(BackendErrorResponse{"Range decryption failed", drogon::k500InternalServerError});
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
                        sp_resp->setContentTypeString(sgrn::utils::mime::inferMimeType(record_res->name));
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
            co_return createTypedResponse(BackendErrorResponse{"Decryption failed", drogon::k500InternalServerError});
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
                sp_resp->setContentTypeString(sgrn::utils::mime::inferMimeType(record_res->name));
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
        sp_resp->setContentTypeString(sgrn::utils::mime::inferMimeType(record_res->name));
    }
    sp_resp->addHeader("Content-Disposition", fmt::format("attachment; filename=\"{}\"", download_name));
    // Total size of the entity after decryption (i.e. the file's stored bytes).
    sp_resp->addHeader("X-Original-Size", std::to_string(file_data.size()));
    sp_resp->setBody(std::move(file_data));
    co_return sp_resp;
}

Task<HttpResponsePtr> StorageService::handleUploadFileRequest(
    ::sgrn::datastore::auth::SessionClaims t_session, std::string t_scope, std::string t_file_path, const drogon::HttpFile& t_http_file) {
    StorageScope parsed_scope = parseScope(t_scope);
    auto scope_res = co_await resolveScopeSession(t_session, parsed_scope, t_file_path);
    if (scope_res.hasError()) {
        co_return createTypedResponse(BackendErrorResponse{scope_res.error()});
    }

    if (!scope_res->can_write) {
        co_return createTypedResponse(
            BackendErrorResponse{"Upload Denied: Write privileges missing for this scope space.", drogon::k403Forbidden});
    }

    // The single-shot endpoint has no encryption pipeline. Never let it
    // bypass a domain's required-encryption policy.
    if (!scope_res->domain.empty()) {
        const std::string& organisation = t_session.user.organisation;
        auto db_res = getDbClient();
        if (db_res.hasError())
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        auto policy = co_await db_res.value()->execSqlCoro(kEncryptionPolicyQuery, organisation, scope_res->domain);
        if (policy.empty())
            co_return createTypedResponse(BackendErrorResponse{"Unknown domain", drogon::k404NotFound});
        if (sgrn::orm::fromRow<EncryptionPolicyRow, kEncryptionPolicyQuery>(policy[0]).encryption_policy == "required") {
            co_return createTypedResponse(BackendErrorResponse{
                "This domain requires encryption: use the chunked upload endpoint.", drogon::k428PreconditionRequired});
        }
    }

    if (scope_res->t_session_id <= 0 || scope_res->owner_id <= 0) {
        co_return createTypedResponse(BackendErrorResponse{"Upload session is missing or invalid", drogon::k401Unauthorized});
    }

    std::string decoded_name = drogon::utils::urlDecode(t_http_file.getFileName());
    std::string base_name = std::filesystem::path(decoded_name).filename().string();

    UploadContext t_context{.original_filename = base_name,
        .virtual_path = std::move(scope_res->actual_path),
        .session_id = scope_res->t_session_id,
        .domain = scope_res->domain,
        .user_id = scope_res->is_automated_service ? std::nullopt : std::optional<int32_t>(scope_res->owner_id),
        .automated_service_id = scope_res->is_automated_service ? std::optional<int32_t>(scope_res->owner_id) : std::nullopt,
        .organisation = t_session.user.organisation,
        .bucket = currentStorageConfig().default_bucket};

    auto r = co_await uploadFile(std::move(t_context), t_http_file);
    if (r.hasError()) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{r.error()});
    }
    co_return sgrn::createTypedResponse(r.value(), k200OK);
}

Task<HttpResponsePtr> StorageService::handleGetConstraints(HttpRequestPtr tsp_req) {
    const StorageConfig cfg = currentStorageConfig();
    co_return sgrn::createTypedResponse(tsp_req,
        StorageConstraintsResponse{
            cfg.max_file_size_mb,
            cfg.maxFileSizeBytes(),
            sgrn::datastore::crypto::isKeyStoreInitialized(),
            cfg.versioning_enabled,
        },
        k200OK);
}

Task<HttpResponsePtr> StorageService::handleCreateDirectoryRequest(
    ::sgrn::datastore::auth::SessionClaims t_session, std::string t_scope, std::string t_path) {
    auto db_res = getDbClient();
    if (db_res.hasError()) {
        co_return createTypedResponse(BackendErrorResponse{db_res.error()});
    }
    drogon::orm::DbClientPtr sp_db_client = db_res.value();

    StorageScope parsed_scope = parseScope(t_scope);
    auto scope_res = co_await resolveScopeSession(t_session, parsed_scope, t_path);
    if (scope_res.hasError()) {
        co_return createTypedResponse(BackendErrorResponse{scope_res.error()});
    }

    if (!scope_res->can_write) {
        co_return createTypedResponse(
            BackendErrorResponse{"Directory Creation Denied: Write privileges missing for this scope space.", drogon::k403Forbidden});
    }

    auto r = co_await helpers::ensureDirectoryPath(sp_db_client, scope_res->user_id, scope_res->t_automated_service_id,
        scope_res->t_session_id, scope_res->actual_path, scope_res.value().domain);
    if (r.hasError()) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{r.error()});
    }

    co_return sgrn::createTypedResponse(CreateDirectoryResponse{r->has_value() ? std::make_optional(**r) : std::nullopt});
}

Task<HttpResponsePtr> StorageService::handleCreateObject(const CreateObjectRequest& t_req) {
    // This is a lower-level API for creating objects directly in the DB
    auto db_res = getDbClient();
    if (db_res.hasError()) {
        co_return createTypedResponse(BackendErrorResponse{db_res.error()});
    }
    drogon::orm::DbClientPtr sp_db_client = db_res.value();

    auto r = co_await helpers::insertObject(sp_db_client, t_req.bucket, t_req.key, t_req.size, t_req.original_size, t_req.sha512,
        t_req.is_compressed, t_req.compression_algorithm);

    if (!r.has_value())
        co_return sgrn::createTypedResponse(BackendErrorResponse{r.error()});
    co_return sgrn::createTypedResponse(CreateObjectResponse{*r});
}

Task<HttpResponsePtr> StorageService::handleListObjects(int32_t t_automated_service_id) {
    auto db_res = getDbClient();
    if (!db_res.has_value())
        co_return createTypedResponse(BackendErrorResponse{db_res.error()});
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
            co_return createTypedResponse(
                BackendErrorResponse{"Failed to serialize object list", k500InternalServerError, "StorageService"});
        }
        std::string response_json = std::move(json_result).value();
        co_return createTypedResponse(sgrn::json::RawJson{std::move(response_json)}, k200OK);
    } catch (const std::exception& ex) {
        co_return createTypedResponse(BackendErrorResponse{std::format("Database error: {}", ex.what()), k500InternalServerError});
    }
}

Task<HttpResponsePtr> StorageService::handleMoveObject(
    ::sgrn::datastore::auth::SessionClaims t_session, int32_t t_automated_service_id, std::string t_name, std::string t_new_name) {
    auto db_res = getDbClient();
    if (!db_res.has_value())
        co_return createTypedResponse(BackendErrorResponse{db_res.error()});
    drogon::orm::DbClientPtr sp_db_client = db_res.value();

    try {
        // 1. Fetch the file to move
        auto file_res = co_await sp_db_client->execSqlCoro(kFilePathQuery, t_automated_service_id, t_name);

        if (file_res.empty()) {
            co_return createTypedResponse(BackendErrorResponse{"Object not found", k404NotFound});
        }

        const auto file = sgrn::orm::fromRow<FilePathRow, kFilePathQuery>(file_res[0]);
        const int64_t t_file_id = file.id;
        const std::string& full_path = file.full_path;

        // 2. Derive new full_path by replacing the filename component
        const std::string::size_type last_slash = full_path.rfind('/');
        const std::string new_full_path = (last_slash == std::string::npos) ? t_new_name : full_path.substr(0, last_slash + 1) + t_new_name;

        // 3. Check for name collision
        auto collision_res = co_await sp_db_client->execSqlCoro("SELECT 1 FROM storage.files "
                                                                "WHERE automated_service_id = $1 AND name = $2 AND id <> $3",
            t_automated_service_id, t_new_name, t_file_id);
        if (!collision_res.empty()) {
            co_return createTypedResponse(BackendErrorResponse{"An object with that name already exists", k409Conflict});
        }

        // 4. Perform the rename
        auto update_res = co_await sp_db_client->execSqlCoro(
            "UPDATE storage.files SET name = $1, full_path = $2 WHERE id = $3", t_new_name, new_full_path, t_file_id);
        if (update_res.affectedRows() == 0) {
            co_return createTypedResponse(BackendErrorResponse{"Rename failed", k500InternalServerError});
        }

        co_return sgrn::createTypedResponse(RenameFileResponse{t_file_id, t_new_name, new_full_path, t_name});
    } catch (const std::exception& ex) {
        co_return createTypedResponse(BackendErrorResponse{std::format("Database error: {}", ex.what()), k500InternalServerError});
    }
}

Task<HttpResponsePtr> StorageService::handleDeleteObject(
    ::sgrn::datastore::auth::SessionClaims t_session, int32_t t_automated_service_id, std::string t_name) {
    auto db_res = getDbClient();
    if (db_res.hasError()) {
        co_return createTypedResponse(BackendErrorResponse{db_res.error()});
    }

    auto tsp_db_client = db_res.value();

    try {
        // 1. Fetch metadata
        auto meta_res = co_await tsp_db_client->execSqlCoro(kFileObjectQuery, t_automated_service_id, t_name);

        if (meta_res.empty()) {
            co_return createTypedResponse(BackendErrorResponse{"Object not found", k404NotFound});
        }

        const auto metadata = sgrn::orm::fromRow<FileObjectRow, kFileObjectQuery>(meta_res[0]);
        const int64_t t_file_id = metadata.id;
        const int64_t object_id = metadata.object_id;
        const std::string& t_bucket = metadata.bucket;
        const std::string& t_key = metadata.key;

        // 2. Check whether deleting this file will orphan the object
        auto ref_res = co_await tsp_db_client->execSqlCoro(kReferenceCountQuery, object_id);

        const int64_t ref_count = sgrn::orm::fromRow<ReferenceCountRow, kReferenceCountQuery>(ref_res[0]).cnt;

        const bool will_be_orphaned = (ref_count == 1);

        // 3. If orphaned, delete from storage first
        if (will_be_orphaned) {
            auto s3 = drogon::app().getPlugin<sgrn::datastore::plugins::aws::S3Client>();

            if (!s3) {
                co_return createTypedResponse(BackendErrorResponse{"Storage backend unavailable", drogon::k503ServiceUnavailable});
            }

            auto s3_res = co_await s3->deleteFile(t_bucket, t_key);

            if (!s3_res.has_value()) {
                ERROR_LOG("[handleDeleteObject] S3 cleanup failed for {}/{}: {}", t_bucket, t_key, s3_res.error().message);

                co_return createTypedResponse(BackendErrorResponse{
                    std::format("Storage deletion failed: {}", s3_res.error().message), drogon::k500InternalServerError});
            }
        }

        // 4. Crypto-shred if encrypted, then delete file row
        auto key_rows = co_await tsp_db_client->execSqlCoro(kFileEncryptionQuery, t_file_id);
        if (!key_rows.empty()) {
            const auto file = sgrn::orm::fromRow<FileEncryptionRow, kFileEncryptionQuery>(key_rows[0]);
            if (file.is_encrypted && file.key_id) {
                auto ks = sgrn::datastore::crypto::getKeyStore();
                if (ks) {
                    (void)ks->shred(*file.key_id);
                }
            }
        }

        auto del_res = co_await tsp_db_client->execSqlCoro("DELETE FROM storage.files WHERE id = $1", t_file_id);

        if (del_res.affectedRows() == 0) {
            co_return createTypedResponse(BackendErrorResponse{"Deletion failed", k500InternalServerError});
        }

        // 5. Remove orphaned object row if your DB no longer does this automatically
        if (will_be_orphaned) {
            co_await tsp_db_client->execSqlCoro("DELETE FROM storage.objects "
                                                "WHERE id = $1",
                object_id);
        }

        co_return sgrn::createTypedResponse(DeleteFileServiceResponse{t_name});
    } catch (const std::exception& ex) {
        co_return createTypedResponse(BackendErrorResponse{std::format("Database error: {}", ex.what()), k500InternalServerError});
    }
}
} // namespace sgrn::datastore::services::storage
