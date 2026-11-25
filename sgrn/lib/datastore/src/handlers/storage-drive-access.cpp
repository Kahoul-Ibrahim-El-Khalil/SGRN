#include <drogon/orm/Exception.h>
#include <fmt/core.h>
#include <sgrn/datastore/core/db.hpp>
#include <sgrn/datastore/core/orm-glaze.hpp>
#include <sgrn/datastore/crypto/key-store.hpp>
#include <sgrn/datastore/error/ApiErrors.hpp>
#include <sgrn/datastore/filters/types.hpp>
#include <sgrn/datastore/handlers/storage.hpp>
#include <sgrn/datastore/services/storage.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe-access.hpp>
#include <sgrn/datastore/wire/storage.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/hashing.hpp>
#include <sgrn/utils/strings.hpp>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <json/value.h>
#include <map>
#include <openssl/evp.h>
#include <unordered_map>
#include <unordered_set>
#include <zstd.h>

#ifdef DEBUG_STORAGE_HANDLER
#define DEBUG_LOG(msg, ...) SGRN_DEBUG("StorageHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define INFO_LOG(msg, ...) SGRN_INFO("StorageHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define WARN_LOG(msg, ...) SGRN_WARN("StorageHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#define ERROR_LOG(msg, ...) SGRN_ERROR("StorageHandler", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define DEBUG_LOG(...) ((void)0)
#define INFO_LOG(...) ((void)0)
#define WARN_LOG(...) ((void)0)
#define ERROR_LOG(...) ((void)0)
#endif

#include "storage-shared.hpp"
#include "storage-drive-internal.hpp"

namespace sgrn::datastore::handlers::storage
{
using namespace drogon;

using sgrn::datastore::services::storage::StorageScope;

Task<HttpResponsePtr> StorageApiHandler::handlePresignedUrl(HttpRequestPtr tsp_req) {
    try {
        auto actor_opt = resolveDriveActor(tsp_req);
        if (!actor_opt) {
            SGRN_WARN_LOG("handlePresignedUrl: No valid session found in request attributes");
            co_return createTypedResponse(BackendErrorResponse{"Unauthorized: Session missing", k401Unauthorized});
        }
        const int32_t user_id = actor_opt->user_id;

        auto db_res = sgrn::datastore::core::getDbClient();
        if (!db_res.has_value()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        // Parse query parameters
        std::string file_id_str = tsp_req->getParameter("file_id");
        std::string action = tsp_req->getParameter("action");     // GET only: objects are content-addressed
        std::string expiry_str = tsp_req->getParameter("expiry"); // seconds, default 3600

        if (file_id_str.empty() || action.empty()) {
            co_return createTypedResponse(BackendErrorResponse{"file_id and action parameters are required", k400BadRequest});
        }

        int64_t file_id;
        try {
            file_id = std::stoll(file_id_str);
        } catch (...) {
            co_return createTypedResponse(BackendErrorResponse{"Invalid file_id", k400BadRequest});
        }

        if (action != "get") {
            co_return createTypedResponse(
                BackendErrorResponse{"Only presigned GET is supported for content-addressed objects", k403Forbidden});
        }

        uint32_t expiry = 3600;
        if (!expiry_str.empty()) {
            try {
                expiry = static_cast<uint32_t>(std::stoul(expiry_str));
            } catch (...) {
                co_return createTypedResponse(BackendErrorResponse{"Invalid expiry parameter", k400BadRequest});
            }
        }
        // Cap expiry at 7 days
        if (expiry > 604800)
            expiry = 604800;

        // Look up the file to get object_id, bucket, key, is_encrypted
        static constexpr char kPresignedFileQuery[] = "SELECT fo.object_id, o.bucket, o.key, f.is_encrypted FROM storage.file_objects fo "
                                                      "JOIN storage.objects o ON o.id = fo.object_id "
                                                      "JOIN storage.files f ON f.id = fo.file_id "
                                                      "WHERE fo.file_id = $1 AND fo.part_index = 0";
        auto file_rows = co_await db->execSqlCoro(kPresignedFileQuery, file_id);
        if (file_rows.empty()) {
            co_return createTypedResponse(BackendErrorResponse{"File not found", k404NotFound});
        }

        const auto file = sgrn::orm::fromRow<detail::PresignedFileRow, kPresignedFileQuery>(file_rows[0]);
        if (file.is_encrypted) {
            co_return createTypedResponse(BackendErrorResponse{"Presigned URLs are not supported for encrypted files", k403Forbidden});
        }

        // Check ownership
        auto own_rows = co_await db->execSqlCoro("SELECT 1 FROM storage.files WHERE id = $1 AND user_id = $2", file_id, user_id);
        if (own_rows.empty()) {
            co_return createTypedResponse(BackendErrorResponse{"Forbidden: File not owned by user", k403Forbidden});
        }

        // Generate presigned URL via S3Client plugin
        auto s3_res = storage_service_.S3Client();
        if (!s3_res.has_value()) {
            co_return createTypedResponse(
                BackendErrorResponse{fmt::format("S3 unavailable: {}", s3_res.error().message), k500InternalServerError});
        }
        auto* s3 = s3_res.value();

        Result<std::string, BackendError> url_res;
        if (action == "get") {
            url_res = co_await s3->presignedGetUrl(file.bucket, file.key, expiry);
        } else if (action == "put") {
            url_res = co_await s3->presignedPutUrl(file.bucket, file.key, expiry);
        } else {
            url_res = co_await s3->presignedDeleteUrl(file.bucket, file.key, expiry);
        }

        if (url_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{
                fmt::format("Failed to generate presigned URL: {}", url_res.error().message), k500InternalServerError});
        }

        co_return createTypedResponse(tsp_req, detail::PresignedUrlResponse{*url_res, std::move(action), expiry, file_id}, k200OK);

    } catch (const std::exception& ex) {
        ERROR_LOG("handlePresignedUrl exception: {}", ex.what());
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    }
}

Task<HttpResponsePtr> StorageApiHandler::handleGetStorageStats(HttpRequestPtr tsp_req) {
    try {
        auto actor_opt = resolveDriveActor(tsp_req);
        if (!actor_opt) {
            SGRN_WARN_LOG("handleGetStorageStats: No valid session found in request attributes");
            co_return createTypedResponse(BackendErrorResponse{"Unauthorized: Session missing", k401Unauthorized});
        }

        const int32_t user_id = actor_opt->user_id;

        auto db_res = sgrn::datastore::core::getDbClient();
        if (!db_res) {
            SGRN_ERROR_LOG("handleGetStorageStats: Database client unavailable");
            co_return createTypedResponse(BackendErrorResponse{"Service Unavailable: Database error", k503ServiceUnavailable});
        }
        auto sp_db_client = db_res.value();

        // Query pre-calculated stats from core.users and count files from storage.files
        static constexpr char kStatsQuery[] = "SELECT (SELECT COUNT(*) FROM storage.files WHERE user_id = $1) AS file_count, "
                                              "u.total_virtual_size AS total_original_bytes, "
                                              "u.total_real_size AS total_compressed_bytes, "
                                              "u.storage_limit "
                                              "FROM core.users u WHERE u.id = $1";

        try {
            auto result = co_await sp_db_client->execSqlCoro(kStatsQuery, user_id);

            auto stats_opt = sgrn::orm::fromSingleResult<detail::StorageStatsResponse, kStatsQuery>(result);
            if (!stats_opt) {
                SGRN_WARN_LOG("handleGetStorageStats: User {} not found in core.users", user_id);
                co_return createTypedResponse(tsp_req, detail::StorageStatsResponse{}, k200OK);
            }

            const auto& stats = *stats_opt;
            DEBUG_LOG("Storage stats for user {}: files={}, raw={}, compressed={}, limit={}", user_id, stats.file_count,
                stats.total_original_bytes, stats.total_compressed_bytes,
                stats.storage_limit ? std::to_string(*stats.storage_limit) : "unlimited");

            co_return createTypedResponse(tsp_req, stats, k200OK);
        } catch (const drogon::orm::DrogonDbException& e) {
            SGRN_ERROR_LOG("handleGetStorageStats: SQL Execution Error: {}", e.base().what());
            co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
        } catch (const std::exception& ex) {
            SGRN_ERROR_LOG("Storage stats handler exception: {}", ex.what());
            co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
        }
    } catch (const drogon::orm::DrogonDbException& e) {
        SGRN_ERROR_LOG("handleGetStorageStats: DB exception: {}", e.base().what());
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    } catch (const std::exception& ex) {
        SGRN_ERROR_LOG("handleGetStorageStats: exception: {}", ex.what());
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    }
}

drogon::Task<drogon::HttpResponsePtr> StorageApiHandler::handleRecursiveDownload(drogon::HttpRequestPtr tsp_req) {
    co_return createTypedResponse(BackendErrorResponse{"Recursive download (ZIP) is not yet implemented.", k501NotImplemented});
}
} // namespace sgrn::datastore::handlers::storage

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
