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

Task<HttpResponsePtr> StorageApiHandler::handleCreateDirectory(HttpRequestPtr tsp_req) {
    try {
        auto path_opt = tsp_req->getOptionalParameter<std::string>("path");
        if (!path_opt.has_value()) {
            co_return createTypedResponse(BackendErrorResponse{"Missing 'path' query parameter", k400BadRequest});
        }
        std::string path = sgrn::utils::strings::trim(std::move(*path_opt));
        if (path.empty()) {
            co_return createTypedResponse(BackendErrorResponse{"Path parameter cannot be empty", k400BadRequest});
        }

        std::string scope = tsp_req->getOptionalParameter<std::string>("scope").value_or("personal");

        if (!tsp_req->attributes()->find("session_claims")) {
            co_return createTypedResponse(BackendErrorResponse{"No session found", k401Unauthorized});
        }
        auto session = tsp_req->getAttributes()->get<::sgrn::datastore::auth::SessionClaims>("session_claims");

        co_return co_await storage_service_.handleCreateDirectoryRequest(std::move(session), std::move(scope), std::move(path));

    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Create directory DB exception: {}", e.base().what());
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    } catch (const std::exception& ex) {
        ERROR_LOG("Create directory handler exception: {}", ex.what());
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    }
}

Task<HttpResponsePtr> StorageApiHandler::handleMove(HttpRequestPtr tsp_req) {
    try {
        // 1. Session and Authorization Check
        auto actor_opt = resolveDriveActor(tsp_req);
        if (!actor_opt) {
            co_return createTypedResponse(BackendErrorResponse{"No session found", k401Unauthorized});
        }

        const auto& actor = *actor_opt;
        const int32_t user_id = actor.user_id;
        const bool is_admin = actor.is_admin;

        // 2. Parameter Extraction
        std::string type = tsp_req->getOptionalParameter<std::string>("type").value_or("file");
        std::string id = tsp_req->getOptionalParameter<std::string>("id").value_or("");
        if (id.empty()) {
            co_return createTypedResponse(BackendErrorResponse{"Missing 'id' parameter", k400BadRequest});
        }
        if (type != "file" && type != "folder") {
            co_return createTypedResponse(BackendErrorResponse{"type must be 'file' or 'folder'", k400BadRequest});
        }

        auto req_res = parseTypedRequest<detail::MoveItemRequest>(tsp_req);
        if (req_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{"Invalid JSON body", k400BadRequest});
        }
        const auto& req = req_res.value();

        const int64_t entity_id = [&]() -> int64_t {
            try {
                return std::stoll(id);
            } catch (const std::exception& e) {
                SGRN_WARN_LOG("StorageHandler", "Failed to parse entity_id '{}': {}", id, e.what());
                return -1;
            }
        }();
        if (entity_id < 0) {
            co_return createTypedResponse(BackendErrorResponse{"id must be a valid integer", k400BadRequest});
        }

        // 3. Database Transaction Initiation
        auto db_res = storage_service_.getDbClient();
        if (!db_res.has_value()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db_client = db_res.value();
        auto transaction = co_await db_client->newTransactionCoro();
        if (!transaction) {
            co_return createTypedResponse(BackendErrorResponse{"Database transaction unavailable", k503ServiceUnavailable});
        }

        // 4. Target Resolution (Optional parent_id and new_name)
        std::optional<std::string> target_name;
        if (req.new_name.has_value() && !req.new_name->empty()) {
            const std::string name_value = sgrn::utils::strings::trim(*req.new_name);
            if (!name_value.empty())
                target_name = name_value;
        } else if (req.name.has_value() && !req.name->empty()) {
            const std::string name_value = sgrn::utils::strings::trim(*req.name);
            if (!name_value.empty())
                target_name = name_value;
        }

        const std::optional<int64_t> target_parent_id = req.parent_id;

        // SEC: Validate the new name to prevent path-traversal injection.
        // Uses sgrn::utils::strings::isValidFileName() to reject names containing
        // path separators (/ or \), "..", or null bytes — ensuring the rename
        // target stays within the current directory.
        if (target_name.has_value()) {
            auto name_result = sgrn::utils::strings::isValidFileName(*target_name);
            if (name_result.hasError()) {
                co_return createTypedResponse(BackendErrorResponse{std::format("Invalid name: {}", name_result.error()), k400BadRequest});
            }
        }

        // 5. Delegate to Specialized Helpers
        if (type == "file") {
            co_return co_await moveFile(transaction, entity_id, user_id, is_admin, target_parent_id, target_name);
        } else {
            co_return co_await moveFolder(transaction, entity_id, user_id, is_admin, target_parent_id, target_name);
        }

    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Move handler DB exception: {}", e.base().what());
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    } catch (const std::exception& ex) {
        ERROR_LOG("Move handler exception: {}", ex.what());
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    }
}

Task<HttpResponsePtr> StorageApiHandler::moveFile(const std::shared_ptr<drogon::orm::Transaction>& tsp_transaction, int64_t t_entity_id,
    int32_t t_user_id, bool t_is_admin, std::optional<int64_t> t_target_parent_id, std::optional<std::string> t_target_name) {
    // 1. Fetch current file state
    auto file_res = t_is_admin ? co_await tsp_transaction->execSqlCoro(detail::kAdminFileMoveStateQuery, t_entity_id)
                               : co_await tsp_transaction->execSqlCoro(detail::kUserFileMoveStateQuery, t_entity_id, t_user_id);

    if (file_res.empty()) {
        tsp_transaction->rollback();
        co_return createTypedResponse(BackendErrorResponse{"File not found or access denied", k404NotFound});
    }

    const auto file_row = t_is_admin ? sgrn::orm::fromRow<detail::FileMoveStateRow, detail::kAdminFileMoveStateQuery>(file_res[0])
                                     : sgrn::orm::fromRow<detail::FileMoveStateRow, detail::kUserFileMoveStateQuery>(file_res[0]);
    const auto& file_user_id = file_row.user_id;
    const auto& file_service_id = file_row.automated_service_id;
    const auto& current_directory_id = file_row.directory_id;

    // 2. Short-circuit if no changes
    if (current_directory_id == t_target_parent_id && !t_target_name.has_value()) {
        tsp_transaction->rollback();
        co_return createTypedResponse(detail::MoveItemResponse{t_entity_id, "file", std::nullopt, std::nullopt}, k200OK);
    }

    // 3. Validate target directory ownership
    if (t_target_parent_id.has_value()) {
        std::optional<drogon::orm::Result> dir_res;
        if (file_service_id.has_value()) {
            dir_res = co_await tsp_transaction->execSqlCoro(
                "SELECT id FROM storage.directories WHERE id = $1 AND automated_service_id = $2", *t_target_parent_id, *file_service_id);
        } else {
            dir_res = co_await tsp_transaction->execSqlCoro(
                "SELECT id FROM storage.directories WHERE id = $1 AND user_id = $2", *t_target_parent_id, file_user_id.value_or(t_user_id));
        }

        if (!dir_res.has_value() || dir_res->empty()) {
            tsp_transaction->rollback();
            co_return createTypedResponse(BackendErrorResponse{"Target directory does not exist or access denied", k403Forbidden});
        }
    }

    // 4. Re-derive the format when the name changes: a suffix the formats
    // registry acknowledges keeps its canonical (lowercased) extension, any
    // other suffix is stored as NULL and remains part of the name.
    std::optional<std::string> renamed_extension;
    if (t_target_name.has_value()) {
        const std::string raw_ext(sgrn::utils::strings::extractExtension(*t_target_name));
        if (!raw_ext.empty()) {
            auto format_res = co_await services::storage::helpers::getFormat(tsp_transaction, raw_ext);
            if (format_res.has_value() && format_res->getExtension())
                renamed_extension = *format_res->getExtension();
        }
    }

    // 5. Perform Update
    auto update_res =
        (t_target_parent_id.has_value() && t_target_name.has_value())
            ? co_await tsp_transaction->execSqlCoro("UPDATE storage.files SET directory_id = $1, name = $2, extension = $3 WHERE id = $4",
                  *t_target_parent_id, *t_target_name, renamed_extension, t_entity_id)
            : (t_target_parent_id.has_value()
                      ? co_await tsp_transaction->execSqlCoro(
                            "UPDATE storage.files SET directory_id = $1 WHERE id = $2", *t_target_parent_id, t_entity_id)
                      : (t_target_name.has_value()
                                ? co_await tsp_transaction->execSqlCoro(
                                      "UPDATE storage.files SET directory_id = NULL, name = $1, extension = $2 WHERE id = $3",
                                      *t_target_name, renamed_extension, t_entity_id)
                                : co_await tsp_transaction->execSqlCoro(
                                      "UPDATE storage.files SET directory_id = NULL WHERE id = $1", t_entity_id)));

    if (update_res.affectedRows() == 0) {
        tsp_transaction->rollback();
        co_return createTypedResponse(BackendErrorResponse{"Update failed", k500InternalServerError});
    }

    co_return createTypedResponse(detail::MoveItemResponse{t_entity_id, "file", t_target_parent_id, t_target_name}, k200OK);
}

Task<HttpResponsePtr> StorageApiHandler::moveFolder(const std::shared_ptr<drogon::orm::Transaction>& tsp_transaction, int64_t t_entity_id,
    int32_t t_user_id, bool t_is_admin, std::optional<int64_t> t_target_parent_id, std::optional<std::string> t_target_name) {
    // 1. Fetch current folder state
    auto dir_res = t_is_admin ? co_await tsp_transaction->execSqlCoro(detail::kAdminFolderMoveStateQuery, t_entity_id)
                              : co_await tsp_transaction->execSqlCoro(detail::kUserFolderMoveStateQuery, t_entity_id, t_user_id);

    if (dir_res.empty()) {
        tsp_transaction->rollback();
        co_return createTypedResponse(BackendErrorResponse{"Folder not found or access denied", k404NotFound});
    }

    const auto dir_row = t_is_admin ? sgrn::orm::fromRow<detail::FolderMoveStateRow, detail::kAdminFolderMoveStateQuery>(dir_res[0])
                                    : sgrn::orm::fromRow<detail::FolderMoveStateRow, detail::kUserFolderMoveStateQuery>(dir_res[0]);
    const auto& dir_user_id = dir_row.user_id;
    const auto& dir_service_id = dir_row.automated_service_id;
    const auto& current_parent_id = dir_row.parent_id;
    const std::string& current_path = dir_row.path;

    // 2. Short-circuit if no changes
    if (current_parent_id == t_target_parent_id && !t_target_name.has_value()) {
        tsp_transaction->rollback();
        co_return createTypedResponse(detail::MoveItemResponse{t_entity_id, "folder", std::nullopt, std::nullopt}, k200OK);
    }

    // 3. Validate target and prevent cycles
    if (t_target_parent_id.has_value()) {
        if (*t_target_parent_id == t_entity_id) {
            tsp_transaction->rollback();
            co_return createTypedResponse(BackendErrorResponse{"A folder cannot be moved into itself", k400BadRequest});
        }

        std::optional<drogon::orm::Result> target_res;
        if (dir_service_id.has_value()) {
            target_res = co_await tsp_transaction->execSqlCoro(detail::kServiceDirectoryPathQuery, *t_target_parent_id, *dir_service_id);
        } else {
            target_res = co_await tsp_transaction->execSqlCoro(
                detail::kUserDirectoryPathQuery, *t_target_parent_id, dir_user_id.value_or(t_user_id));
        }

        if (!target_res.has_value() || target_res->empty()) {
            tsp_transaction->rollback();
            co_return createTypedResponse(BackendErrorResponse{"Target directory does not exist or access denied", k403Forbidden});
        }

        const auto target = dir_service_id.has_value()
                                ? sgrn::orm::fromRow<detail::DirectoryPathRow, detail::kServiceDirectoryPathQuery>((*target_res)[0])
                                : sgrn::orm::fromRow<detail::DirectoryPathRow, detail::kUserDirectoryPathQuery>((*target_res)[0]);
        const std::string& target_path = target.path;
        const std::string descendant_prefix = current_path + "/";
        if (target_path == current_path || target_path.rfind(descendant_prefix, 0) == 0) {
            tsp_transaction->rollback();
            co_return createTypedResponse(BackendErrorResponse{"A folder cannot be moved into its own descendant", k400BadRequest});
        }
    }

    // 4. Perform Update
    auto update_res = (t_target_parent_id.has_value() && t_target_name.has_value())
                          ? co_await tsp_transaction->execSqlCoro("UPDATE storage.directories SET parent_id = $1, name = $2 WHERE id = $3",
                                *t_target_parent_id, *t_target_name, t_entity_id)
                          : (t_target_parent_id.has_value()
                                    ? co_await tsp_transaction->execSqlCoro(
                                          "UPDATE storage.directories SET parent_id = $1 WHERE id = $2", *t_target_parent_id, t_entity_id)
                                    : (t_target_name.has_value()
                                              ? co_await tsp_transaction->execSqlCoro(
                                                    "UPDATE storage.directories SET name = $1 WHERE id = $2", *t_target_name, t_entity_id)
                                              : co_await tsp_transaction->execSqlCoro(
                                                    "UPDATE storage.directories SET parent_id = NULL WHERE id = $1", t_entity_id)));

    if (update_res.affectedRows() == 0) {
        tsp_transaction->rollback();
        co_return createTypedResponse(BackendErrorResponse{"Update failed", k500InternalServerError});
    }

    co_return createTypedResponse(detail::MoveItemResponse{t_entity_id, "folder", t_target_parent_id, t_target_name}, k200OK);
}

Task<HttpResponsePtr> StorageApiHandler::handleDelete(HttpRequestPtr tsp_req) {
    try {
        auto session = tsp_req->getAttributes()->get<::sgrn::datastore::auth::SessionClaims>("session_claims");
        if (session.user.id <= 0) {
            co_return createTypedResponse(BackendErrorResponse{"No session found", k401Unauthorized});
        }

        std::string type = tsp_req->getOptionalParameter<std::string>("type").value_or("file");
        std::string id = tsp_req->getOptionalParameter<std::string>("id").value_or("");
        if (id.empty()) {
            co_return createTypedResponse(BackendErrorResponse{"Missing 'id' parameter", k400BadRequest});
        }
        if (type != "file" && type != "folder") {
            co_return createTypedResponse(BackendErrorResponse{"type must be 'file' or 'folder'", k400BadRequest});
        }

        if (session.user.role.name.empty()) {
            co_return createTypedResponse(
                BackendErrorResponse{"Corrupted session: user.role.name missing or not a string", k500InternalServerError});
        }
        if (session.user.id <= 0) {
            co_return createTypedResponse(
                BackendErrorResponse{"Corrupted session: user.id missing or not an integer", k500InternalServerError});
        }

        const bool is_admin = (session.user.role.name == "admin");
        if (session.session_id <= 0) {
            co_return createTypedResponse(BackendErrorResponse{"Corrupted session: session_id missing", k500InternalServerError});
        }

        const int32_t user_id = session.user.id;
        const int64_t entity_id = [&]() -> int64_t {
            try {
                return std::stoll(id);
            } catch (const std::exception&) {
                return -1;
            }
        }();
        if (entity_id < 0) {
            co_return createTypedResponse(BackendErrorResponse{"id must be a valid integer", k400BadRequest});
        }

        auto db_res = storage_service_.getDbClient();
        if (!db_res.has_value()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db_client = db_res.value();

        if (type == "file") {
            co_return co_await deleteFile(db_client, entity_id, user_id, is_admin);
        } else {
            co_return co_await deleteFolder(db_client, entity_id, user_id, is_admin);
        }

        co_return createTypedResponse(tsp_req, detail::DeleteResponse{entity_id, type}, k200OK);

    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Delete handler DB exception: {}", e.base().what());
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    } catch (const std::exception& ex) {
        ERROR_LOG("Delete handler exception: {}", ex.what());
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    }
}

Task<HttpResponsePtr> StorageApiHandler::deleteFile(
    const drogon::orm::DbClientPtr& tsp_db_client, int64_t t_entity_id, int32_t t_user_id, bool t_is_admin) {

    // Single query: fetch file ownership + permission in one pass.
    // LEFT JOINs return NULL columns when the user has no domain permission
    // or the file has no user_id (domain-owned), both handled below.
    static constexpr char kFileDeletePermissionQuery[] = "SELECT f.id, f.user_id, f.domain, "
                                                         "       udp.can_delete  AS domain_can_delete, "
                                                         "       u.can_delete_personal "
                                                         "FROM storage.files f "
                                                         "LEFT JOIN core.user_domain_permissions udp "
                                                         "       ON udp.user_id = $2 AND udp.domain = f.domain "
                                                         "LEFT JOIN core.users u ON u.id = $2 "
                                                         "WHERE f.id = $1";
    auto file_res = co_await tsp_db_client->execSqlCoro(kFileDeletePermissionQuery, t_entity_id, t_user_id);

    if (file_res.empty()) {
        co_return createTypedResponse(BackendErrorResponse{"File not found or access denied", k404NotFound});
    }

    const auto fr = sgrn::orm::fromRow<detail::DeletePermissionRow, kFileDeletePermissionQuery>(file_res[0]);
    const std::string domain_name = fr.domain.value_or("");

    if (!t_is_admin) {
        if (!domain_name.empty()) {
            // Shared domain: require explicit can_delete permission
            if (!fr.domain_can_delete.value_or(false)) {
                co_return createTypedResponse(
                    BackendErrorResponse{"Deletion Denied: Delete capability is not granted for this operational domain.", k403Forbidden});
            }
        } else {
            // Personal workspace: must be owner + can_delete_personal flag
            if (!fr.user_id || *fr.user_id != t_user_id) {
                co_return createTypedResponse(BackendErrorResponse{"File not found or access denied", k404NotFound});
            }
            if (!fr.can_delete_personal.value_or(false)) {
                co_return createTypedResponse(
                    BackendErrorResponse{"Deletion Denied: Deletion capability is disabled for this personal workspace.", k403Forbidden});
            }
        }
    }

    static constexpr char kStoredObjectQuery[] =
        "SELECT o.id, o.bucket, o.key FROM storage.objects o JOIN storage.file_objects fo ON fo.object_id = o.id AND fo.part_index = 0 "
        "WHERE fo.file_id = $1";
    auto file_meta = co_await tsp_db_client->execSqlCoro(kStoredObjectQuery, t_entity_id);
    if (file_meta.empty()) {
        co_return createTypedResponse(BackendErrorResponse{"File object not found", k404NotFound});
    }

    const auto object = sgrn::orm::fromRow<detail::StoredObjectRow, kStoredObjectQuery>(file_meta[0]);

    // Crypto-shred if encrypted
    static constexpr char kFileEncryptionQuery[] = "SELECT key_id, is_encrypted FROM storage.files WHERE id = $1";
    auto enc_rows = co_await tsp_db_client->execSqlCoro(kFileEncryptionQuery, t_entity_id);
    if (!enc_rows.empty()) {
        const auto encryption = sgrn::orm::fromRow<detail::FileEncryptionRow, kFileEncryptionQuery>(enc_rows[0]);
        if (encryption.is_encrypted && encryption.key_id) {
            auto ks = sgrn::datastore::crypto::getKeyStore();
            if (ks) {
                (void)ks->shred(*encryption.key_id);
            }
        }
    }

    auto delete_res = co_await tsp_db_client->execSqlCoro("DELETE FROM storage.files WHERE id = $1", t_entity_id);
    if (delete_res.affectedRows() == 0) {
        co_return createTypedResponse(BackendErrorResponse{"File deletion failed", k404NotFound});
    }

    // Check if the physical object is still referenced by other file records
    auto obj_ref_res = co_await tsp_db_client->execSqlCoro("SELECT 1 FROM storage.file_objects WHERE object_id = $1 LIMIT 1", object.id);
    if (obj_ref_res.empty()) {
        // No more references — delete physical file from MinIO first, then remove the DB row.
        // Order matters: if MinIO delete fails we must not remove the DB record, otherwise
        // the object becomes unreachable and leaks in MinIO forever.
        auto s3 = drogon::app().getPlugin<sgrn::datastore::plugins::aws::S3Client>();
        if (!s3) {
            co_return createTypedResponse(BackendErrorResponse{"Storage backend unavailable", drogon::k503ServiceUnavailable});
        }
        auto s3_res = co_await s3->deleteFile(object.bucket, object.key);
        if (!s3_res.has_value()) {
            ERROR_LOG("Failed to delete physical object {}/{} from MinIO: {}", object.bucket, object.key, s3_res.error().message);
            co_return createTypedResponse(
                BackendErrorResponse{std::format("Storage deletion failed: {}", s3_res.error().message), drogon::k500InternalServerError});
        }
        co_await tsp_db_client->execSqlCoro("DELETE FROM storage.objects WHERE id = $1", object.id);
    }

    co_return createTypedResponse(detail::DeleteResponse{t_entity_id, "file"}, k200OK);
}

Task<HttpResponsePtr> StorageApiHandler::deleteFolder(
    const drogon::orm::DbClientPtr& tsp_db_client, int64_t t_entity_id, int32_t t_user_id, bool t_is_admin) {

    // Single query: ownership + permission in one pass (same pattern as deleteFile).
    static constexpr char kDirectoryDeletePermissionQuery[] = "SELECT d.id, d.user_id, d.domain, "
                                                              "       udp.can_delete  AS domain_can_delete, "
                                                              "       u.can_delete_personal "
                                                              "FROM storage.directories d "
                                                              "LEFT JOIN core.user_domain_permissions udp "
                                                              "       ON udp.user_id = $2 AND udp.domain = d.domain "
                                                              "LEFT JOIN core.users u ON u.id = $2 "
                                                              "WHERE d.id = $1";
    auto dir_res = co_await tsp_db_client->execSqlCoro(kDirectoryDeletePermissionQuery, t_entity_id, t_user_id);

    if (dir_res.empty()) {
        co_return createTypedResponse(BackendErrorResponse{"Folder not found or access denied", k404NotFound});
    }

    const auto dr = sgrn::orm::fromRow<detail::DeletePermissionRow, kDirectoryDeletePermissionQuery>(dir_res[0]);
    const std::string domain_name = dr.domain.value_or("");

    if (!t_is_admin) {
        if (!domain_name.empty()) {
            if (!dr.domain_can_delete.value_or(false)) {
                co_return createTypedResponse(BackendErrorResponse{
                    "Folder Deletion Denied: Delete capability is not granted for this operational domain.", k403Forbidden});
            }
        } else {
            if (!dr.user_id || *dr.user_id != t_user_id) {
                co_return createTypedResponse(BackendErrorResponse{"Folder not found or access denied", k404NotFound});
            }
            if (!dr.can_delete_personal.value_or(false)) {
                co_return createTypedResponse(BackendErrorResponse{
                    "Folder Deletion Denied: Deletion capability is disabled for this personal workspace.", k403Forbidden});
            }
        }
    }

    // NOTE: sub-files and sub-directories are removed by ON DELETE CASCADE.
    auto delete_res = co_await tsp_db_client->execSqlCoro("DELETE FROM storage.directories WHERE id = $1", t_entity_id);
    if (delete_res.affectedRows() == 0) {
        co_return createTypedResponse(BackendErrorResponse{"Folder deletion failed", k404NotFound});
    }

    co_return createTypedResponse(detail::DeleteResponse{t_entity_id, "folder"}, k200OK);
}

Task<HttpResponsePtr> StorageApiHandler::handleBulkAction(HttpRequestPtr tsp_req) {
    try {
        auto actor_opt = resolveDriveActor(tsp_req);
        if (!actor_opt) {
            co_return createTypedResponse(BackendErrorResponse{"No session found", k401Unauthorized});
        }

        const int32_t user_id = actor_opt->user_id;
        const bool is_admin = actor_opt->is_admin;

        auto req_res = parseTypedRequest<detail::BulkActionRequest>(tsp_req);
        if (req_res.hasError() || req_res->action.empty() || req_res->items.empty()) {
            co_return createTypedResponse(BackendErrorResponse{"Invalid JSON: action and items array required", k400BadRequest});
        }

        const auto& req = req_res.value();
        const std::string& action = req.action;
        const auto& items = req.items;

        auto db_res = storage_service_.getDbClient();
        if (!db_res.has_value())
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        auto db_client = db_res.value();

        std::vector<detail::BulkResult> results;
        results.reserve(items.size());
        int success_count = 0;

        if (action == "delete") {
            for (const auto& item : items) {
                const int64_t id = item.numeric_id();
                if (id < 0 || item.type.empty())
                    continue;
                const std::string& type = item.type;

                HttpResponsePtr res;
                if (type == "file")
                    res = co_await deleteFile(db_client, id, user_id, is_admin);
                else
                    res = co_await deleteFolder(db_client, id, user_id, is_admin);

                const bool succeeded = res->statusCode() == k200OK;
                results.push_back({id, type, succeeded, succeeded ? std::nullopt : std::optional<std::string>(res->getBody())});
                if (succeeded)
                    success_count++;
            }
        } else if (action == "move") {
            if (!req.target_parent_id.has_value()) {
                co_return createTypedResponse(BackendErrorResponse{"target_parent_id required for move action", k400BadRequest});
            }
            const auto& target_parent_id = req.target_parent_id;

            for (const auto& item : items) {
                const int64_t id = item.numeric_id();
                if (id < 0 || item.type.empty())
                    continue;
                const std::string& type = item.type;

                auto transaction = co_await db_client->newTransactionCoro();
                HttpResponsePtr res;
                if (type == "file")
                    res = co_await moveFile(transaction, id, user_id, is_admin, target_parent_id, std::nullopt);
                else
                    res = co_await moveFolder(transaction, id, user_id, is_admin, target_parent_id, std::nullopt);

                const bool succeeded = res->statusCode() == k200OK;
                results.push_back({id, type, succeeded, succeeded ? std::nullopt : std::optional<std::string>(res->getBody())});
                if (succeeded)
                    success_count++;
            }
        } else {
            co_return createTypedResponse(BackendErrorResponse{"Unsupported action", k400BadRequest});
        }

        co_return createTypedResponse(
            tsp_req, detail::BulkResponse{action, static_cast<int32_t>(items.size()), success_count, std::move(results)}, k200OK);

    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Bulk action handler DB exception: {}", e.base().what());
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    } catch (const std::exception& ex) {
        ERROR_LOG("Bulk action handler exception: {}", ex.what());
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    }
}

} // namespace sgrn::datastore::handlers::storage

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
