#include <sgrn/datastore/handlers/storage.hpp>
#include <sgrn/datastore/services/proxy.hpp>
#include <sgrn/datastore/services/storage.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe_access.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/hashing.hpp>
#include <sgrn/utils/strings.hpp>
#include <json/value.h>

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

namespace sgrn::datastore::handlers::storage
{
using namespace drogon;

using sgrn::datastore::services::storage::StorageScope;
StorageApiHandler::StorageApiHandler()
    : IHandler<StorageApiHandler>(this, kRoutes)
    , storage_service_() {
}

Task<HttpResponsePtr> StorageApiHandler::handleGetConstraints(HttpRequestPtr tsp_req) {
    co_return co_await storage_service_.handleGetConstraints();
}

Task<HttpResponsePtr> StorageApiHandler::handleCreateObject(HttpRequestPtr tsp_req) {
    auto json = tsp_req->getJsonObject();
    if (!json) {
        co_return sgrn::createJsonErrorResponse("Invalid JSON", k400BadRequest);
    }
    // The filter has already validated the automated service and injected its ID into attributes.
    (*json)["automated_service_id"] = tsp_req->getAttributes()->get<int32_t>("automated_service_id");
    co_return co_await storage_service_.handleCreateObject(std::move(*json));
}

Task<HttpResponsePtr> StorageApiHandler::handleListObjects(HttpRequestPtr tsp_req) {
    // The filter has already validated the automated service and injected its ID into attributes.
    auto automated_service_id = tsp_req->getAttributes()->get<int32_t>("automated_service_id");
    co_return co_await storage_service_.handleListObjects(automated_service_id);
}

Task<HttpResponsePtr> StorageApiHandler::handleMoveObject(HttpRequestPtr tsp_req) {
    auto session = tsp_req->getAttributes()->get<Json::Value>("session_json");
    if (!session) {
        co_return createJsonErrorResponse("No session found", k401Unauthorized);
    }

    const int32_t automated_service_id = tsp_req->getAttributes()->get<int32_t>("automated_service_id");
    const std::string name = tsp_req->getParameter("name");
    auto json = tsp_req->getJsonObject();
    if (name.empty() || !json || !json->isMember("new_name") || !(*json)["new_name"].isString()) {
        co_return createJsonErrorResponse("name and new_name are required", k400BadRequest);
    }

    co_return co_await storage_service_.handleMoveObject(std::move(session), automated_service_id, name, (*json)["new_name"].asString());
}

Task<HttpResponsePtr> StorageApiHandler::handleDeleteObject(HttpRequestPtr tsp_req) {
    auto session = tsp_req->getAttributes()->get<Json::Value>("session_json");
    if (!session) {
        co_return createJsonErrorResponse("No session found", k401Unauthorized);
    }

    const int32_t automated_service_id = tsp_req->getAttributes()->get<int32_t>("automated_service_id");
    const std::string name = tsp_req->getParameter("name");
    if (name.empty()) {
        co_return createJsonErrorResponse("name is required", k400BadRequest);
    }

    co_return co_await storage_service_.handleDeleteObject(std::move(session), automated_service_id, name);
}

// ============================================================================
// Files Metadata  (proxied through PostgREST → storage.files)
// ============================================================================

Task<HttpResponsePtr> StorageApiHandler::handleGetFilesMetadata(HttpRequestPtr tsp_req) {
    co_return co_await sgrn::datastore::services::proxy::PostgrestProxyService::proxyToPostgrest(tsp_req, "files");
}

Task<HttpResponsePtr> StorageApiHandler::handleAutomatedServiceGetFilesMetadata(HttpRequestPtr tsp_req) {
    co_return co_await sgrn::datastore::services::proxy::PostgrestProxyService::proxyToPostgrest(tsp_req, "files");
}

// ============================================================================
// File Upload / Download
// ============================================================================
static std::optional<std::string> normalizePath(std::string t_path) {
    if (t_path.empty()) {
        return "/";
    }

    // Reject directory traversal attempts
    if (t_path.find("..") != std::string::npos) {
        return std::nullopt;
    }

    // Ensure leading slash.
    if (t_path.front() != '/') {
        t_path = "/" + t_path;
    }
    // Strip trailing slash (but not if the path is just "/").
    if (t_path.length() > 1 && t_path.back() == '/') {
        t_path.pop_back();
    }
    return t_path;
}

Task<HttpResponsePtr> StorageApiHandler::handleFileRequest(HttpRequestPtr tsp_req) {
    try {
        // ── 1. Path Normalization & Security �────────────────────────
        auto path_opt = tsp_req->getOptionalParameter<std::string>("path");
        if (!path_opt.has_value()) {
            co_return sgrn::createErrorResponse({::sgrn::datastore::scope_application_logic, "Missing path parameter"}, k400BadRequest);
        }

        auto norm = normalizePath(std::move(path_opt.value()));
        if (!norm.has_value()) {
            // This catches directory traversal attempts (e.g. "../")
            co_return sgrn::createErrorResponse({::sgrn::datastore::scope_application_logic, "Invalid path"}, k400BadRequest);
        }
        std::string path_str = std::move(*norm);
        DEBUG_LOG("[StorageApiHandler::handleFileRequest] Normalized path: '{}'", path_str);

        const drogon::HttpMethod method = tsp_req->getMethod();
        std::string scope = tsp_req->getOptionalParameter<std::string>("scope").value_or("personal");

        // ── 2. Session Integrity ─────────────────────────────────────────────
        auto session = tsp_req->getAttributes()->get<Json::Value>("session_json");
        if (!session) {
            ERROR_LOG("User session not found");
            co_return createJsonErrorResponse("No session found", k401Unauthorized);
        }

        if (method == Get) {
            // ── 3. Handle Download (GET) ─────────────────────────────────────
            DEBUG_LOG("[StorageApiHandler::handleFileRequest] GET - scope: {}, path: {}", scope, path_str);
            co_return co_await storage_service_.handleDownloadFileRequest(std::move(session), std::move(scope), std::move(path_str));

        } else if (method == Post) {
            // ── 4. Handle Upload (POST) ──────────────────────────────────────
            drogon::MultiPartParser file_upload;
            if (file_upload.parse(tsp_req) == -1) {
                const std::string& ct = tsp_req->getHeader("Content-Type");
                size_t body_len = tsp_req->body().size();
                ERROR_LOG("[handleFileRequest] Failed to parse multipart request body.");
                ERROR_LOG(" - Content-Type: '{}'", ct);
                ERROR_LOG(" - Body Size: {} bytes", body_len);
                co_return createJsonErrorResponse("Failed to parse multipart request body", k400BadRequest);
            }

            // Extract metadata from form parameters if provided
            if (file_upload.getParameters().contains("scope")) {
                auto it = file_upload.getParameters().find("scope");
                if (it != file_upload.getParameters().end()) {
                    scope = it->second;
                }
            }
            if (file_upload.getParameters().contains("path")) {
                auto it = file_upload.getParameters().find("path");
                if (it != file_upload.getParameters().end()) {
                    auto norm2 = normalizePath(it->second);
                    if (norm2.has_value()) {
                        path_str = std::move(*norm2);
                    }
                }
            }

            const std::vector<HttpFile>& files = file_upload.getFiles();
            if (files.empty()) {
                co_return createJsonErrorResponse("No file parts found in multipart request", k400BadRequest);
            }

            DEBUG_LOG("[StorageApiHandler::handleFileRequest] POST - scope: {}, path: {}, count: {}", scope, path_str, files.size());

            // ── 5. Storage Service Delegation ────────────────────────────────
            if (files.size() == 1) {
                // If the single file has a slash in its name (from webkitRelativePath),
                // we join it with the path parameter to reconstruct the hierarchy.
                std::string file_rel_path = drogon::utils::urlDecode(files[0].getFileName());

                // SEC: Sanitize the relative path segment before merging with the base path
                auto safe_rel = sgrn::utils::strings::sanitizeRelativeFilename(file_rel_path);
                if (!safe_rel.has_value()) {
                    co_return createJsonErrorResponse("Invalid filename: path traversal detected", k400BadRequest);
                }
                file_rel_path = std::move(*safe_rel);

                std::string target_path = path_str;
                if (file_rel_path.find('/') != std::string::npos) {
                    if (target_path.back() != '/' && file_rel_path.front() != '/') {
                        target_path += "/";
                    }
                    target_path += file_rel_path;
                    DEBUG_LOG("[StorageApiHandler::handleFileRequest] Hierarchy detected, target path: '{}'", target_path);
                }

                co_return co_await storage_service_.handleUploadFileRequest(
                    std::move(session), std::move(scope), std::move(target_path), files[0]);
            } else {
                // Batch upload optimization
                co_return co_await storage_service_.handleUploadFilesBatchRequest(
                    std::move(session), std::move(scope), std::move(path_str), files);
            }
        }

        co_return createJsonErrorResponse("Method not allowed", k405MethodNotAllowed);

    } catch (const std::exception& ex) {
        ERROR_LOG("Handler exception: {}", ex.what());
        co_return createJsonErrorResponse(std::format("Internal error: {}", ex.what()), k500InternalServerError);
    }
}

// ============================================================================
// Drive Directory Listing
//
// Uses the materialized `storage.files.full_path` column. The compatibility
// view still exists, but the hot read path no longer needs a directory join.
//
// Schema changes vs old version:
//   - Table was storage.user_files → now via storage.files (and the
//     storage.file_paths compatibility view)
//   - `uf.path`   → `fp.full_path`  (materialized on storage.files)
//   - `uf.status` → removed (no status column on storage.files)
//   - Ownership filter uses fp.user_id / fp.automated_service_id directly
//     (no join to core.sessions)
// ============================================================================

Task<HttpResponsePtr> StorageApiHandler::handleDriveList(HttpRequestPtr tsp_req) {
    try {
        // 1. Session Validation
        auto session = tsp_req->getAttributes()->get<Json::Value>("session_json");
        if (!session || !session.isMember("user")) {
            co_return createJsonErrorResponse("No valid session found", k401Unauthorized);
        }

        if (!session["user"].isMember("id") || !session["user"]["id"].isInt()) {
            co_return createJsonErrorResponse("Invalid session: user.id missing or not an integer", k401Unauthorized);
        }
        if (!session["user"].isMember("role") || !session["user"]["role"].isMember("name") || !session["user"]["role"]["name"].isString()) {
            co_return createJsonErrorResponse("Invalid session: user.role.name missing or not a string", k401Unauthorized);
        }

        const int32_t user_id = session["user"]["id"].asInt();
        const std::string role = session["user"]["role"]["name"].asString();
        const bool is_admin = (role == "admin");

        if (!is_admin && !session["session_id"].isInt()) {
            co_return createJsonErrorResponse("Invalid session: session_id missing or not an integer", k401Unauthorized);
        }

        // 2. Scope and Path Resolution
        std::string scope_str = tsp_req->getOptionalParameter<std::string>("scope").value_or("personal");
        auto path_param = tsp_req->getOptionalParameter<std::string>("path");
        std::string req_path = path_param.value_or("/");

        StorageScope scope = storage_service_.parseScope(scope_str);

        auto scope_res = co_await storage_service_.resolveScopeSession(session, scope, req_path);

        if (!scope_res.has_value()) {
            co_return sgrn::createJsonResponse(scope_res);
        }

        if (!scope_res->can_read) {
            co_return createJsonErrorResponse("Access Denied: Read capability is not granted for this scope.", drogon::k403Forbidden);
        }

        const int32_t target_owner_id = scope_res->owner_id;
        std::string current_path = scope_res->actual_path;
        bool is_virtual_root = scope_res->is_virtual_root;

        // 3. Namespace Prefix Reconstruction
        std::string namespace_prefix = "";
        if (!is_virtual_root && scope != StorageScope::Personal) {
            std::string path_no_slash = (req_path.front() == '/') ? req_path.substr(1) : req_path;
            if (scope == StorageScope::Domain) {
                if (path_no_slash.rfind("domains/", 0) == 0) {
                    std::string rem = path_no_slash.substr(8);
                    std::string::size_type next_slash = rem.find('/');
                    std::string domain_name = (next_slash == std::string::npos) ? rem : rem.substr(0, next_slash);
                    namespace_prefix = "/domains/" + domain_name;
                } else {
                    std::string::size_type next_slash = path_no_slash.find('/');
                    std::string domain_name = (next_slash == std::string::npos) ? path_no_slash : path_no_slash.substr(0, next_slash);
                    namespace_prefix = "/domains/" + domain_name;
                }
            } else {
                auto first_slash_pos = path_no_slash.find('/');
                if (first_slash_pos == std::string::npos) {
                    namespace_prefix = "/" + path_no_slash;
                } else {
                    namespace_prefix = "/" + path_no_slash.substr(0, first_slash_pos);
                }
            }
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (!db_res) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db_client = db_res.value();

        Json::Value folders_array(Json::arrayValue);
        Json::Value files_array(Json::arrayValue);
        Json::Value trail_array(Json::arrayValue);

        // 4. Trail Construction
        std::string target_domain = "";
        if (scope == StorageScope::Domain) {
            std::string path_no_slash = (req_path.front() == '/') ? req_path.substr(1) : req_path;
            if (path_no_slash.rfind("domains/", 0) == 0) {
                std::string rem = path_no_slash.substr(8);
                std::string::size_type next_slash = rem.find('/');
                target_domain = (next_slash == std::string::npos) ? rem : rem.substr(0, next_slash);
            } else {
                std::string::size_type next_slash = path_no_slash.find('/');
                target_domain = (next_slash == std::string::npos) ? path_no_slash : path_no_slash.substr(0, next_slash);
            }
        }

        auto lookup_directory = [sp_db_client = db_client, user_id, target_owner_id, scope, target_domain](
                                    const std::string& t_path) -> Task<std::optional<drogon::orm::Result>> {
            if (scope == StorageScope::Domain) {
                auto res = co_await sp_db_client->execSqlCoro(
                    "SELECT d.id, d.name, d.path FROM storage.directories d WHERE d.domain = $1 AND d.path = $2", target_domain, t_path);
                if (res.empty()) {
                    co_return std::nullopt;
                }
                co_return res;
            }

            const int32_t owner_id = (scope == StorageScope::Personal) ? user_id : target_owner_id;
            if (scope == StorageScope::Personal || scope == StorageScope::Users) {
                auto res = co_await sp_db_client->execSqlCoro(
                    "SELECT d.id, d.name, d.path FROM storage.directories d WHERE d.user_id = $1 AND d.path = $2", owner_id, t_path);
                if (res.empty()) {
                    co_return std::nullopt;
                }
                co_return res;
            }

            auto res = co_await sp_db_client->execSqlCoro(
                "SELECT d.id, d.name, d.path FROM storage.directories d WHERE d.automated_service_id = $1 AND d.path = $2", target_owner_id,
                t_path);
            if (res.empty()) {
                co_return std::nullopt;
            }
            co_return res;
        };

        auto append_trail_node = [&trail_array](std::string t_path, std::string t_name, std::optional<int64_t> t_id = std::nullopt,
                                     std::optional<std::string> t_display_name = std::nullopt) {
            Json::Value node;
            node["path"] = std::move(t_path);
            node["name"] = std::move(t_name);
            if (t_id.has_value()) {
                node["id"] = Json::Int64(*t_id);
            } else {
                node["id"] = Json::Value();
            }
            if (t_display_name.has_value()) {
                node["display_name"] = std::move(*t_display_name);
            }
            trail_array.append(std::move(node));
        };

        if (!is_virtual_root) {
            if (scope != StorageScope::Personal) {
                append_trail_node(namespace_prefix, namespace_prefix.length() > 1 ? namespace_prefix.substr(1) : namespace_prefix,
                    std::nullopt, scope_res->display_name.empty() ? std::nullopt : std::optional<std::string>(scope_res->display_name));
            }

            if (current_path != "/") {
                std::string current_relative;
                std::string path_no_slash = (current_path.front() == '/') ? current_path.substr(1) : current_path;

                std::size_t start = 0;
                while (start < path_no_slash.size()) {
                    auto slash_pos = path_no_slash.find('/', start);
                    std::string segment =
                        (slash_pos == std::string::npos) ? path_no_slash.substr(start) : path_no_slash.substr(start, slash_pos - start);
                    if (segment.empty()) {
                        break;
                    }

                    current_relative += "/" + segment;
                    std::string display_path = namespace_prefix.empty() ? current_relative : namespace_prefix + current_relative;
                    auto dir_lookup = co_await lookup_directory(current_relative);
                    if (dir_lookup.has_value() && !dir_lookup->empty()) {
                        const auto& row = (*dir_lookup)[0];
                        append_trail_node(display_path, row["name"].as<std::string>(), row["id"].as<int64_t>());
                    } else {
                        append_trail_node(display_path, segment);
                    }

                    if (slash_pos == std::string::npos) {
                        break;
                    }
                    start = slash_pos + 1;
                }
            }
        }

        // 5. Pagination & Search Parameters
        int32_t limit = tsp_req->getOptionalParameter<int32_t>("limit").value_or(20);
        int32_t page = tsp_req->getOptionalParameter<int32_t>("page").value_or(1);
        std::string search = tsp_req->getOptionalParameter<std::string>("search").value_or("");

        if (limit < 1)
            limit = 20;
        if (page < 1)
            page = 1;

        int32_t total_folders = 0;
        int32_t total_files = 0;

        // 6. Build Content Listing (Virtual Root vs Normal Drive)
        if (is_virtual_root) {
            const std::string& organisation = session["user"]["organisation"].asString();

            BackendResult<void> vrl_res = co_await buildVirtualRootListing(folders_array, namespace_prefix, scope, organisation, db_client);
            if (vrl_res.hasError()) {
                co_return createJsonErrorResponse(
                    std::format("Failed to list virtual root: {}", vrl_res.error().message_), drogon::k500InternalServerError);
            }

            total_folders = folders_array.size();
        } else {
            auto stats = co_await buildNormalDriveListing(folders_array, files_array, namespace_prefix, current_path, user_id,
                target_owner_id, scope, db_client, limit, page, search);
            total_folders = stats.first;
            total_files = stats.second;
        }

        // 7. Response Construction
        std::string display_path = current_path;
        if (display_path.length() > 1 && display_path.back() == '/') {
            display_path.pop_back();
        }

        Json::Value response;
        if (display_path == "/" && !namespace_prefix.empty()) {

            response["path"] = namespace_prefix;
        } else {
            response["path"] = namespace_prefix + display_path;
        }
        response["trail"] = std::move(trail_array);
        response["folders"] = std::move(folders_array);
        response["files"] = std::move(files_array);

        Json::Value capabilities(Json::objectValue);
        capabilities["can_read"] = scope_res->can_read;
        capabilities["can_write"] = scope_res->can_write;
        capabilities["can_delete"] = scope_res->can_delete;
        capabilities["allowed_subpath"] = scope_res->allowed_subpath;
        response["capabilities"] = std::move(capabilities);

        // Pagination Metadata
        response["total_folders"] = total_folders;
        response["total_files"] = total_files;
        response["total_items"] = total_folders + total_files;
        response["page"] = page;
        response["page_size"] = limit;
        response["total_pages"] = std::max(1, (int32_t)std::ceil((double)(total_folders + total_files) / limit));

        co_return drogon::HttpResponse::newHttpJsonResponse(std::move(response));
    } catch (const std::exception& ex) {
        ERROR_LOG("Drive list handler exception: {}", ex.what());
        co_return createJsonErrorResponse(std::format("Internal error: {}", ex.what()), k500InternalServerError);
    }
}

Task<BackendResult<void>> StorageApiHandler::buildVirtualRootListing(Json::Value& t_folders_array, const std::string& t_namespace_prefix,
    StorageScope t_scope, const std::string& t_organisation, const drogon::orm::DbClientPtr& tsp_db_client) {
    // Admin namespace roots are synthetic:
    // - / in "users" lists all users in the organisation
    // - / in "automated-services" lists all service tokens in the organisation

    try {
        drogon::orm::Result virtual_res =
            (t_scope == StorageScope::AutomatedServices)
                ? co_await tsp_db_client->execSqlCoro(
                      "SELECT token::text, name AS display_name, metadata FROM core.automated_services WHERE "
                      "organisation = $1 AND deleted_at IS NULL ORDER BY token",
                      t_organisation)
            : (t_scope == StorageScope::Domain)
                ? co_await tsp_db_client->execSqlCoro("SELECT name FROM core.domains WHERE organisation = $1 ORDER BY name", t_organisation)
                : co_await tsp_db_client->execSqlCoro(
                      "SELECT email AS name FROM core.users WHERE organisation = $1 AND deleted_at IS NULL ORDER BY email", t_organisation);

        for (const auto& row : virtual_res) {
            Json::Value folder_item;
            if (t_scope == StorageScope::AutomatedServices) {
                folder_item["name"] = row["token"].as<std::string>();
                folder_item["display_name"] = row["display_name"].as<std::string>();
            } else if (t_scope == StorageScope::Domain) {
                folder_item["name"] = row["name"].as<std::string>();
            } else {
                folder_item["name"] = row["name"].as<std::string>();
            }
            folder_item["path"] = t_namespace_prefix + "/" + folder_item["name"].asString();
            t_folders_array.append(std::move(folder_item));
        }
        co_return {};

    } catch (const std::exception& ex) {
        co_return BackendError{scope_database, std::string("buildVirtualRootListing failed: ") + ex.what()};
    }
}

Task<std::pair<int32_t, int32_t>> StorageApiHandler::buildNormalDriveListing(Json::Value& t_folders_array, Json::Value& t_files_array,
    const std::string& t_namespace_prefix, const std::string& t_current_path_in, int32_t t_user_id, int32_t t_target_owner_id,
    StorageScope t_scope, const drogon::orm::DbClientPtr& tsp_db_client, int32_t t_limit, int32_t t_page, const std::string& t_search) {

    std::string current_path = t_current_path_in;
    if (current_path.empty() || current_path.front() != '/') {
        current_path = "/" + current_path;
    }
    if (current_path.back() != '/') {
        current_path += '/';
    }

    const bool is_domain = (t_scope == StorageScope::Domain);
    const bool is_automated = (t_scope == StorageScope::AutomatedServices);
    const std::string owner_col = is_domain ? "domain" : (is_automated ? "automated_service_id" : "user_id");
    const int32_t offset = (t_page - 1) * t_limit;

    std::string target_domain = "";
    if (is_domain) {
        std::string path_no_slash = (t_namespace_prefix.front() == '/') ? t_namespace_prefix.substr(1) : t_namespace_prefix;
        if (path_no_slash.rfind("domains/", 0) == 0) {
            std::string rem = path_no_slash.substr(8);
            std::string::size_type next_slash = rem.find('/');
            target_domain = (next_slash == std::string::npos) ? rem : rem.substr(0, next_slash);
        } else {
            std::string::size_type next_slash = path_no_slash.find('/');
            target_domain = (next_slash == std::string::npos) ? path_no_slash : path_no_slash.substr(0, next_slash);
        }
    }

    const int32_t owner_id = (t_scope == StorageScope::Personal) ? t_user_id : t_target_owner_id;
    std::string path_for_parent_lookup = (current_path.length() > 1 ? current_path.substr(0, current_path.length() - 1) : "/");
    std::string search_pattern = t_search.empty() ? "" : "%" + t_search + "%";
    std::string recursive_pattern = current_path + "%";

    // ── 1. Count Totals ──────────────────────────────────────────────────────
    int32_t total_folders = 0;
    int32_t total_files = 0;

    if (t_search.empty()) {
        if (is_domain) {
            drogon::orm::Result count_res = co_await tsp_db_client->execSqlCoro(
                "SELECT "
                "  (SELECT count(*) FROM storage.directories WHERE domain = $1 AND ((parent_id IS NULL AND $2 = '/') OR (parent_id IN "
                "(SELECT id FROM storage.directories WHERE domain = $1 AND path = $3)))) as folders, "
                "  (SELECT count(*) FROM storage.files WHERE domain = $1 AND ((directory_id IS NULL AND $2 = '/') OR (directory_id IN "
                "(SELECT id FROM storage.directories WHERE domain = $1 AND path = $3)))) as files",
                target_domain, current_path, path_for_parent_lookup);
            total_folders = static_cast<int32_t>(count_res[0]["folders"].as<int64_t>());
            total_files = static_cast<int32_t>(count_res[0]["files"].as<int64_t>());
        } else {
            drogon::orm::Result count_res = co_await tsp_db_client->execSqlCoro(
                "SELECT "
                "  (SELECT count(*) FROM storage.directories WHERE " +
                    owner_col + " = $1 AND ((parent_id IS NULL AND $2 = '/') OR (parent_id IN (SELECT id FROM storage.directories WHERE " +
                    owner_col +
                    " = $1 AND path = $3)))) as folders, "
                    "  (SELECT count(*) FROM storage.files WHERE " +
                    owner_col +
                    " = $1 AND ((directory_id IS NULL AND $2 = '/') OR (directory_id IN (SELECT id FROM storage.directories WHERE " +
                    owner_col + " = $1 AND path = $3)))) as files",
                owner_id, current_path, path_for_parent_lookup);
            total_folders = static_cast<int32_t>(count_res[0]["folders"].as<int64_t>());
            total_files = static_cast<int32_t>(count_res[0]["files"].as<int64_t>());
        }
    } else {
        if (is_domain) {
            drogon::orm::Result count_res = co_await tsp_db_client->execSqlCoro(
                "SELECT "
                "  (SELECT count(*) FROM storage.directories WHERE domain = $1 AND name ILIKE $2 AND path LIKE $3) as folders, "
                "  (SELECT count(*) FROM storage.files WHERE domain = $1 AND name ILIKE $2 AND full_path LIKE $3) as files",
                target_domain, search_pattern, recursive_pattern);
            total_folders = static_cast<int32_t>(count_res[0]["folders"].as<int64_t>());
            total_files = static_cast<int32_t>(count_res[0]["files"].as<int64_t>());
        } else {
            drogon::orm::Result count_res =
                co_await tsp_db_client->execSqlCoro("SELECT "
                                                    "  (SELECT count(*) FROM storage.directories WHERE " +
                                                        owner_col +
                                                        " = $1 AND name ILIKE $2 AND path LIKE $3) as folders, "
                                                        "  (SELECT count(*) FROM storage.files WHERE " +
                                                        owner_col + " = $1 AND name ILIKE $2 AND full_path LIKE $3) as files",
                    owner_id, search_pattern, recursive_pattern);
            total_folders = static_cast<int32_t>(count_res[0]["folders"].as<int64_t>());
            total_files = static_cast<int32_t>(count_res[0]["files"].as<int64_t>());
        }
    }

    // ── 2. Fetch Folders ─────────────────────────────────────────────────────
    auto folders_res = co_await [&]() -> Task<drogon::orm::Result> {
        if (offset >= total_folders) {
            co_return co_await tsp_db_client->execSqlCoro("SELECT 1 WHERE 1=0");
        }
        int32_t folder_limit = std::min(t_limit, total_folders - offset);
        if (t_search.empty()) {
            if (is_domain) {
                co_return co_await tsp_db_client->execSqlCoro(
                    "SELECT id, name, created_at, virtual_size, real_size, count_sub_files, count_sub_directories, path "
                    "FROM storage.directories WHERE domain = $1 AND "
                    "((parent_id IS NULL AND $2 = '/') OR (parent_id IN (SELECT id FROM storage.directories WHERE domain = $1 AND path = "
                    "$3))) "
                    "ORDER BY name LIMIT " +
                        std::to_string(folder_limit) + " OFFSET " + std::to_string(offset),
                    target_domain, current_path, path_for_parent_lookup);
            } else {
                co_return co_await tsp_db_client->execSqlCoro(
                    "SELECT id, name, created_at, virtual_size, real_size, count_sub_files, count_sub_directories, path "
                    "FROM storage.directories WHERE " +
                        owner_col +
                        " = $1 AND "
                        "((parent_id IS NULL AND $2 = '/') OR (parent_id IN (SELECT id FROM storage.directories WHERE " +
                        owner_col +
                        " = $1 AND path = $3))) "
                        "ORDER BY name LIMIT " +
                        std::to_string(folder_limit) + " OFFSET " + std::to_string(offset),
                    owner_id, current_path, path_for_parent_lookup);
            }
        } else {
            if (is_domain) {
                co_return co_await tsp_db_client->execSqlCoro(
                    "SELECT id, name, created_at, virtual_size, real_size, count_sub_files, count_sub_directories, path "
                    "FROM storage.directories WHERE domain = $1 AND name ILIKE $2 AND path LIKE $3 "
                    "ORDER BY name LIMIT " +
                        std::to_string(folder_limit) + " OFFSET " + std::to_string(offset),
                    target_domain, search_pattern, recursive_pattern);
            } else {
                co_return co_await tsp_db_client->execSqlCoro(
                    "SELECT id, name, created_at, virtual_size, real_size, count_sub_files, count_sub_directories, path "
                    "FROM storage.directories WHERE " +
                        owner_col +
                        " = $1 AND name ILIKE $2 AND path LIKE $3 "
                        "ORDER BY name LIMIT " +
                        std::to_string(folder_limit) + " OFFSET " + std::to_string(offset),
                    owner_id, search_pattern, recursive_pattern);
            }
        }
    }();

    for (const auto& row : folders_res) {
        Json::Value folder_item;
        folder_item["id"] = row["id"].as<int64_t>();
        folder_item["name"] = row["name"].as<std::string>();
        folder_item["path"] = t_namespace_prefix + row["path"].as<std::string>();
        folder_item["created_at"] = row["created_at"].as<std::string>();
        folder_item["virtual_size"] = Json::Int64(row["virtual_size"].as<int64_t>());
        folder_item["real_size"] = Json::Int64(row["real_size"].as<int64_t>());
        folder_item["count_sub_files"] = row["count_sub_files"].as<int64_t>();
        folder_item["count_sub_directories"] = row["count_sub_directories"].as<int64_t>();
        t_folders_array.append(std::move(folder_item));
    }

    // ── 3. Fetch Files ───────────────────────────────────────────────────────
    int32_t folders_on_page = t_folders_array.size();
    int32_t remaining_limit = t_limit - folders_on_page;

    if (remaining_limit > 0) {
        int32_t file_offset = std::max(0, offset - total_folders);
        auto files_res = co_await [&]() -> Task<drogon::orm::Result> {
            if (t_search.empty()) {
                if (is_domain) {
                    co_return co_await tsp_db_client->execSqlCoro(
                        "SELECT f.id, f.name, f.full_path, f.extension, f.created_at, so.size AS compressed_size, so.original_size "
                        "FROM storage.files f JOIN storage.objects so ON so.id = f.object_id WHERE f.domain = $1 AND "
                        "((f.directory_id IS NULL AND $2 = '/') OR (f.directory_id IN (SELECT id FROM storage.directories WHERE domain = "
                        "$1 AND path = $3))) "
                        "ORDER BY f.name LIMIT " +
                            std::to_string(remaining_limit) + " OFFSET " + std::to_string(file_offset),
                        target_domain, current_path, path_for_parent_lookup);
                } else {
                    co_return co_await tsp_db_client->execSqlCoro(
                        "SELECT f.id, f.name, f.full_path, f.extension, f.created_at, so.size AS compressed_size, so.original_size "
                        "FROM storage.files f JOIN storage.objects so ON so.id = f.object_id WHERE f." +
                            owner_col +
                            " = $1 AND "
                            "((f.directory_id IS NULL AND $2 = '/') OR (f.directory_id IN (SELECT id FROM storage.directories WHERE " +
                            owner_col +
                            " = $1 AND path = $3))) "
                            "ORDER BY f.name LIMIT " +
                            std::to_string(remaining_limit) + " OFFSET " + std::to_string(file_offset),
                        owner_id, current_path, path_for_parent_lookup);
                }
            } else {
                if (is_domain) {
                    co_return co_await tsp_db_client->execSqlCoro(
                        "SELECT f.id, f.name, f.full_path, f.extension, f.created_at, so.size AS compressed_size, so.original_size "
                        "FROM storage.files f JOIN storage.objects so ON so.id = f.object_id WHERE f.domain = $1 AND "
                        "f.name ILIKE $2 AND f.full_path LIKE $3 "
                        "ORDER BY f.name LIMIT " +
                            std::to_string(remaining_limit) + " OFFSET " + std::to_string(file_offset),
                        target_domain, search_pattern, recursive_pattern);
                } else {
                    co_return co_await tsp_db_client->execSqlCoro(
                        "SELECT f.id, f.name, f.full_path, f.extension, f.created_at, so.size AS compressed_size, so.original_size "
                        "FROM storage.files f JOIN storage.objects so ON so.id = f.object_id WHERE f." +
                            owner_col +
                            " = $1 AND "
                            "f.name ILIKE $2 AND f.full_path LIKE $3 "
                            "ORDER BY f.name LIMIT " +
                            std::to_string(remaining_limit) + " OFFSET " + std::to_string(file_offset),
                        owner_id, search_pattern, recursive_pattern);
                }
            }
        }();

        for (const auto& row : files_res) {
            Json::Value file_item;
            file_item["id"] = row["id"].as<int64_t>();
            file_item["name"] = row["name"].as<std::string>();
            file_item["path"] = t_namespace_prefix + row["full_path"].as<std::string>();
            file_item["extension"] = row["extension"].isNull() ? "" : row["extension"].as<std::string>();
            file_item["size"] = Json::Int64(row["compressed_size"].as<int64_t>());
            file_item["original_size"] = Json::Int64(row["original_size"].as<int64_t>());
            file_item["created_at"] = row["created_at"].as<std::string>();
            t_files_array.append(std::move(file_item));
        }
    }
    co_return {total_folders, total_files};
}

Task<HttpResponsePtr> StorageApiHandler::handleCreateDirectory(HttpRequestPtr tsp_req) {
    try {
        auto path_opt = tsp_req->getOptionalParameter<std::string>("path");
        if (!path_opt.has_value()) {
            co_return createJsonErrorResponse("Missing 'path' query parameter", k400BadRequest);
        }
        std::string path = sgrn::utils::strings::trim(std::move(*path_opt));
        if (path.empty()) {
            co_return createJsonErrorResponse("Path parameter cannot be empty", k400BadRequest);
        }

        std::string scope = tsp_req->getOptionalParameter<std::string>("scope").value_or("personal");

        auto session = tsp_req->getAttributes()->get<Json::Value>("session_json");
        if (!session) {
            co_return createJsonErrorResponse("No session found", k401Unauthorized);
        }

        co_return co_await storage_service_.handleCreateDirectoryRequest(std::move(session), std::move(scope), std::move(path));

    } catch (const std::exception& ex) {
        ERROR_LOG("Create directory handler exception: {}", ex.what());
        co_return createJsonErrorResponse(std::format("Internal error: {}", ex.what()), k500InternalServerError);
    }
}

Task<HttpResponsePtr> StorageApiHandler::handleAutomatedServiceFileRequest(HttpRequestPtr tsp_req) {
    try {
        auto norm = normalizePath(tsp_req->getOptionalParameter<std::string>("path").value_or(""));
        if (!norm.has_value()) {
            co_return sgrn::createErrorResponse({::sgrn::datastore::scope_application_logic, "Invalid path"}, k400BadRequest);
        }
        std::string path_str = std::move(*norm);
        const HttpMethod method = tsp_req->getMethod();

        // Automated services don't supply generic session_json the same way users do, but let's see.
        const auto session = tsp_req->getAttributes()->get<Json::Value>("session_json");
        if (!session) {
            co_return createJsonErrorResponse("No session found", k401Unauthorized);
        }

        if (method == Get) {
            DEBUG_LOG("[StorageApiHandler::handleAutomatedServiceFileRequest] GET - path: {}", path_str);
            co_return co_await storage_service_.handleDownloadFileRequest(std::move(session), "personal", path_str);

        } else if (method == Post) {
            drogon::MultiPartParser parser;
            if (parser.parse(tsp_req) == -1) {
                const std::string& ct = tsp_req->getHeader("Content-Type");
                ERROR_LOG("[handleAutomatedServiceFileRequest] Failed to parse multipart: Content-Type='{}'", ct);
                co_return createJsonErrorResponse("Failed to parse automated service multipart request body", k400BadRequest);
            }
            const auto& files = parser.getFiles();
            if (files.empty()) {
                co_return createJsonErrorResponse("No file parts found in automated service multipart request", k400BadRequest);
            }

            if (parser.getParameters().contains("path")) {
                auto it = parser.getParameters().find("path");
                if (it != parser.getParameters().end()) {
                    auto norm2 = normalizePath(it->second);
                    if (norm2.has_value()) {
                        path_str = std::move(*norm2);
                    }
                }
            }

            DEBUG_LOG("[StorageApiHandler::handleAutomatedServiceFileRequest] POST - path: {}, count: {}", path_str, files.size());

            if (files.size() == 1) {
                // If the single file has a slash in its name (from webkitRelativePath),
                // we should join it with the path parameter.
                std::string file_rel_path = drogon::utils::urlDecode(files[0].getFileName());

                // SEC: Sanitize the relative path segment before merging with the base path
                auto safe_rel = sgrn::utils::strings::sanitizeRelativeFilename(file_rel_path);
                if (!safe_rel.has_value()) {
                    co_return createJsonErrorResponse("Invalid filename: path traversal detected", k400BadRequest);
                }
                file_rel_path = std::move(*safe_rel);

                std::string target_path = path_str;
                if (file_rel_path.find('/') != std::string::npos) {
                    if (target_path.back() != '/' && file_rel_path.front() != '/') {
                        target_path += "/";
                    }
                    target_path += file_rel_path;
                }
                co_return co_await storage_service_.handleUploadFileRequest(std::move(session), "personal", target_path, files[0]);
            } else {
                co_return co_await storage_service_.handleUploadFilesBatchRequest(std::move(session), "personal", path_str, files);
            }
        }

        co_return createJsonErrorResponse("Method not allowed", k405MethodNotAllowed);

    } catch (const std::exception& ex) {
        ERROR_LOG("AutomatedServiceApiHandler::handleFileRequest exception: {}", ex.what());
        co_return createJsonErrorResponse(fmt::format("Internal error: {}", ex.what()), k500InternalServerError);
    }
}

Task<HttpResponsePtr> StorageApiHandler::handleMove(HttpRequestPtr tsp_req) {
    try {
        // 1. Session and Authorization Check
        auto session = tsp_req->getAttributes()->get<Json::Value>("session_json");
        if (!session) {
            co_return createJsonErrorResponse("No session found", k401Unauthorized);
        }

        if (!session.isMember("user") || !session["user"].isMember("id") || !session["user"]["id"].isInt()) {
            co_return createJsonErrorResponse("Invalid session: user.id missing or not an integer", k401Unauthorized);
        }
        if (!session["user"].isMember("role") || !session["user"]["role"].isMember("name") || !session["user"]["role"]["name"].isString()) {
            co_return createJsonErrorResponse("Invalid session: user.role.name missing or not a string", k401Unauthorized);
        }

        const int32_t user_id = session["user"]["id"].asInt();
        const bool is_admin = (session["user"]["role"]["name"].asString() == "admin");

        if (!session["session_id"].isInt()) {
            co_return createJsonErrorResponse("Invalid session: session_id missing", k401Unauthorized);
        }

        // 2. Parameter Extraction
        std::string type = tsp_req->getOptionalParameter<std::string>("type").value_or("file");
        std::string id = tsp_req->getOptionalParameter<std::string>("id").value_or("");
        if (id.empty()) {
            co_return createJsonErrorResponse("Missing 'id' parameter", k400BadRequest);
        }
        if (type != "file" && type != "folder") {
            co_return createJsonErrorResponse("type must be 'file' or 'folder'", k400BadRequest);
        }

        auto json = tsp_req->getJsonObject();
        if (!json) {
            co_return createJsonErrorResponse("Invalid JSON body", k400BadRequest);
        }

        const int64_t entity_id = [&]() -> int64_t {
            try {
                return std::stoll(id);
            } catch (const std::exception& e) {
                SGRN_WARN_LOG("StorageHandler", "Failed to parse entity_id '{}': {}", id, e.what());
                return -1;
            }
        }();
        if (entity_id < 0) {
            co_return createJsonErrorResponse("id must be a valid integer", k400BadRequest);
        }

        // 3. Database Transaction Initiation
        auto db_res = storage_service_.getDbClient();
        if (!db_res.has_value()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db_client = db_res.value();
        auto transaction = co_await db_client->newTransactionCoro();
        if (!transaction) {
            co_return createJsonErrorResponse("Database transaction unavailable", k503ServiceUnavailable);
        }

        // 4. Target Resolution (Optional parent_id and new_name)
        auto read_optional_parent_id = [&]() -> std::optional<int64_t> {
            if (!json->isMember("parent_id") || (*json)["parent_id"].isNull())
                return std::nullopt;
            if (!(*json)["parent_id"].isInt64() && !(*json)["parent_id"].isInt())
                return std::nullopt;
            return (*json)["parent_id"].asInt64();
        };

        auto read_optional_name = [&]() -> std::optional<std::string> {
            if (json->isMember("new_name") && (*json)["new_name"].isString()) {
                const std::string name_value = sgrn::utils::strings::trim((*json)["new_name"].asString());
                if (!name_value.empty())
                    return name_value;
            }
            if (json->isMember("name") && (*json)["name"].isString()) {
                const std::string name_value = sgrn::utils::strings::trim((*json)["name"].asString());
                if (!name_value.empty())
                    return name_value;
            }
            return std::nullopt;
        };

        const std::optional<int64_t> target_parent_id = read_optional_parent_id();
        const std::optional<std::string> target_name = read_optional_name();

        // SEC: Validate the new name to prevent path-traversal injection.
        // Uses sgrn::utils::strings::isValidFileName() to reject names containing
        // path separators (/ or \), "..", or null bytes — ensuring the rename
        // target stays within the current directory.
        if (target_name.has_value()) {
            auto name_result = sgrn::utils::strings::isValidFileName(*target_name);
            if (name_result.hasError()) {
                co_return createJsonErrorResponse(std::format("Invalid name: {}", name_result.error()), k400BadRequest);
            }
        }

        // 5. Delegate to Specialized Helpers
        if (type == "file") {
            co_return co_await moveFile(transaction, entity_id, user_id, is_admin, target_parent_id, target_name);
        } else {
            co_return co_await moveFolder(transaction, entity_id, user_id, is_admin, target_parent_id, target_name);
        }

    } catch (const std::exception& ex) {
        ERROR_LOG("Move handler exception: {}", ex.what());
        co_return createJsonErrorResponse(std::format("Internal error: {}", ex.what()), k500InternalServerError);
    }
}

Task<HttpResponsePtr> StorageApiHandler::moveFile(const std::shared_ptr<drogon::orm::Transaction>& tsp_transaction, int64_t t_entity_id,
    int32_t t_user_id, bool t_is_admin, std::optional<int64_t> t_target_parent_id, std::optional<std::string> t_target_name) {
    // 1. Fetch current file state
    auto file_res = t_is_admin
                        ? co_await tsp_transaction->execSqlCoro(
                              "SELECT id, user_id, automated_service_id, directory_id FROM storage.files WHERE id = $1", t_entity_id)

                        : co_await tsp_transaction->execSqlCoro(
                              "SELECT id, user_id, automated_service_id, directory_id FROM storage.files WHERE id = $1 AND user_id = $2",
                              t_entity_id, t_user_id);

    if (file_res.empty()) {
        tsp_transaction->rollback();
        co_return createJsonErrorResponse("File not found or access denied", k404NotFound);
    }

    const auto& file_row = file_res[0];
    const std::optional<int32_t> file_user_id =
        file_row["user_id"].isNull() ? std::nullopt : std::optional<int32_t>(file_row["user_id"].as<int32_t>());
    const std::optional<int32_t> file_service_id =
        file_row["automated_service_id"].isNull() ? std::nullopt : std::optional<int32_t>(file_row["automated_service_id"].as<int32_t>());
    const std::optional<int64_t> current_directory_id =
        file_row["directory_id"].isNull() ? std::nullopt : std::optional<int64_t>(file_row["directory_id"].as<int64_t>());

    // 2. Short-circuit if no changes
    if (current_directory_id == t_target_parent_id && !t_target_name.has_value()) {
        tsp_transaction->rollback();
        Json::Value response;
        response["success"] = true;
        response["id"] = Json::Int64(t_entity_id);
        response["type"] = "file";
        co_return createJsonResponse(std::move(response), k200OK);
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
            co_return createJsonErrorResponse("Target directory does not exist or access denied", k403Forbidden);
        }
    }

    // 4. Perform Update
    auto update_res =
        (t_target_parent_id.has_value() && t_target_name.has_value())
            ? co_await tsp_transaction->execSqlCoro(
                  "UPDATE storage.files SET directory_id = $1, name = $2 WHERE id = $3", *t_target_parent_id, *t_target_name, t_entity_id)
            : (t_target_parent_id.has_value()
                      ? co_await tsp_transaction->execSqlCoro(
                            "UPDATE storage.files SET directory_id = $1 WHERE id = $2", *t_target_parent_id, t_entity_id)
                      : (t_target_name.has_value()
                                ? co_await tsp_transaction->execSqlCoro(
                                      "UPDATE storage.files SET directory_id = NULL, name = $1 WHERE id = $2", *t_target_name, t_entity_id)
                                : co_await tsp_transaction->execSqlCoro(
                                      "UPDATE storage.files SET directory_id = NULL WHERE id = $1", t_entity_id)));

    if (update_res.affectedRows() == 0) {
        tsp_transaction->rollback();
        co_return createJsonErrorResponse("Update failed", k500InternalServerError);
    }

    Json::Value response;
    response["success"] = true;
    response["id"] = Json::Int64(t_entity_id);
    response["type"] = "file";
    if (t_target_parent_id)
        response["parent_id"] = Json::Int64(*t_target_parent_id);
    if (t_target_name)
        response["name"] = *t_target_name;
    co_return createJsonResponse(std::move(response), k200OK);
}

Task<HttpResponsePtr> StorageApiHandler::moveFolder(const std::shared_ptr<drogon::orm::Transaction>& tsp_transaction, int64_t t_entity_id,
    int32_t t_user_id, bool t_is_admin, std::optional<int64_t> t_target_parent_id, std::optional<std::string> t_target_name) {
    // 1. Fetch current folder state
    auto dir_res =
        t_is_admin
            ? co_await tsp_transaction->execSqlCoro(
                  "SELECT id, user_id, automated_service_id, parent_id, path FROM storage.directories WHERE id = $1", t_entity_id)
            : co_await tsp_transaction->execSqlCoro(
                  "SELECT id, user_id, automated_service_id, parent_id, path FROM storage.directories WHERE id = $1 AND user_id = $2",
                  t_entity_id, t_user_id);

    if (dir_res.empty()) {
        tsp_transaction->rollback();
        co_return createJsonErrorResponse("Folder not found or access denied", k404NotFound);
    }

    const auto& dir_row = dir_res[0];
    const std::optional<int32_t> dir_user_id =
        dir_row["user_id"].isNull() ? std::nullopt : std::optional<int32_t>(dir_row["user_id"].as<int32_t>());
    const std::optional<int32_t> dir_service_id =
        dir_row["automated_service_id"].isNull() ? std::nullopt : std::optional<int32_t>(dir_row["automated_service_id"].as<int32_t>());
    const std::optional<int64_t> current_parent_id =
        dir_row["parent_id"].isNull() ? std::nullopt : std::optional<int64_t>(dir_row["parent_id"].as<int64_t>());
    const std::string current_path = dir_row["path"].as<std::string>();

    // 2. Short-circuit if no changes
    if (current_parent_id == t_target_parent_id && !t_target_name.has_value()) {
        tsp_transaction->rollback();
        Json::Value response;
        response["success"] = true;
        response["id"] = Json::Int64(t_entity_id);
        response["type"] = "folder";
        co_return createJsonResponse(std::move(response), k200OK);
    }

    // 3. Validate target and prevent cycles
    if (t_target_parent_id.has_value()) {
        if (*t_target_parent_id == t_entity_id) {
            tsp_transaction->rollback();
            co_return createJsonErrorResponse("A folder cannot be moved into itself", k400BadRequest);
        }

        std::optional<drogon::orm::Result> target_res;
        if (dir_service_id.has_value()) {
            target_res = co_await tsp_transaction->execSqlCoro(
                "SELECT id, path FROM storage.directories WHERE id = $1 AND automated_service_id = $2", *t_target_parent_id,
                *dir_service_id);
        } else {
            target_res = co_await tsp_transaction->execSqlCoro("SELECT id, path FROM storage.directories WHERE id = $1 AND user_id = $2",
                *t_target_parent_id, dir_user_id.value_or(t_user_id));
        }

        if (!target_res.has_value() || target_res->empty()) {
            tsp_transaction->rollback();
            co_return createJsonErrorResponse("Target directory does not exist or access denied", k403Forbidden);
        }

        const std::string target_path = (*target_res)[0]["path"].as<std::string>();
        const std::string descendant_prefix = current_path + "/";
        if (target_path == current_path || target_path.rfind(descendant_prefix, 0) == 0) {
            tsp_transaction->rollback();
            co_return createJsonErrorResponse("A folder cannot be moved into its own descendant", k400BadRequest);
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
        co_return createJsonErrorResponse("Update failed", k500InternalServerError);
    }

    Json::Value response;
    response["success"] = true;
    response["id"] = Json::Int64(t_entity_id);
    response["type"] = "folder";
    if (t_target_parent_id)
        response["parent_id"] = Json::Int64(*t_target_parent_id);
    if (t_target_name)
        response["name"] = *t_target_name;
    co_return createJsonResponse(std::move(response), k200OK);
}

Task<HttpResponsePtr> StorageApiHandler::handleDelete(HttpRequestPtr tsp_req) {
    try {
        auto session = tsp_req->getAttributes()->get<Json::Value>("session_json");
        if (!session) {
            co_return createJsonErrorResponse("No session found", k401Unauthorized);
        }

        std::string type = tsp_req->getOptionalParameter<std::string>("type").value_or("file");
        std::string id = tsp_req->getOptionalParameter<std::string>("id").value_or("");
        if (id.empty()) {
            co_return createJsonErrorResponse("Missing 'id' parameter", k400BadRequest);
        }
        if (type != "file" && type != "folder") {
            co_return createJsonErrorResponse("type must be 'file' or 'folder'", k400BadRequest);
        }

        if (!session.isMember("user") || !session["user"].isMember("role") || !session["user"]["role"].isMember("name") ||
            !session["user"]["role"]["name"].isString()) {
            co_return createJsonErrorResponse("Invalid session: user.role.name missing or not a string", k401Unauthorized);
        }
        if (!session.isMember("user") || !session["user"].isMember("id") || !session["user"]["id"].isInt()) {
            co_return createJsonErrorResponse("Invalid session: user.id missing or not an integer", k401Unauthorized);
        }

        const bool is_admin = (session["user"]["role"]["name"].asString() == "admin");
        if (!session["session_id"].isInt()) {
            co_return createJsonErrorResponse("Invalid session: session_id mismatch", k401Unauthorized);
        }

        const int32_t user_id = session["user"]["id"].asInt();
        const int64_t entity_id = [&]() -> int64_t {
            try {
                return std::stoll(id);
            } catch (const std::exception&) {
                return -1;
            }
        }();
        if (entity_id < 0) {
            co_return createJsonErrorResponse("id must be a valid integer", k400BadRequest);
        }

        auto db_res = storage_service_.getDbClient();
        if (!db_res.has_value()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db_client = db_res.value();

        if (type == "file") {
            co_return co_await deleteFile(db_client, entity_id, user_id, is_admin);
        } else {
            co_return co_await deleteFolder(db_client, entity_id, user_id, is_admin);
        }

        Json::Value response;
        response["success"] = true;
        response["id"] = Json::Int64(entity_id);
        response["type"] = type;
        co_return createJsonResponse(std::move(response), k200OK);

    } catch (const std::exception& ex) {
        ERROR_LOG("Delete handler exception: {}", ex.what());
        co_return createJsonErrorResponse(std::format("Internal error: {}", ex.what()), k500InternalServerError);
    }
}

Task<HttpResponsePtr> StorageApiHandler::deleteFile(
    const drogon::orm::DbClientPtr& tsp_db_client, int64_t t_entity_id, int32_t t_user_id, bool t_is_admin) {
    auto file_res = co_await tsp_db_client->execSqlCoro(
        "SELECT f.id, f.user_id, f.automated_service_id, f.domain FROM storage.files f WHERE f.id = $1", t_entity_id);

    if (file_res.empty()) {
        co_return createJsonErrorResponse("File not found or access denied", k404NotFound);
    }

    const std::string domain_name = file_res[0]["domain"].isNull() ? "" : file_res[0]["domain"].as<std::string>();

    if (!t_is_admin) {
        if (!domain_name.empty()) {
            // Shared domain space: check capability matrix
            auto perm = co_await tsp_db_client->execSqlCoro(
                "SELECT can_delete FROM core.user_domain_permissions WHERE user_id = $1 AND domain = $2", t_user_id, domain_name);
            if (perm.empty() || !perm[0]["can_delete"].as<bool>()) {
                co_return createJsonErrorResponse(
                    "Deletion Denied: Delete capability is not granted for this operational domain.", k403Forbidden);
            }
        } else {
            // Personal workspace: must be owner + check tamper-proof personal capability
            if (file_res[0]["user_id"].isNull() || file_res[0]["user_id"].as<int32_t>() != t_user_id) {
                co_return createJsonErrorResponse("File not found or access denied", k404NotFound);
            }

            auto user_res = co_await tsp_db_client->execSqlCoro("SELECT can_delete_personal FROM core.users WHERE id = $1", t_user_id);
            if (user_res.empty() || !user_res[0]["can_delete_personal"].as<bool>()) {
                co_return createJsonErrorResponse(
                    "Deletion Denied: Deletion capability is disabled for this personal workspace.", k403Forbidden);
            }
        }
    }

    auto file_meta = co_await tsp_db_client->execSqlCoro(
        "SELECT o.id, o.bucket, o.key FROM storage.objects o JOIN storage.files f ON f.object_id = o.id WHERE f.id = $1", t_entity_id);
    if (file_meta.empty()) {
        co_return createJsonErrorResponse("File object not found", k404NotFound);
    }

    const int64_t object_id = file_meta[0]["id"].as<int64_t>();
    const std::string bucket = file_meta[0]["bucket"].as<std::string>();
    const std::string key = file_meta[0]["key"].as<std::string>();

    auto delete_res = co_await tsp_db_client->execSqlCoro("DELETE FROM storage.files WHERE id = $1", t_entity_id);
    if (delete_res.affectedRows() == 0) {
        co_return createJsonErrorResponse("File deletion failed", k404NotFound);
    }

    // Check if the physical object is still referenced by other file records
    auto obj_ref_res = co_await tsp_db_client->execSqlCoro("SELECT 1 FROM storage.files WHERE object_id = $1 LIMIT 1", object_id);
    if (obj_ref_res.empty()) {
        // No more references — delete physical file from MinIO first, then remove the DB row.
        // Order matters: if MinIO delete fails we must not remove the DB record, otherwise
        // the object becomes unreachable and leaks in MinIO forever.
        auto s3 = drogon::app().getPlugin<sgrn::datastore::plugins::aws::S3Client>();
        if (!s3) {
            co_return createJsonErrorResponse("Storage backend unavailable", drogon::k503ServiceUnavailable);
        }
        auto s3_res = co_await s3->deleteFile(bucket, key);
        if (!s3_res.has_value()) {
            ERROR_LOG("Failed to delete physical object {}/{} from MinIO: {}", bucket, key, s3_res.error().message);
            co_return createJsonErrorResponse(
                std::format("Storage deletion failed: {}", s3_res.error().message_), drogon::k500InternalServerError);
        }
        co_await tsp_db_client->execSqlCoro("DELETE FROM storage.objects WHERE id = $1", object_id);
    }

    Json::Value response;
    response["success"] = true;
    response["id"] = Json::Int64(t_entity_id);
    response["type"] = "file";

    co_return createJsonResponse(std::move(response), k200OK);
}

Task<HttpResponsePtr> StorageApiHandler::deleteFolder(
    const drogon::orm::DbClientPtr& tsp_db_client, int64_t t_entity_id, int32_t t_user_id, bool t_is_admin) {
    auto dir_res = co_await tsp_db_client->execSqlCoro(
        "SELECT d.id, d.user_id, d.automated_service_id, d.domain FROM storage.directories d WHERE d.id = $1", t_entity_id);

    if (dir_res.empty()) {
        co_return createJsonErrorResponse("Folder not found or access denied", k404NotFound);
    }

    const std::string domain_name = dir_res[0]["domain"].isNull() ? "" : dir_res[0]["domain"].as<std::string>();

    if (!t_is_admin) {
        if (!domain_name.empty()) {
            // Shared domain space: check capability matrix
            auto perm = co_await tsp_db_client->execSqlCoro(
                "SELECT can_delete FROM core.user_domain_permissions WHERE user_id = $1 AND domain = $2", t_user_id, domain_name);
            if (perm.empty() || !perm[0]["can_delete"].as<bool>()) {
                co_return createJsonErrorResponse(
                    "Folder Deletion Denied: Delete capability is not granted for this operational domain.", k403Forbidden);
            }
        } else {
            // Personal workspace: must be owner + check tamper-proof personal capability
            if (dir_res[0]["user_id"].isNull() || dir_res[0]["user_id"].as<int32_t>() != t_user_id) {
                co_return createJsonErrorResponse("Folder not found or access denied", k404NotFound);
            }

            auto user_res = co_await tsp_db_client->execSqlCoro("SELECT can_delete_personal FROM core.users WHERE id = $1", t_user_id);
            if (user_res.empty() || !user_res[0]["can_delete_personal"].as<bool>()) {
                co_return createJsonErrorResponse(
                    "Folder Deletion Denied: Deletion capability is disabled for this personal workspace.", k403Forbidden);
            }
        }
    }

    // NOTE: In Postgres, if a directory is deleted, its sub-files and sub-directories are handled
    // by ON DELETE CASCADE if configured, or it will fail if not.
    auto delete_res = co_await tsp_db_client->execSqlCoro("DELETE FROM storage.directories WHERE id = $1", t_entity_id);
    if (delete_res.affectedRows() == 0) {
        co_return createJsonErrorResponse("Folder deletion failed", k404NotFound);
    }

    Json::Value response;
    response["success"] = true;
    response["id"] = Json::Int64(t_entity_id);
    response["type"] = "folder";
    co_return createJsonResponse(std::move(response), k200OK);
}

Task<HttpResponsePtr> StorageApiHandler::handleBulkAction(HttpRequestPtr tsp_req) {
    try {
        auto session = tsp_req->getAttributes()->get<Json::Value>("session_json");
        if (!session)
            co_return createJsonErrorResponse("No session found", k401Unauthorized);

        const int32_t user_id = session["user"]["id"].asInt();
        const bool is_admin = (session["user"]["role"]["name"].asString() == "admin");

        auto json = tsp_req->getJsonObject();
        if (!json || !json->isMember("action") || !json->isMember("items") || !(*json)["items"].isArray()) {
            co_return createJsonErrorResponse("Invalid JSON: action and items array required", k400BadRequest);
        }

        std::string action = (*json)["action"].asString();
        const auto& items = (*json)["items"];

        auto db_res = storage_service_.getDbClient();
        if (!db_res.has_value())
            co_return sgrn::createJsonResponse(db_res);
        auto db_client = db_res.value();

        Json::Value results(Json::arrayValue);
        int success_count = 0;

        if (action == "delete") {
            for (const auto& item : items) {
                if (!item.isMember("id") || !item.isMember("type"))
                    continue;
                int64_t id = item["id"].isString() ? std::stoll(item["id"].asString()) : item["id"].asInt64();
                std::string type = item["type"].asString();

                HttpResponsePtr res;
                if (type == "file")
                    res = co_await deleteFile(db_client, id, user_id, is_admin);
                else
                    res = co_await deleteFolder(db_client, id, user_id, is_admin);

                Json::Value res_item;
                res_item["id"] = Json::Int64(id);
                res_item["type"] = type;
                res_item["success"] = (res->statusCode() == k200OK);
                if (res->statusCode() != k200OK)
                    res_item["error"] = res->getBody();
                else
                    success_count++;
                results.append(res_item);
            }
        } else if (action == "move") {
            if (!json->isMember("target_parent_id")) {
                co_return createJsonErrorResponse("target_parent_id required for move action", k400BadRequest);
            }
            std::optional<int64_t> target_parent_id =
                (*json)["target_parent_id"].isNull() ? std::nullopt : std::optional<int64_t>((*json)["target_parent_id"].asInt64());

            for (const auto& item : items) {
                if (!item.isMember("id") || !item.isMember("type"))
                    continue;
                int64_t id = item["id"].isString() ? std::stoll(item["id"].asString()) : item["id"].asInt64();
                std::string type = item["type"].asString();

                auto transaction = co_await db_client->newTransactionCoro();
                HttpResponsePtr res;
                if (type == "file")
                    res = co_await moveFile(transaction, id, user_id, is_admin, target_parent_id, std::nullopt);
                else
                    res = co_await moveFolder(transaction, id, user_id, is_admin, target_parent_id, std::nullopt);

                Json::Value res_item;
                res_item["id"] = Json::Int64(id);
                res_item["type"] = type;
                res_item["success"] = (res->statusCode() == k200OK);
                if (res->statusCode() != k200OK)
                    res_item["error"] = res->getBody();
                else
                    success_count++;
                results.append(res_item);
            }
        } else {
            co_return createJsonErrorResponse("Unsupported action", k400BadRequest);
        }

        Json::Value final_res;
        final_res["success"] = true;
        final_res["action"] = action;
        final_res["total"] = (int)items.size();
        final_res["success_count"] = success_count;
        final_res["results"] = results;
        co_return createJsonResponse(std::move(final_res), k200OK);

    } catch (const std::exception& ex) {
        ERROR_LOG("Bulk action handler exception: {}", ex.what());
        co_return createJsonErrorResponse(std::format("Internal error: {}", ex.what()), k500InternalServerError);
    }
}

Task<HttpResponsePtr> StorageApiHandler::handleGetStorageStats(HttpRequestPtr tsp_req) {
    try {
        const auto& session = tsp_req->attributes()->get<Json::Value>("session_json");
        if (session.isNull()) {
            SGRN_WARN_LOG("handleGetStorageStats: No session found in request attributes");
            co_return createJsonErrorResponse("Unauthorized: Session missing", k401Unauthorized);
        }

        if (!session.isMember("user") || !session["user"].isMember("id") || !session["user"]["id"].isInt()) {
            SGRN_WARN_LOG("handleGetStorageStats: Malformed session - missing or invalid user ID");
            co_return createJsonErrorResponse("Forbidden: Malformed session payload", k403Forbidden);
        }

        const int32_t user_id = session["user"]["id"].asInt();

        auto db_res = sgrn::datastore::core::getDbClient();
        if (!db_res) {
            SGRN_ERROR_LOG("handleGetStorageStats: Database client unavailable");
            co_return createJsonErrorResponse("Service Unavailable: Database error", k503ServiceUnavailable);
        }
        auto sp_db_client = db_res.value();

        // Query pre-calculated stats from core.users and count files from storage.files
        try {
            auto result = co_await sp_db_client->execSqlCoro("SELECT u.total_virtual_size, u.total_real_size, u.storage_limit, "
                                                             "       (SELECT COUNT(*) FROM storage.files WHERE user_id = $1) as file_count "
                                                             "FROM core.users u "
                                                             "WHERE u.id = $1",
                user_id);

            if (result.empty()) {
                SGRN_WARN_LOG("handleGetStorageStats: User {} not found in core.users", user_id);
                Json::Value empty_stats;
                empty_stats["file_count"] = 0;
                empty_stats["total_original_bytes"] = 0;
                empty_stats["total_compressed_bytes"] = 0;
                empty_stats["storage_limit"] = Json::nullValue;
                co_return HttpResponse::newHttpJsonResponse(std::move(empty_stats));
            }

            const auto& row = result[0];
            Json::Value stats;
            stats["file_count"] = row["file_count"].as<int64_t>();
            stats["total_original_bytes"] = row["total_virtual_size"].as<int64_t>();
            stats["total_compressed_bytes"] = row["total_real_size"].as<int64_t>();

            if (row["storage_limit"].isNull()) {
                stats["storage_limit"] = Json::nullValue;
            } else {
                stats["storage_limit"] = row["storage_limit"].as<int64_t>();
            }

            DEBUG_LOG("Storage stats for user {}: files={}, raw={}, compressed={}, limit={}", user_id, stats["file_count"].asInt64(),
                stats["total_original_bytes"].asInt64(), stats["total_compressed_bytes"].asInt64(),
                stats["storage_limit"].isNull() ? "unlimited" : std::to_string(stats["storage_limit"].asInt64()));

            co_return HttpResponse::newHttpJsonResponse(std::move(stats));
        } catch (const drogon::orm::DrogonDbException& e) {
            SGRN_ERROR_LOG("handleGetStorageStats: SQL Execution Error: {}", e.base().what());
            co_return createJsonErrorResponse(std::format("Database Error: {}", e.base().what()), k500InternalServerError);
        }

    } catch (const std::exception& ex) {
        SGRN_ERROR_LOG("Storage stats handler exception: {}", ex.what());
        co_return createJsonErrorResponse(std::format("Internal error: {}", ex.what()), k500InternalServerError);
    }
}

drogon::Task<drogon::HttpResponsePtr> StorageApiHandler::handleRecursiveDownload(drogon::HttpRequestPtr tsp_req) {
    co_return createJsonErrorResponse("Recursive download (ZIP) is not yet implemented.", k501NotImplemented);
}

} // namespace sgrn::datastore::handlers::storage

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
