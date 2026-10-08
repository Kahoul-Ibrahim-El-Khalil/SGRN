#include <drogon/orm/Exception.h>
#include <fmt/core.h>
#include <sgrn/datastore/core/db.hpp>
#include <sgrn/datastore/error/ApiErrors.hpp>
#include <sgrn/datastore/handlers/storage.hpp>
#include <sgrn/datastore/services/helpers/magic.hpp>
#include <sgrn/datastore/services/storage.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe_access.hpp>
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

namespace sgrn::datastore::handlers::storage
{
using namespace drogon;

using sgrn::datastore::services::storage::StorageScope;
StorageApiHandler::StorageApiHandler()
    : IHandler<StorageApiHandler>(this, kRoutes, kItemRoutes)
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

Task<HttpResponsePtr> StorageApiHandler::handleMoveObject(HttpRequestPtr tsp_req, std::string t_name) {
    if (!tsp_req->attributes()->find("session_json") || !tsp_req->attributes()->find("automated_service_id")) {
        co_return createJsonErrorResponse("No session found", k401Unauthorized);
    }
    auto session = tsp_req->getAttributes()->get<Json::Value>("session_json");
    if (!session) {
        co_return createJsonErrorResponse("No session found", k401Unauthorized);
    }

    const int32_t automated_service_id = tsp_req->getAttributes()->get<int32_t>("automated_service_id");
    const std::string name = std::move(t_name);
    auto json = tsp_req->getJsonObject();
    if (name.empty() || !json || !json->isMember("new_name") || !(*json)["new_name"].isString()) {
        co_return createJsonErrorResponse("name and new_name are required", k400BadRequest);
    }

    co_return co_await storage_service_.handleMoveObject(std::move(session), automated_service_id, name, (*json)["new_name"].asString());
}

Task<HttpResponsePtr> StorageApiHandler::handleDeleteObject(HttpRequestPtr tsp_req, std::string t_name) {
    if (!tsp_req->attributes()->find("session_json") || !tsp_req->attributes()->find("automated_service_id")) {
        co_return createJsonErrorResponse("No session found", k401Unauthorized);
    }
    auto session = tsp_req->getAttributes()->get<Json::Value>("session_json");
    if (!session) {
        co_return createJsonErrorResponse("No session found", k401Unauthorized);
    }

    const int32_t automated_service_id = tsp_req->getAttributes()->get<int32_t>("automated_service_id");
    const std::string name = std::move(t_name);
    if (name.empty()) {
        co_return createJsonErrorResponse("name is required", k400BadRequest);
    }

    co_return co_await storage_service_.handleDeleteObject(std::move(session), automated_service_id, name);
}

// ============================================================================
// Files Metadata (direct DB query over storage.file_details)
//
// Previously proxied through PostgREST (postgrest.files view). Now served
// in-process: same tenant scoping (organisation bound server-side from the
// session, never from the query string), same wire shape as the old
// postgrest.files view (soft-deleted objects excluded). storage.files has
// no direct tenant column, so this stays a hand-written handler rather
// than a generated CRUD view.
// ============================================================================

namespace
{
// Columns exposed by the metadata endpoints, aliased to the historical
// postgrest.files wire names so existing consumers keep working.
constexpr std::string_view kFileMetadataColumns =
    "file_id AS id, file_name AS name, file_path AS full_path, directory_path, directory_id, extension, created_at, "
    "is_compressed, compression_algorithm, compression_level, session_id, user_id, automated_service_id, "
    "domain_name AS domain, organisation_name AS organisation, object_id, bucket, key, object_size AS size, "
    "object_created_at, mime_type, upload_mode, part_count, part_size_bytes, sha256";

// Wire types mirror the old postgrest.files JSON: integers as numbers,
// booleans as booleans, everything else as strings, SQL NULL as null.
// (Output column names below are the SELECT aliases.)
Json::Value fileFieldToJson(const drogon::orm::Field& t_field, bool t_as_int, bool t_as_bool) {
    if (t_field.isNull()) {
        return Json::Value::null;
    }
    try {
        if (t_as_int) {
            return Json::Int64(t_field.as<int64_t>());
        }
        if (t_as_bool) {
            return Json::Value(t_field.as<bool>());
        }
    } catch (const std::exception&) {
        // fall through to string rendering below
    }
    try {
        return Json::Value(t_field.as<std::string>());
    } catch (const std::exception&) {
        return Json::Value::null;
    }
}

Json::Value fileRowToJson(const drogon::orm::Row& t_row) {
    static const std::unordered_set<std::string> kIntColumns = {"id", "directory_id", "object_id", "session_id", "user_id",
        "automated_service_id", "size", "compression_level", "part_count", "part_size_bytes"};
    Json::Value r(Json::objectValue);
    for (std::size_t i = 0; i < t_row.size(); ++i) {
        drogon::orm::Field f = t_row[i];
        std::string col = f.name();
        r[col] = fileFieldToJson(f, kIntColumns.contains(col), col == "is_compressed");
    }
    // Add ETag derived from sha256 (S3-compatible format: sha256-<hex>)
    if (!r["sha256"].isNull() && r["sha256"].isString()) {
        r["etag"] = Json::Value("\"" + r["sha256"].asString() + "\"");
    }
    return r;
}

// Applies one whitelisted `?col=op.value` filter onto t_sql/t_binds.
// Operators mirror the generated CRUD grammar; `*` in like/ilike patterns
// is translated to SQL `%`, matching PostgREST wildcard semantics.
// Anything not allowlisted is a client error (400), never silently ignored.
bool appendFileFilter(
    std::string& t_sql, std::vector<std::string>& t_binds, const std::string& t_key, const std::string& t_raw, std::string& t_out_error) {
    auto dot = t_raw.find('.');
    if (dot == std::string::npos) {
        t_out_error = fmt::format("Unsupported filter: {}", t_key);
        return false;
    }
    const std::string op = t_raw.substr(0, dot);
    const std::string val = t_raw.substr(dot + 1);

    auto bind_cmp = [&](std::string_view t_col, std::string_view t_sql_op) {
        t_binds.push_back(val);
        t_sql += fmt::format(" AND {} {} ${}", t_col, t_sql_op, t_binds.size());
        return true;
    };
    auto bind_int = [&](std::string_view t_col) {
        int64_t v = 0;
        auto [ptr, ec] = std::from_chars(val.data(), val.data() + val.size(), v);
        if (ec != std::errc() || ptr != val.data() + val.size()) {
            t_out_error = fmt::format("Unsupported filter: {}", t_key);
            return false;
        }
        return bind_cmp(t_col, "=");
    };
    auto bind_like = [&](std::string_view t_col, std::string_view t_keyword) {
        std::string pattern = val;
        std::replace(pattern.begin(), pattern.end(), '*', '%');
        t_binds.push_back(pattern);
        t_sql += fmt::format(" AND {} {} ${}", t_col, t_keyword, t_binds.size());
        return true;
    };

    if (t_key == "user_id" && op == "eq") {
        return bind_int("user_id");
    }
    if (t_key == "session_id" && op == "eq") {
        return bind_int("session_id");
    }
    if (t_key == "directory_id" && op == "eq") {
        if (val == "null") {
            t_sql += " AND directory_id IS NULL";
            return true;
        }
        return bind_int("directory_id");
    }
    if (t_key == "directory_id" && op == "is" && val == "null") {
        t_sql += " AND directory_id IS NULL";
        return true;
    }
    if (t_key == "domain" && op == "eq") {
        return bind_cmp("domain_name", "=");
    }
    if (t_key == "extension" && op == "eq") {
        return bind_cmp("extension", "=");
    }
    if (t_key == "bucket" && op == "eq") {
        return bind_cmp("bucket", "=");
    }
    if (t_key == "name" && (op == "like" || op == "ilike")) {
        return bind_like("file_name", op == "like" ? "LIKE" : "ILIKE");
    }
    if (t_key == "full_path" && (op == "like" || op == "ilike")) {
        return bind_like("file_path", op == "like" ? "LIKE" : "ILIKE");
    }
    t_out_error = fmt::format("Unsupported filter: {}", t_key);
    return false;
}

// Parses `?order=created_at.desc` (or `.asc`, or bare column = ASC).
// Only allowlisted sort columns are accepted; anything else is a 400.
bool parseFileOrder(const std::string& t_raw, std::string& t_out_sql) {
    static const std::unordered_map<std::string, std::string> kAllowed = {
        {"id", "file_id"},
        {"name", "file_name"},
        {"full_path", "file_path"},
        {"created_at", "created_at"},
    };
    bool desc = false;
    std::string col = t_raw;
    if (t_raw.size() > 5 && t_raw.compare(t_raw.size() - 5, 5, ".desc") == 0) {
        desc = true;
        col = t_raw.substr(0, t_raw.size() - 5);
    } else if (t_raw.size() > 4 && t_raw.compare(t_raw.size() - 4, 4, ".asc") == 0) {
        col = t_raw.substr(0, t_raw.size() - 4);
    }
    auto it = kAllowed.find(col);
    if (it == kAllowed.end()) {
        return false;
    }
    t_out_sql = it->second + (desc ? " DESC" : " ASC");
    return true;
}

std::size_t parseClampedSize(const drogon::HttpRequestPtr& tsp_req, const char* tp_key, std::size_t t_default, std::size_t t_max) {
    auto opt = tsp_req->getOptionalParameter<std::string>(tp_key);
    if (!opt.has_value()) {
        return t_default;
    }
    std::size_t v = t_default;
    auto [ptr, ec] = std::from_chars(opt->data(), opt->data() + opt->size(), v);
    if (ec != std::errc()) {
        return t_default;
    }
    return std::min(v, t_max);
}

// Shared tail of both metadata endpoints: applies whitelisted `?col=op.value`
// filters, ordering and pagination onto t_sql/t_binds, then runs the query.
// t_sql already contains the tenant/service base predicate with its binds.
Task<HttpResponsePtr> runFileMetadataQuery(HttpRequestPtr tsp_req, std::string t_sql, std::vector<std::string> t_binds) {
    for (const auto& [key, raw] : tsp_req->getParameters()) {
        if (key == "order" || key == "limit" || key == "offset") {
            continue;
        }
        std::string error;
        if (!appendFileFilter(t_sql, t_binds, key, raw, error)) {
            co_return createJsonErrorResponse(error, k400BadRequest);
        }
    }

    std::string order_sql = "file_id ASC";
    if (auto order_opt = tsp_req->getOptionalParameter<std::string>("order"); order_opt.has_value()) {
        if (!parseFileOrder(*order_opt, order_sql)) {
            co_return createJsonErrorResponse("Unsupported sort column", k400BadRequest);
        }
    }
    const std::size_t limit = parseClampedSize(tsp_req, "limit", 500, 500);
    const std::size_t offset = parseClampedSize(tsp_req, "offset", 0, 1000000);

    auto db_res = sgrn::datastore::core::getDbClient();
    if (!db_res.has_value()) {
        co_return sgrn::createJsonResponse(db_res);
    }
    try {
        auto res = co_await sgrn::datastore::core::execSqlCoroVec(
            db_res.value(), fmt::format("{} ORDER BY {} LIMIT {} OFFSET {}", t_sql, order_sql, limit, offset), t_binds);
        Json::Value arr(Json::arrayValue);
        for (const auto& row : res) {
            arr.append(fileRowToJson(row));
        }
        co_return drogon::HttpResponse::newHttpJsonResponse(std::move(arr));
    } catch (const std::exception& e) {
        ERROR_LOG("Files metadata DB error: {}", e.what());
        co_return createJsonErrorResponse(fmt::format("Database error: {}", e.what()), k500InternalServerError);
    }
}
} // namespace

Task<HttpResponsePtr> StorageApiHandler::handleGetFilesMetadata(HttpRequestPtr tsp_req) {
    // find() first: attributes()->get() throws on a missing key, and an
    // exception escaping a coroutine terminates the process. The auth
    // filter always sets session_json, but never trust that blindly here.
    if (!tsp_req->attributes()->find("session_json")) {
        co_return createJsonErrorResponse("No session found", k401Unauthorized);
    }
    auto session = tsp_req->attributes()->get<Json::Value>("session_json");
    if (!session || !session.isMember("user") || !session["user"].isMember("organisation") || !session["user"]["organisation"].isString()) {
        co_return createJsonErrorResponse("No session found", k401Unauthorized);
    }
    const std::string org = session["user"]["organisation"].asString();

    // Back-compat: `?session_id=eq.current` scopes to the caller's own
    // session (previously expanded by the PostgREST proxy layer).
    if (auto sid_opt = tsp_req->getOptionalParameter<std::string>("session_id"); sid_opt.has_value() && *sid_opt == "eq.current") {
        const Json::Value& sid_node = session["session_id"];
        if (!sid_node.isInt() && !sid_node.isInt64() && !sid_node.isUInt() && !sid_node.isUInt64()) {
            co_return createJsonErrorResponse("Corrupted session: session_id missing", k500InternalServerError);
        }
        tsp_req->setParameter("session_id", fmt::format("eq.{}", sid_node.asInt64()));
    }

    // organisation_name is bound server-side from the session — the
    // client cannot scope (or escape) to another tenant's files.
    std::string sql =
        fmt::format("SELECT {} FROM storage.file_details WHERE organisation_name = $1 AND object_deleted_at IS NULL", kFileMetadataColumns);
    co_return co_await runFileMetadataQuery(tsp_req, std::move(sql), {org});
}

Task<HttpResponsePtr> StorageApiHandler::handleAutomatedServiceGetFilesMetadata(HttpRequestPtr tsp_req) {
    // The filter has already validated the automated service and injected its ID into attributes.
    int32_t automated_service_id = 0;
    try {
        automated_service_id = tsp_req->getAttributes()->get<int32_t>("automated_service_id");
    } catch (const std::exception&) {
        co_return createJsonErrorResponse("No session found", k401Unauthorized);
    }

    std::string sql = fmt::format(
        "SELECT {} FROM storage.file_details WHERE automated_service_id = $1 AND object_deleted_at IS NULL", kFileMetadataColumns);
    co_return co_await runFileMetadataQuery(tsp_req, std::move(sql), {std::to_string(automated_service_id)});
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

// Joins a sanitized multipart filename onto the target directory to form the
// full virtual file path. Downstream (resolveDirectoryPath) derives the
// parent directory from this path, so passing the bare directory here would
// resolve to the session root and strand the file there — always join.
static std::string joinUploadTargetPath(std::string t_base_dir, std::string t_rel_name) {
    if (t_base_dir.empty()) {
        t_base_dir = "/";
    }
    while (!t_rel_name.empty() && t_rel_name.front() == '/') {
        t_rel_name.erase(t_rel_name.begin());
    }
    if (!t_rel_name.empty()) {
        if (t_base_dir.back() != '/') {
            t_base_dir += "/";
        }
        t_base_dir += t_rel_name;
    }
    return t_base_dir;
}

// Resolves the full virtual file path for a single-part upload.
// Two client contracts exist:
//  - SDK/object clients (doUpload) send the FULL remote file path twice:
//    as ?path= AND as a multipart "path" field. Trust the field verbatim.
//  - The drive UI sends the target DIRECTORY as ?path= with the bare (or
//    hierarchical) filename in the file part; join them here.
// Returns nullopt (with t_out_error set) when the resolved path is invalid.
std::optional<std::string> resolveSingleUploadTarget(const drogon::SafeStringMap<std::string>& t_form_params,
    const std::string& t_query_dir, const std::string& t_rel_name, std::string& t_out_error) {
    if (auto it = t_form_params.find("path"); it != t_form_params.end() && !it->second.empty()) {
        auto norm = normalizePath(it->second);
        if (!norm) {
            t_out_error = "Invalid path form field";
            return std::nullopt;
        }
        return *norm;
    }
    return joinUploadTargetPath(t_query_dir, t_rel_name);
}
Task<HttpResponsePtr> StorageApiHandler::handleFileRequest(HttpRequestPtr tsp_req) {
    try {
        const drogon::HttpMethod method = tsp_req->getMethod();
        std::string scope = tsp_req->getOptionalParameter<std::string>("scope").value_or("personal");

        // ── 1. Session Integrity ─────────────────────────────────────────────
        auto session = tsp_req->getAttributes()->get<Json::Value>("session_json");
        if (!session) {
            ERROR_LOG("User session not found");
            co_return createJsonErrorResponse("No session found", k401Unauthorized, "StorageApi");
        }

        std::string path_str;

        if (method == Get) {
            // ── 2. Handle Download (GET) ─────────────────────────────────────
            auto path_opt = tsp_req->getOptionalParameter<std::string>("path");
            if (!path_opt.has_value()) {
                co_return createJsonErrorResponse("Missing path parameter", k400BadRequest, "StorageApi");
            }

            auto norm = normalizePath(std::move(path_opt.value()));
            if (!norm.has_value()) {
                co_return createJsonErrorResponse("Invalid path", k400BadRequest, "StorageApi");
            }
            path_str = std::move(*norm);
            std::string range_hdr = tsp_req->getHeader("Range");
            co_return co_await storage_service_.handleDownloadFileRequest(
                std::move(session), std::move(scope), std::move(path_str), std::move(range_hdr));

        } else if (method == Post) {
            // ── 3. Handle Upload (POST) ──────────────────────────────────────
            drogon::MultiPartParser file_upload;
            if (file_upload.parse(tsp_req) == -1) {
                const std::string& ct = tsp_req->getHeader("Content-Type");
                size_t body_len = tsp_req->body().size();
                ERROR_LOG("[handleFileRequest] Failed to parse multipart request body. CT: '{}', Size: {} bytes", ct, body_len);
                co_return createJsonErrorResponse("Failed to parse multipart request body", k400BadRequest, "StorageApi");
            }

            // Extract metadata from form parameters if provided
            if (file_upload.getParameters().contains("scope")) {
                auto it = file_upload.getParameters().find("scope");
                if (it != file_upload.getParameters().end()) {
                    scope = it->second;
                }
            }

            auto path_opt = tsp_req->getOptionalParameter<std::string>("path");
            if (path_opt.has_value() && !path_opt->empty()) {
                auto norm = normalizePath(std::move(*path_opt));
                if (norm.has_value()) {
                    path_str = std::move(*norm);
                }
            }
            if (path_str.empty() && file_upload.getParameters().contains("path")) {
                auto it = file_upload.getParameters().find("path");
                if (it != file_upload.getParameters().end()) {
                    auto norm2 = normalizePath(it->second);
                    if (norm2.has_value()) {
                        path_str = std::move(*norm2);
                    }
                }
            }

            if (path_str.empty()) {
                co_return createJsonErrorResponse("Missing path parameter", k400BadRequest, "StorageApi");
            }

            const std::vector<HttpFile>& files = file_upload.getFiles();
            if (files.empty()) {
                co_return createJsonErrorResponse("No file parts found in multipart request", k400BadRequest, "StorageApi");
            }

            DEBUG_LOG("[StorageApiHandler::handleFileRequest] POST - scope: {}, path: {}, count: {}", scope, path_str, files.size());

            // ── 5. Storage Service Delegation ────────────────────────────────
            if (files.size() == 1) {
                // Single file: resolve the full target path — either the
                // multipart "path" field verbatim (SDK full-path contract)
                // or the query directory joined with the file part name
                // (drive UI contract). See resolveSingleUploadTarget().
                std::string file_rel_path = drogon::utils::urlDecode(files[0].getFileName());

                // SEC: Sanitize the relative path segment before merging with the base path
                auto safe_rel = sgrn::utils::strings::sanitizeRelativeFilename(file_rel_path);
                if (!safe_rel.has_value()) {
                    co_return createJsonErrorResponse("Invalid filename: path traversal detected", k400BadRequest);
                }
                file_rel_path = std::move(*safe_rel);

                std::string path_error;
                auto target_opt = resolveSingleUploadTarget(file_upload.getParameters(), path_str, file_rel_path, path_error);
                if (!target_opt) {
                    co_return createJsonErrorResponse(path_error, k400BadRequest, "StorageApi");
                }
                std::string target_path = std::move(*target_opt);
                DEBUG_LOG("[StorageApiHandler::handleFileRequest] Target path: '{}'", target_path);

                co_return co_await storage_service_.handleUploadFileRequest(
                    std::move(session), std::move(scope), std::move(target_path), files[0]);
            } else {
                // Batch upload optimization
                co_return co_await storage_service_.handleUploadFilesBatchRequest(
                    std::move(session), std::move(scope), std::move(path_str), files);
            }
        }

        co_return createJsonErrorResponse("Method not allowed", k405MethodNotAllowed);

    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Handler DB exception: {}", e.base().what());
        co_return createErrorResponse(GenericApiError::InternalServerError);
    } catch (const std::exception& ex) {
        ERROR_LOG("Handler exception: {}", ex.what());
        co_return createErrorResponse(GenericApiError::InternalServerError);
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
            co_return createJsonErrorResponse("Corrupted session: user.id missing or not an integer", k500InternalServerError);
        }
        if (!session["user"].isMember("role") || !session["user"]["role"].isMember("name") || !session["user"]["role"]["name"].isString()) {
            co_return createJsonErrorResponse("Corrupted session: user.role.name missing or not a string", k500InternalServerError);
        }

        const int32_t user_id = session["user"]["id"].asInt();
        const std::string role = session["user"]["role"]["name"].asString();
        const bool is_admin = (role == "admin");

        if (!is_admin && !session["session_id"].isInt()) {
            co_return createJsonErrorResponse("Corrupted session: session_id missing or not an integer", k500InternalServerError);
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

        std::string folders_json = "[";
        std::string files_json = "[";
        std::string trail_json = "[";

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

        // Builds a PostgreSQL text[] literal string from a vector of path strings,
        // e.g. ["/a", "/a/b"] → '{"/ a","/a/b"}'
        //
        // Why a literal string instead of a parameterised array?
        // drogon's execSqlCoro binds each $N to a single scalar. There is no
        // built-in way to bind a std::vector<std::string> as a $N parameter.
        // The alternative — one query per segment — is what this replaces: N RTTs
        // → 1 RTT. The paths here come from the URL's own path component, not
        // from user-supplied free-text, so they will never contain '"' or '\'.
        // The escaping below is kept for correctness anyway.
        auto make_pg_array = [](const std::vector<std::string>& paths) -> std::string {
            std::string arr = "{";
            bool first = true;
            for (const auto& p : paths) {
                if (!first)
                    arr += ',';
                first = false;
                arr += '"';
                for (char c : p) {
                    if (c == '"')
                        arr += "\\\"";
                    else if (c == '\\')
                        arr += "\\\\";
                    else
                        arr += c;
                }
                arr += '"';
            }
            arr += '}';
            return arr;
        };

        auto append_trail_node = [&trail_json](std::string t_path, std::string t_name, std::optional<int64_t> t_id = std::nullopt,
                                     std::optional<std::string> t_display_name = std::nullopt) {
            if (trail_json.size() > 1)
                trail_json += ',';
            if (t_id.has_value()) {
                trail_json += fmt::format(R"({{"path":"{}","name":"{}","id":{}{}}})", jsonEscape(t_path), jsonEscape(t_name), *t_id,
                    t_display_name ? fmt::format(R"(,"display_name":"{}")", jsonEscape(*t_display_name)) : "");
            } else {
                trail_json += fmt::format(R"({{"path":"{}","name":"{}","id":null{}}})", jsonEscape(t_path), jsonEscape(t_name),
                    t_display_name ? fmt::format(R"(,"display_name":"{}")", jsonEscape(*t_display_name)) : "");
            }
        };

        if (!is_virtual_root && current_path != "/") {
            // ── Trail resolution (two-pass, one DB round-trip) ────────────────────
            //
            // The breadcrumb trail for path "/a/b/c" needs the DB id+name for each
            // ancestor directory: /a, /a/b, /a/b/c.
            //
            // Naïve approach: one SELECT per segment inside the loop below → N RTTs.
            //
            // Optimised approach (two passes):
            //   Pass 1 — collect all prefix paths with no awaits.
            //   Pass 2 — one SELECT ... WHERE path = ANY($2::text[]) fetches all
            //             of them at once, result stored in dir_map.
            //   The trail loop below then does only map lookups — zero DB calls.
            //
            // Trade-off: the original loop was shorter and read like English.
            // This is more complex but avoids O(depth) sequential round-trips.

            // Pass 1: collect prefix paths — e.g. ["/a", "/a/b", "/a/b/c"]
            std::vector<std::string> trail_paths;
            {
                std::string cur;
                const std::string path_noslash = (current_path.front() == '/') ? current_path.substr(1) : current_path;
                std::size_t s = 0;
                while (s < path_noslash.size()) {
                    auto sl = path_noslash.find('/', s);
                    std::string seg = (sl == std::string::npos) ? path_noslash.substr(s) : path_noslash.substr(s, sl - s);
                    if (seg.empty())
                        break;
                    cur += '/' + seg;
                    trail_paths.push_back(cur);
                    if (sl == std::string::npos)
                        break;
                    s = sl + 1;
                }
            }

            // Pass 2: one query for all segments.
            // dir_map: path → (directory_id, display_name)
            std::unordered_map<std::string, std::pair<int64_t, std::string>> dir_map;
            if (!trail_paths.empty()) {
                const std::string pg_arr = make_pg_array(trail_paths);
                // drogon::orm::Result has no default constructor — use an IIFE.
                auto batch_res = co_await [&]() -> Task<drogon::orm::Result> {
                    if (scope == StorageScope::Domain) {
                        co_return co_await db_client->execSqlCoro("SELECT d.id, d.name, d.path FROM storage.directories d "
                                                                  "WHERE d.domain = $1 AND d.path = ANY($2::text[])",
                            target_domain, pg_arr);
                    }
                    if (scope == StorageScope::Personal || scope == StorageScope::Users) {
                        const int32_t trail_owner = (scope == StorageScope::Personal) ? user_id : target_owner_id;
                        co_return co_await db_client->execSqlCoro("SELECT d.id, d.name, d.path FROM storage.directories d "
                                                                  "WHERE d.user_id = $1 AND d.path = ANY($2::text[])",
                            trail_owner, pg_arr);
                    }
                    co_return co_await db_client->execSqlCoro("SELECT d.id, d.name, d.path FROM storage.directories d "
                                                              "WHERE d.automated_service_id = $1 AND d.path = ANY($2::text[])",
                        target_owner_id, pg_arr);
                }();
                for (const auto& row : batch_res) {
                    dir_map[row["path"].as<std::string>()] = {row["id"].as<int64_t>(), row["name"].as<std::string>()};
                }
            }

            if (scope != StorageScope::Personal) {
                append_trail_node(namespace_prefix, namespace_prefix.length() > 1 ? namespace_prefix.substr(1) : namespace_prefix,
                    std::nullopt, scope_res->display_name.empty() ? std::nullopt : std::optional<std::string>(scope_res->display_name));
            }

            // Build trail from map — zero DB round-trips in this loop.
            std::string cur_rel;
            const std::string path_noslash2 = (current_path.front() == '/') ? current_path.substr(1) : current_path;
            std::size_t s2 = 0;
            while (s2 < path_noslash2.size()) {
                auto sl2 = path_noslash2.find('/', s2);
                std::string seg2 = (sl2 == std::string::npos) ? path_noslash2.substr(s2) : path_noslash2.substr(s2, sl2 - s2);
                if (seg2.empty())
                    break;
                cur_rel += '/' + seg2;
                const std::string disp = namespace_prefix.empty() ? cur_rel : namespace_prefix + cur_rel;
                auto it = dir_map.find(cur_rel);
                if (it != dir_map.end()) {
                    append_trail_node(disp, it->second.second, it->second.first);
                } else {
                    // Directory not found in DB (e.g. deleted mid-session): fall
                    // back to using the raw URL segment as the display name.
                    append_trail_node(disp, seg2);
                }
                if (sl2 == std::string::npos)
                    break;
                s2 = sl2 + 1;
            }
        } else if (!is_virtual_root && scope != StorageScope::Personal) {
            append_trail_node(namespace_prefix, namespace_prefix.length() > 1 ? namespace_prefix.substr(1) : namespace_prefix, std::nullopt,
                scope_res->display_name.empty() ? std::nullopt : std::optional<std::string>(scope_res->display_name));
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

            BackendResult<void> vrl_res = co_await buildVirtualRootListing(folders_json, namespace_prefix, scope, organisation, db_client);
            if (vrl_res.hasError()) {
                co_return createJsonErrorResponse(
                    std::format("Failed to list virtual root: {}", vrl_res.error().message_), drogon::k500InternalServerError);
            }

            // Count synthetic folders by scanning commas in the JSON array
            // (cheaper than a second query — virtual root is always small)
            total_folders = static_cast<int32_t>(std::count(folders_json.begin(), folders_json.end(), '{'));
        } else {
            auto stats = co_await buildNormalDriveListing(
                folders_json, files_json, namespace_prefix, current_path, user_id, target_owner_id, scope, db_client, limit, page, search);
            total_folders = stats.first;
            total_files = stats.second;
        }

        // Close the JSON arrays
        trail_json += ']';
        folders_json += ']';
        files_json += ']';

        // Strip trailing slash from current_path for display
        std::string display_path = current_path;
        if (display_path.length() > 1 && display_path.back() == '/') {
            display_path.pop_back();
        }

        const std::string display_path_val =
            (display_path == "/" && !namespace_prefix.empty()) ? namespace_prefix : namespace_prefix + display_path;
        const int32_t total_pages = std::max(1, (int32_t)std::ceil((double)(total_folders + total_files) / limit));

        co_return createJsonResponse(fmt::format(
            R"({{"path":"{}","trail":{},"folders":{},"files":{},"capabilities":{{"can_read":{},"can_write":{},"can_delete":{},"allowed_subpath":"{}"}},"total_folders":{},"total_files":{},"total_items":{},"page":{},"page_size":{},"total_pages":{}}})",
            jsonEscape(display_path_val), trail_json, folders_json, files_json, scope_res->can_read ? "true" : "false",
            scope_res->can_write ? "true" : "false", scope_res->can_delete ? "true" : "false", jsonEscape(scope_res->allowed_subpath),
            total_folders, total_files, total_folders + total_files, page, limit, total_pages));
    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Drive list DB exception: {}", e.base().what());
        co_return createErrorResponse(GenericApiError::InternalServerError);
    } catch (const std::exception& ex) {
        ERROR_LOG("Drive list handler exception: {}", ex.what());
        co_return createErrorResponse(GenericApiError::InternalServerError);
    }
}

Task<BackendResult<void>> StorageApiHandler::buildVirtualRootListing(std::string& t_folders_json, const std::string& t_namespace_prefix,
    StorageScope t_scope, const std::string& t_organisation, const drogon::orm::DbClientPtr& tsp_db_client) {

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
            if (t_folders_json.size() > 1)
                t_folders_json += ',';
            if (t_scope == StorageScope::AutomatedServices) {
                const std::string token = row["token"].as<std::string>();
                t_folders_json += fmt::format(R"({{"name":"{}","display_name":"{}","path":"{}"}})", jsonEscape(token),
                    jsonEscape(row["display_name"].as<std::string>()), jsonEscape(t_namespace_prefix + "/" + token));
            } else {
                const std::string name = row["name"].as<std::string>();
                t_folders_json +=
                    fmt::format(R"({{"name":"{}","path":"{}"}})", jsonEscape(name), jsonEscape(t_namespace_prefix + "/" + name));
            }
        }
        co_return {};

    } catch (const std::exception& ex) {
        co_return BackendError{"Database", std::string("buildVirtualRootListing failed: ") + ex.what()};
    }
}

Task<std::pair<int32_t, int32_t>> StorageApiHandler::buildNormalDriveListing(std::string& t_folders_json, std::string& t_files_json,
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
    const int32_t offset = (t_page - 1) * t_limit;

    // ── Precomputed SQL query strings ─────────────────────────────────────────
    //
    // WHY: The original code concatenated `owner_col + " = $1 AND ..."` at
    // runtime on every request, allocating a new std::string each time.
    // These static constants are built once at program start and then only
    // referenced by pointer. The only dynamic part is `LIMIT N OFFSET M`,
    // which is appended via fmt::format at the call site (search for `+ lo`).
    //
    // NAMING SCHEME:
    //   k<Kind><Scope>[Search]
    //   Kind   : Count | Folder | File
    //   Scope  : User (user_id) | Svc (automated_service_id)
    //   Search : absent = exact-parent query,  Search = ILIKE recursive query
    //
    // PARAMETERS (same for all variants in a family):
    //   Count plain  : $1 = owner_id,  $2 = current_path,  $3 = parent_path
    //   Count search : $1 = owner_id,  $2 = ILIKE pattern, $3 = LIKE pattern
    //   Folder/File plain  : same as Count plain  + LIMIT/OFFSET appended
    //   Folder/File search : same as Count search + LIMIT/OFFSET appended
    //
    // TO ADD A NEW COLUMN: edit the SELECT list in the relevant k* constant(s)
    // and update the row-serialisation loop in section 2/3 of this function.
    //
    // Domain queries are NOT here — they use a different ownership column
    // (text `domain` vs int owner_id) and are written inline below.

    // user_id variants
    static const std::string kCountUser =
        "SELECT "
        "  (SELECT count(*) FROM storage.directories WHERE user_id = $1 AND ((parent_id IS NULL AND $2 = '/') OR (parent_id IN (SELECT id "
        "FROM storage.directories WHERE user_id = $1 AND path = $3)))) as folders, "
        "  (SELECT count(*) FROM storage.files        WHERE user_id = $1 AND ((directory_id IS NULL AND $2 = '/') OR (directory_id IN "
        "(SELECT id FROM storage.directories WHERE user_id = $1 AND path = $3)))) as files";
    static const std::string kCountUserSearch =
        "SELECT "
        "  (SELECT count(*) FROM storage.directories WHERE user_id = $1 AND name ILIKE $2 AND path LIKE $3) as folders, "
        "  (SELECT count(*) FROM storage.files        WHERE user_id = $1 AND name ILIKE $2 AND full_path LIKE $3) as files";
    static const std::string kFolderUser =
        "SELECT id, name, created_at, virtual_size, real_size, count_sub_files, count_sub_directories, path "
        "FROM storage.directories WHERE user_id = $1 AND "
        "((parent_id IS NULL AND $2 = '/') OR (parent_id IN (SELECT id FROM storage.directories WHERE user_id = $1 AND path = $3))) "
        "ORDER BY name "; // LIMIT/OFFSET appended at call site
    static const std::string kFolderUserSearch =
        "SELECT id, name, created_at, virtual_size, real_size, count_sub_files, count_sub_directories, path "
        "FROM storage.directories WHERE user_id = $1 AND name ILIKE $2 AND path LIKE $3 ORDER BY name "; // LIMIT/OFFSET appended
    static const std::string kFileUser =
        "SELECT f.id, f.name, f.full_path, f.extension, f.created_at, "
        "(SELECT COALESCE(SUM(so_sum.size), 0) FROM storage.file_objects fo_sum JOIN storage.objects so_sum ON so_sum.id = "
        "fo_sum.object_id WHERE fo_sum.file_id = f.id) AS compressed_size, "
        "(SELECT COALESCE(SUM(so_sum.original_size), 0) FROM storage.file_objects fo_sum JOIN storage.objects so_sum ON so_sum.id = "
        "fo_sum.object_id WHERE fo_sum.file_id = f.id) AS original_size, "
        "so.upload_mode, (SELECT COUNT(*) FROM storage.file_objects WHERE file_id = f.id) AS part_count "
        "FROM storage.files f JOIN storage.file_objects fo0 ON fo0.file_id = f.id AND fo0.part_index = 0 "
        "JOIN storage.objects so ON so.id = fo0.object_id "
        "WHERE f.user_id = $1 AND ((f.directory_id IS NULL AND $2 = '/') OR (f.directory_id IN (SELECT id FROM storage.directories WHERE "
        "user_id = $1 AND path = $3))) "
        "ORDER BY f.name "; // LIMIT/OFFSET appended
    static const std::string kFileUserSearch =
        "SELECT f.id, f.name, f.full_path, f.extension, f.created_at, "
        "(SELECT COALESCE(SUM(so_sum.size), 0) FROM storage.file_objects fo_sum JOIN storage.objects so_sum ON so_sum.id = "
        "fo_sum.object_id WHERE fo_sum.file_id = f.id) AS compressed_size, "
        "(SELECT COALESCE(SUM(so_sum.original_size), 0) FROM storage.file_objects fo_sum JOIN storage.objects so_sum ON so_sum.id = "
        "fo_sum.object_id WHERE fo_sum.file_id = f.id) AS original_size, "
        "so.upload_mode, (SELECT COUNT(*) FROM storage.file_objects WHERE file_id = f.id) AS part_count "
        "FROM storage.files f JOIN storage.file_objects fo0 ON fo0.file_id = f.id AND fo0.part_index = 0 "
        "JOIN storage.objects so ON so.id = fo0.object_id "
        "WHERE f.user_id = $1 AND f.name ILIKE $2 AND f.full_path LIKE $3 ORDER BY f.name "; // LIMIT/OFFSET appended
    // automated_service_id variants (same structure, different ownership column)
    static const std::string kCountSvc =
        "SELECT "
        "  (SELECT count(*) FROM storage.directories WHERE automated_service_id = $1 AND ((parent_id IS NULL AND $2 = '/') OR (parent_id "
        "IN (SELECT id FROM storage.directories WHERE automated_service_id = $1 AND path = $3)))) as folders, "
        "  (SELECT count(*) FROM storage.files        WHERE automated_service_id = $1 AND ((directory_id IS NULL AND $2 = '/') OR "
        "(directory_id IN (SELECT id FROM storage.directories WHERE automated_service_id = $1 AND path = $3)))) as files";
    static const std::string kCountSvcSearch =
        "SELECT "
        "  (SELECT count(*) FROM storage.directories WHERE automated_service_id = $1 AND name ILIKE $2 AND path LIKE $3) as folders, "
        "  (SELECT count(*) FROM storage.files        WHERE automated_service_id = $1 AND name ILIKE $2 AND full_path LIKE $3) as files";
    static const std::string kFolderSvc =
        "SELECT id, name, created_at, virtual_size, real_size, count_sub_files, count_sub_directories, path "
        "FROM storage.directories WHERE automated_service_id = $1 AND "
        "((parent_id IS NULL AND $2 = '/') OR (parent_id IN (SELECT id FROM storage.directories WHERE automated_service_id = $1 AND path = "
        "$3))) "
        "ORDER BY name "; // LIMIT/OFFSET appended
    static const std::string kFolderSvcSearch =
        "SELECT id, name, created_at, virtual_size, real_size, count_sub_files, count_sub_directories, path "
        "FROM storage.directories WHERE automated_service_id = $1 AND name ILIKE $2 AND path LIKE $3 ORDER BY name "; // LIMIT/OFFSET
                                                                                                                      // appended
    static const std::string kFileSvc =
        "SELECT f.id, f.name, f.full_path, f.extension, f.created_at, "
        "(SELECT COALESCE(SUM(so_sum.size), 0) FROM storage.file_objects fo_sum JOIN storage.objects so_sum ON so_sum.id = "
        "fo_sum.object_id WHERE fo_sum.file_id = f.id) AS compressed_size, "
        "(SELECT COALESCE(SUM(so_sum.original_size), 0) FROM storage.file_objects fo_sum JOIN storage.objects so_sum ON so_sum.id = "
        "fo_sum.object_id WHERE fo_sum.file_id = f.id) AS original_size, "
        "so.upload_mode, (SELECT COUNT(*) FROM storage.file_objects WHERE file_id = f.id) AS part_count "
        "FROM storage.files f JOIN storage.file_objects fo0 ON fo0.file_id = f.id AND fo0.part_index = 0 "
        "JOIN storage.objects so ON so.id = fo0.object_id "
        "WHERE f.automated_service_id = $1 AND ((f.directory_id IS NULL AND $2 = '/') OR (f.directory_id IN (SELECT id FROM "
        "storage.directories WHERE automated_service_id = $1 AND path = $3))) "
        "ORDER BY f.name "; // LIMIT/OFFSET appended
    static const std::string kFileSvcSearch =
        "SELECT f.id, f.name, f.full_path, f.extension, f.created_at, "
        "(SELECT COALESCE(SUM(so_sum.size), 0) FROM storage.file_objects fo_sum JOIN storage.objects so_sum ON so_sum.id = "
        "fo_sum.object_id WHERE fo_sum.file_id = f.id) AS compressed_size, "
        "(SELECT COALESCE(SUM(so_sum.original_size), 0) FROM storage.file_objects fo_sum JOIN storage.objects so_sum ON so_sum.id = "
        "fo_sum.object_id WHERE fo_sum.file_id = f.id) AS original_size, "
        "so.upload_mode, (SELECT COUNT(*) FROM storage.file_objects WHERE file_id = f.id) AS part_count "
        "FROM storage.files f JOIN storage.file_objects fo0 ON fo0.file_id = f.id AND fo0.part_index = 0 "
        "JOIN storage.objects so ON so.id = fo0.object_id "
        "WHERE f.automated_service_id = $1 AND f.name ILIKE $2 AND f.full_path LIKE $3 ORDER BY f.name "; // LIMIT/OFFSET appended

    // Select the right query family for this call.
    const std::string& kCount = is_automated ? kCountSvc : kCountUser;
    const std::string& kCountSearch = is_automated ? kCountSvcSearch : kCountUserSearch;
    const std::string& kFolderBase = is_automated ? kFolderSvc : kFolderUser;
    const std::string& kFolderSearch = is_automated ? kFolderSvcSearch : kFolderUserSearch;
    const std::string& kFileBase = is_automated ? kFileSvc : kFileUser;
    const std::string& kFileSearch = is_automated ? kFileSvcSearch : kFileUserSearch;

    std::string target_domain;
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
    const std::string path_for_parent_lookup = (current_path.length() > 1 ? current_path.substr(0, current_path.length() - 1) : "/");
    const std::string search_pattern = t_search.empty() ? "" : "%" + t_search + "%";
    const std::string recursive_pattern = current_path + "%";

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
            drogon::orm::Result count_res = co_await tsp_db_client->execSqlCoro(kCount, owner_id, current_path, path_for_parent_lookup);
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
            drogon::orm::Result count_res = co_await tsp_db_client->execSqlCoro(kCountSearch, owner_id, search_pattern, recursive_pattern);
            total_folders = static_cast<int32_t>(count_res[0]["folders"].as<int64_t>());
            total_files = static_cast<int32_t>(count_res[0]["files"].as<int64_t>());
        }
    }

    // ── 2. Fetch Folders ─────────────────────────────────────────────────────
    auto folders_res = co_await [&]() -> Task<drogon::orm::Result> {
        if (offset >= total_folders) {
            co_return co_await tsp_db_client->execSqlCoro("SELECT 1 WHERE 1=0");
        }
        const int32_t folder_limit = std::min(t_limit, total_folders - offset);
        const std::string lo = fmt::format("LIMIT {} OFFSET {}", folder_limit, offset);
        if (t_search.empty()) {
            if (is_domain) {
                co_return co_await tsp_db_client->execSqlCoro(
                    "SELECT id, name, created_at, virtual_size, real_size, count_sub_files, count_sub_directories, path "
                    "FROM storage.directories WHERE domain = $1 AND "
                    "((parent_id IS NULL AND $2 = '/') OR (parent_id IN (SELECT id FROM storage.directories WHERE domain = $1 AND path = "
                    "$3))) "
                    "ORDER BY name " +
                        lo,
                    target_domain, current_path, path_for_parent_lookup);
            }
            co_return co_await tsp_db_client->execSqlCoro(kFolderBase + lo, owner_id, current_path, path_for_parent_lookup);
        }
        if (is_domain) {
            co_return co_await tsp_db_client->execSqlCoro(
                "SELECT id, name, created_at, virtual_size, real_size, count_sub_files, count_sub_directories, path "
                "FROM storage.directories WHERE domain = $1 AND name ILIKE $2 AND path LIKE $3 ORDER BY name " +
                    lo,
                target_domain, search_pattern, recursive_pattern);
        }
        co_return co_await tsp_db_client->execSqlCoro(kFolderSearch + lo, owner_id, search_pattern, recursive_pattern);
    }();

    int32_t folders_on_page = 0;
    for (const auto& row : folders_res) {
        if (t_folders_json.size() > 1)
            t_folders_json += ',';
        t_folders_json += fmt::format(
            R"({{"id":{},"name":"{}","path":"{}","created_at":"{}","virtual_size":{},"real_size":{},"count_sub_files":{},"count_sub_directories":{}}})",
            row["id"].as<int64_t>(), jsonEscape(row["name"].as<std::string>()),
            jsonEscape(t_namespace_prefix + row["path"].as<std::string>()), jsonEscape(row["created_at"].as<std::string>()),
            row["virtual_size"].as<int64_t>(), row["real_size"].as<int64_t>(), row["count_sub_files"].as<int64_t>(),
            row["count_sub_directories"].as<int64_t>());
        ++folders_on_page;
    }

    // ── 3. Fetch Files ───────────────────────────────────────────────────────
    int32_t remaining_limit = t_limit - folders_on_page;

    if (remaining_limit > 0) {
        const int32_t file_offset = std::max(0, offset - total_folders);
        const std::string lo2 = fmt::format("LIMIT {} OFFSET {}", remaining_limit, file_offset);
        auto files_res = co_await [&]() -> Task<drogon::orm::Result> {
            if (t_search.empty()) {
                if (is_domain) {
                    co_return co_await tsp_db_client->execSqlCoro(
                        "SELECT f.id, f.name, f.full_path, f.extension, f.created_at, "
                        "(SELECT COALESCE(SUM(so_sum.size), 0) FROM storage.file_objects fo_sum JOIN storage.objects so_sum ON so_sum.id = "
                        "fo_sum.object_id WHERE fo_sum.file_id = f.id) AS compressed_size, "
                        "(SELECT COALESCE(SUM(so_sum.original_size), 0) FROM storage.file_objects fo_sum JOIN storage.objects so_sum ON "
                        "so_sum.id = fo_sum.object_id WHERE fo_sum.file_id = f.id) AS original_size, "
                        "so.upload_mode, (SELECT COUNT(*) FROM storage.file_objects WHERE file_id = f.id) AS part_count "
                        "FROM storage.files f JOIN storage.file_objects fo0 ON fo0.file_id = f.id AND fo0.part_index = 0 JOIN "
                        "storage.objects so ON so.id = fo0.object_id WHERE f.domain = $1 AND "
                        "((f.directory_id IS NULL AND $2 = '/') OR (f.directory_id IN (SELECT id FROM storage.directories WHERE domain = "
                        "$1 AND path = $3))) ORDER BY f.name " +
                            lo2,
                        target_domain, current_path, path_for_parent_lookup);
                }
                co_return co_await tsp_db_client->execSqlCoro(kFileBase + lo2, owner_id, current_path, path_for_parent_lookup);
            }
            if (is_domain) {
                co_return co_await tsp_db_client->execSqlCoro(
                    "SELECT f.id, f.name, f.full_path, f.extension, f.created_at, "
                    "(SELECT COALESCE(SUM(so_sum.size), 0) FROM storage.file_objects fo_sum JOIN storage.objects so_sum ON so_sum.id = "
                    "fo_sum.object_id WHERE fo_sum.file_id = f.id) AS compressed_size, "
                    "(SELECT COALESCE(SUM(so_sum.original_size), 0) FROM storage.file_objects fo_sum JOIN storage.objects so_sum ON "
                    "so_sum.id = fo_sum.object_id WHERE fo_sum.file_id = f.id) AS original_size, "
                    "so.upload_mode, (SELECT COUNT(*) FROM storage.file_objects WHERE file_id = f.id) AS part_count "
                    "FROM storage.files f JOIN storage.file_objects fo0 ON fo0.file_id = f.id AND fo0.part_index = 0 JOIN "
                    "storage.objects so ON so.id = fo0.object_id WHERE f.domain = $1 AND "
                    "f.name ILIKE $2 AND f.full_path LIKE $3 ORDER BY f.name " +
                        lo2,
                    target_domain, search_pattern, recursive_pattern);
            }
            co_return co_await tsp_db_client->execSqlCoro(kFileSearch + lo2, owner_id, search_pattern, recursive_pattern);
        }();

        for (const auto& row : files_res) {
            if (t_files_json.size() > 1)
                t_files_json += ',';
            const std::string part_count_val = row["part_count"].isNull() ? "null" : std::to_string(row["part_count"].as<int64_t>());
            t_files_json += fmt::format(
                R"({{"id":{},"name":"{}","path":"{}","extension":"{}","size":{},"original_size":{},"upload_mode":"{}","part_count":{},"created_at":"{}"}})",
                row["id"].as<int64_t>(), jsonEscape(row["name"].as<std::string>()),
                jsonEscape(t_namespace_prefix + row["full_path"].as<std::string>()),
                jsonEscape(row["extension"].isNull() ? "" : row["extension"].as<std::string>()), row["compressed_size"].as<int64_t>(),
                row["original_size"].as<int64_t>(), jsonEscape(row["upload_mode"].as<std::string>()), part_count_val,
                jsonEscape(row["created_at"].as<std::string>()));
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

    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Create directory DB exception: {}", e.base().what());
        co_return createErrorResponse(GenericApiError::InternalServerError);
    } catch (const std::exception& ex) {
        ERROR_LOG("Create directory handler exception: {}", ex.what());
        co_return createErrorResponse(GenericApiError::InternalServerError);
    }
}

Task<HttpResponsePtr> StorageApiHandler::handleAutomatedServiceFileRequest(HttpRequestPtr tsp_req) {
    try {
        auto norm = normalizePath(tsp_req->getOptionalParameter<std::string>("path").value_or(""));
        if (!norm.has_value()) {
            co_return createJsonErrorResponse("Invalid path", k400BadRequest, "StorageApi");
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

            DEBUG_LOG("[StorageApiHandler::handleAutomatedServiceFileRequest] POST - path: {}, count: {}", path_str, files.size());

            if (files.size() == 1) {
                // Single file: the target is either the multipart "path"
                // field verbatim (SDK full-path contract) or the query
                // directory joined with the file part name (drive UI).
                std::string file_rel_path = drogon::utils::urlDecode(files[0].getFileName());

                // SEC: Sanitize the relative path segment before merging with the base path
                auto safe_rel = sgrn::utils::strings::sanitizeRelativeFilename(file_rel_path);
                if (!safe_rel.has_value()) {
                    co_return createJsonErrorResponse("Invalid filename: path traversal detected", k400BadRequest);
                }
                file_rel_path = std::move(*safe_rel);

                std::string path_error;
                auto target_opt = resolveSingleUploadTarget(parser.getParameters(), path_str, file_rel_path, path_error);
                if (!target_opt) {
                    co_return createJsonErrorResponse(path_error, k400BadRequest, "StorageApi");
                }
                std::string target_path = std::move(*target_opt);
                co_return co_await storage_service_.handleUploadFileRequest(std::move(session), "personal", target_path, files[0]);
            } else {
                co_return co_await storage_service_.handleUploadFilesBatchRequest(std::move(session), "personal", path_str, files);
            }
        }

        co_return createJsonErrorResponse("Method not allowed", k405MethodNotAllowed);

    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("AutomatedServiceApiHandler::handleFileRequest DB exception: {}", e.base().what());
        co_return createErrorResponse(GenericApiError::InternalServerError);
    } catch (const std::exception& ex) {
        ERROR_LOG("AutomatedServiceApiHandler::handleFileRequest exception: {}", ex.what());
        co_return createErrorResponse(GenericApiError::InternalServerError);
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
            co_return createJsonErrorResponse("Corrupted session: user.id missing or not an integer", k500InternalServerError);
        }
        if (!session["user"].isMember("role") || !session["user"]["role"].isMember("name") || !session["user"]["role"]["name"].isString()) {
            co_return createJsonErrorResponse("Corrupted session: user.role.name missing or not a string", k500InternalServerError);
        }

        const int32_t user_id = session["user"]["id"].asInt();
        const bool is_admin = (session["user"]["role"]["name"].asString() == "admin");

        if (!session["session_id"].isInt()) {
            co_return createJsonErrorResponse("Corrupted session: session_id missing", k500InternalServerError);
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

    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Move handler DB exception: {}", e.base().what());
        co_return createErrorResponse(GenericApiError::InternalServerError);
    } catch (const std::exception& ex) {
        ERROR_LOG("Move handler exception: {}", ex.what());
        co_return createErrorResponse(GenericApiError::InternalServerError);
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
        co_return createJsonResponse(fmt::format(R"({{"success":true,"id":{},"type":"file"}})", t_entity_id));
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

    // 4. Re-derive the format when the name changes: a suffix the formats
    // registry acknowledges keeps its canonical (lowercased) extension, any
    // other suffix is stored as NULL and remains part of the name.
    std::optional<std::string> renamed_extension;
    if (t_target_name.has_value()) {
        const std::string raw_ext(services::storage::helpers::extractExtension(*t_target_name));
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
        co_return createJsonErrorResponse("Update failed", k500InternalServerError);
    }

    co_return createJsonResponse(fmt::format(R"({{"success":true,"id":{},"type":"file"{}{}}})", t_entity_id,
        t_target_parent_id ? fmt::format(R"(,"parent_id":{})", *t_target_parent_id) : "",
        t_target_name ? fmt::format(R"(,"name":"{}")", jsonEscape(*t_target_name)) : ""));
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
        co_return createJsonResponse(fmt::format(R"({{"success":true,"id":{},"type":"folder"}})", t_entity_id));
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

    co_return createJsonResponse(fmt::format(R"({{"success":true,"id":{},"type":"folder"{}{}}})", t_entity_id,
        t_target_parent_id ? fmt::format(R"(,"parent_id":{})", *t_target_parent_id) : "",
        t_target_name ? fmt::format(R"(,"name":"{}")", jsonEscape(*t_target_name)) : ""));
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
            co_return createJsonErrorResponse("Corrupted session: user.role.name missing or not a string", k500InternalServerError);
        }
        if (!session.isMember("user") || !session["user"].isMember("id") || !session["user"]["id"].isInt()) {
            co_return createJsonErrorResponse("Corrupted session: user.id missing or not an integer", k500InternalServerError);
        }

        const bool is_admin = (session["user"]["role"]["name"].asString() == "admin");
        if (!session["session_id"].isInt()) {
            co_return createJsonErrorResponse("Corrupted session: session_id missing", k500InternalServerError);
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

    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Delete handler DB exception: {}", e.base().what());
        co_return createErrorResponse(GenericApiError::InternalServerError);
    } catch (const std::exception& ex) {
        ERROR_LOG("Delete handler exception: {}", ex.what());
        co_return createErrorResponse(GenericApiError::InternalServerError);
    }
}

Task<HttpResponsePtr> StorageApiHandler::deleteFile(
    const drogon::orm::DbClientPtr& tsp_db_client, int64_t t_entity_id, int32_t t_user_id, bool t_is_admin) {

    // Single query: fetch file ownership + permission in one pass.
    // LEFT JOINs return NULL columns when the user has no domain permission
    // or the file has no user_id (domain-owned), both handled below.
    auto file_res = co_await tsp_db_client->execSqlCoro("SELECT f.id, f.user_id, f.domain, "
                                                        "       udp.can_delete  AS domain_can_delete, "
                                                        "       u.can_delete_personal "
                                                        "FROM storage.files f "
                                                        "LEFT JOIN core.user_domain_permissions udp "
                                                        "       ON udp.user_id = $2 AND udp.domain = f.domain "
                                                        "LEFT JOIN core.users u ON u.id = $2 "
                                                        "WHERE f.id = $1",
        t_entity_id, t_user_id);

    if (file_res.empty()) {
        co_return createJsonErrorResponse("File not found or access denied", k404NotFound);
    }

    const auto& fr = file_res[0];
    const std::string domain_name = fr["domain"].isNull() ? "" : fr["domain"].as<std::string>();

    if (!t_is_admin) {
        if (!domain_name.empty()) {
            // Shared domain: require explicit can_delete permission
            if (fr["domain_can_delete"].isNull() || !fr["domain_can_delete"].as<bool>()) {
                co_return createJsonErrorResponse(
                    "Deletion Denied: Delete capability is not granted for this operational domain.", k403Forbidden);
            }
        } else {
            // Personal workspace: must be owner + can_delete_personal flag
            if (fr["user_id"].isNull() || fr["user_id"].as<int32_t>() != t_user_id) {
                co_return createJsonErrorResponse("File not found or access denied", k404NotFound);
            }
            if (fr["can_delete_personal"].isNull() || !fr["can_delete_personal"].as<bool>()) {
                co_return createJsonErrorResponse(
                    "Deletion Denied: Deletion capability is disabled for this personal workspace.", k403Forbidden);
            }
        }
    }

    auto file_meta = co_await tsp_db_client->execSqlCoro(
        "SELECT o.id, o.bucket, o.key FROM storage.objects o JOIN storage.file_objects fo ON fo.object_id = o.id AND fo.part_index = 0 "
        "WHERE fo.file_id = $1",
        t_entity_id);
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
    auto obj_ref_res = co_await tsp_db_client->execSqlCoro("SELECT 1 FROM storage.file_objects WHERE object_id = $1 LIMIT 1", object_id);
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

    co_return createJsonResponse(fmt::format(R"({{"success":true,"id":{},"type":"file"}})", t_entity_id));
}

Task<HttpResponsePtr> StorageApiHandler::deleteFolder(
    const drogon::orm::DbClientPtr& tsp_db_client, int64_t t_entity_id, int32_t t_user_id, bool t_is_admin) {

    // Single query: ownership + permission in one pass (same pattern as deleteFile).
    auto dir_res = co_await tsp_db_client->execSqlCoro("SELECT d.id, d.user_id, d.domain, "
                                                       "       udp.can_delete  AS domain_can_delete, "
                                                       "       u.can_delete_personal "
                                                       "FROM storage.directories d "
                                                       "LEFT JOIN core.user_domain_permissions udp "
                                                       "       ON udp.user_id = $2 AND udp.domain = d.domain "
                                                       "LEFT JOIN core.users u ON u.id = $2 "
                                                       "WHERE d.id = $1",
        t_entity_id, t_user_id);

    if (dir_res.empty()) {
        co_return createJsonErrorResponse("Folder not found or access denied", k404NotFound);
    }

    const auto& dr = dir_res[0];
    const std::string domain_name = dr["domain"].isNull() ? "" : dr["domain"].as<std::string>();

    if (!t_is_admin) {
        if (!domain_name.empty()) {
            if (dr["domain_can_delete"].isNull() || !dr["domain_can_delete"].as<bool>()) {
                co_return createJsonErrorResponse(
                    "Folder Deletion Denied: Delete capability is not granted for this operational domain.", k403Forbidden);
            }
        } else {
            if (dr["user_id"].isNull() || dr["user_id"].as<int32_t>() != t_user_id) {
                co_return createJsonErrorResponse("Folder not found or access denied", k404NotFound);
            }
            if (dr["can_delete_personal"].isNull() || !dr["can_delete_personal"].as<bool>()) {
                co_return createJsonErrorResponse(
                    "Folder Deletion Denied: Deletion capability is disabled for this personal workspace.", k403Forbidden);
            }
        }
    }

    // NOTE: sub-files and sub-directories are removed by ON DELETE CASCADE.
    auto delete_res = co_await tsp_db_client->execSqlCoro("DELETE FROM storage.directories WHERE id = $1", t_entity_id);
    if (delete_res.affectedRows() == 0) {
        co_return createJsonErrorResponse("Folder deletion failed", k404NotFound);
    }

    co_return createJsonResponse(fmt::format(R"({{"success":true,"id":{},"type":"folder"}})", t_entity_id));
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

    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Bulk action handler DB exception: {}", e.base().what());
        co_return createErrorResponse(GenericApiError::InternalServerError);
    } catch (const std::exception& ex) {
        ERROR_LOG("Bulk action handler exception: {}", ex.what());
        co_return createErrorResponse(GenericApiError::InternalServerError);
    }
}

Task<HttpResponsePtr> StorageApiHandler::handlePresignedUrl(HttpRequestPtr tsp_req) {
    try {
        const auto& session = tsp_req->getAttributes()->get<Json::Value>("session_json");
        if (session.isNull()) {
            SGRN_WARN_LOG("handlePresignedUrl: No session found in request attributes");
            co_return createJsonErrorResponse("Unauthorized: Session missing", k401Unauthorized);
        }
        if (!session.isMember("user") || !session["user"].isMember("id") || !session["user"]["id"].isInt()) {
            SGRN_WARN_LOG("handlePresignedUrl: Malformed session - missing or invalid user ID");
            co_return createJsonErrorResponse("Forbidden: Malformed session payload", k403Forbidden);
        }
        const int32_t user_id = session["user"]["id"].asInt();

        auto db_res = sgrn::datastore::core::getDbClient();
        if (!db_res.has_value()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        // Parse query parameters
        std::string file_id_str = tsp_req->getParameter("file_id");
        std::string action = tsp_req->getParameter("action");     // "get" | "put" | "delete"
        std::string expiry_str = tsp_req->getParameter("expiry"); // seconds, default 3600

        if (file_id_str.empty() || action.empty()) {
            co_return createJsonErrorResponse("file_id and action parameters are required", k400BadRequest);
        }

        int64_t file_id;
        try {
            file_id = std::stoll(file_id_str);
        } catch (...) {
            co_return createJsonErrorResponse("Invalid file_id", k400BadRequest);
        }

        if (action != "get" && action != "put" && action != "delete") {
            co_return createJsonErrorResponse("action must be 'get', 'put', or 'delete'", k400BadRequest);
        }

        uint32_t expiry = 3600;
        if (!expiry_str.empty()) {
            try {
                expiry = static_cast<uint32_t>(std::stoul(expiry_str));
            } catch (...) {
                co_return createJsonErrorResponse("Invalid expiry parameter", k400BadRequest);
            }
        }
        // Cap expiry at 7 days
        if (expiry > 604800)
            expiry = 604800;

        // Look up the file to get object_id, bucket, key
        auto file_rows = co_await db->execSqlCoro("SELECT fo.object_id, o.bucket, o.key FROM storage.file_objects fo "
                                                  "JOIN storage.objects o ON o.id = fo.object_id "
                                                  "WHERE fo.file_id = $1 AND fo.part_index = 0",
            file_id);
        if (file_rows.empty()) {
            co_return createJsonErrorResponse("File not found", k404NotFound);
        }

        int64_t object_id = file_rows[0]["object_id"].as<int64_t>();
        std::string bucket = file_rows[0]["bucket"].as<std::string>();
        std::string key = file_rows[0]["key"].as<std::string>();

        // Check ownership
        auto own_rows = co_await db->execSqlCoro("SELECT 1 FROM storage.files WHERE id = $1 AND user_id = $2", file_id, user_id);
        if (own_rows.empty()) {
            co_return createJsonErrorResponse("Forbidden: File not owned by user", k403Forbidden);
        }

        // Generate presigned URL via S3Client plugin
        auto s3_res = storage_service_.S3Client();
        if (!s3_res.has_value()) {
            co_return createJsonErrorResponse(fmt::format("S3 unavailable: {}", s3_res.error().message_), k500InternalServerError);
        }
        auto* s3 = s3_res.value();

        BackendResult<std::string> url_res;
        if (action == "get") {
            url_res = co_await s3->presignedGetUrl(bucket, key, expiry);
        } else if (action == "put") {
            url_res = co_await s3->presignedPutUrl(bucket, key, expiry);
        } else {
            url_res = co_await s3->presignedDeleteUrl(bucket, key, expiry);
        }

        if (url_res.hasError()) {
            co_return createJsonErrorResponse(
                fmt::format("Failed to generate presigned URL: {}", url_res.error().message_), k500InternalServerError);
        }

        co_return createJsonResponse(fmt::format(R"({{"success":true,"url":"{}","action":"{}","expiry":{},"file_id":{}}})",
            jsonEscape(*url_res), jsonEscape(action), expiry, file_id));

    } catch (const std::exception& ex) {
        ERROR_LOG("handlePresignedUrl exception: {}", ex.what());
        co_return createErrorResponse(GenericApiError::InternalServerError);
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
            co_return createErrorResponse(GenericApiError::InternalServerError);
        } catch (const std::exception& ex) {
            SGRN_ERROR_LOG("Storage stats handler exception: {}", ex.what());
            co_return createErrorResponse(GenericApiError::InternalServerError);
        }
    } catch (const drogon::orm::DrogonDbException& e) {
        SGRN_ERROR_LOG("handleGetStorageStats: DB exception: {}", e.base().what());
        co_return createErrorResponse(GenericApiError::InternalServerError);
    } catch (const std::exception& ex) {
        SGRN_ERROR_LOG("handleGetStorageStats: exception: {}", ex.what());
        co_return createErrorResponse(GenericApiError::InternalServerError);
    }
}

drogon::Task<drogon::HttpResponsePtr> StorageApiHandler::handleRecursiveDownload(drogon::HttpRequestPtr tsp_req) {
    co_return createJsonErrorResponse("Recursive download (ZIP) is not yet implemented.", k501NotImplemented);
}

drogon::Task<drogon::HttpResponsePtr> StorageApiHandler::handleInitUploadSession(drogon::HttpRequestPtr tsp_req) {
    auto p_json = tsp_req->getJsonObject();
    if (!p_json) {
        co_return createJsonErrorResponse("Invalid JSON payload", k400BadRequest);
    }

    std::string filename = p_json->get("filename", "").asString();
    std::string target_path = p_json->get("target_path", "/").asString();
    std::string mime_type = p_json->get("mime_type", "application/octet-stream").asString();
    int64_t total_size = p_json->get("total_size", 0).asInt64();
    int64_t chunk_size = 0;
    if (p_json->isMember("chunk_size") && (*p_json)["chunk_size"].asInt64() > 0) {
        chunk_size = (*p_json)["chunk_size"].asInt64();
    } else {
        // Adaptive chunk negotiation based on file size:
        // <= 50MB: 5MB chunks (<= 10 HTTP requests)
        // 50MB - 500MB: 8MB chunks (6 to 63 HTTP requests)
        // 500MB - 5GB: 16MB chunks (31 to 312 HTTP requests)
        // > 5GB: 32MB chunks
        if (total_size > 5ULL * 1024ULL * 1024ULL * 1024ULL) {
            chunk_size = 32 * 1024 * 1024;
        } else if (total_size > 500 * 1024 * 1024) {
            chunk_size = 16 * 1024 * 1024;
        } else if (total_size > 50 * 1024 * 1024) {
            chunk_size = 8 * 1024 * 1024;
        } else {
            chunk_size = 5 * 1024 * 1024;
        }
    }

    if (filename.empty() || total_size <= 0 || chunk_size <= 0) {
        co_return createJsonErrorResponse("filename, total_size, and chunk_size are required", k400BadRequest);
    }

    int32_t total_chunks = static_cast<int32_t>((total_size + chunk_size - 1) / chunk_size);
    std::string upload_id = drogon::utils::getUuid();

    const Json::Value& session = tsp_req->attributes()->get<Json::Value>("session_json");
    int32_t user_id = session["user"]["id"].asInt();

    try {
        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        // ── Check user constraints (quota, max file size, chunk size) ────────
        auto user_row = co_await db->execSqlCoro(
            "SELECT storage_limit, total_real_size, max_file_size_mb, preferred_chunk_size_mb FROM core.users WHERE id = $1", user_id);
        if (!user_row.empty()) {
            const auto& u = user_row[0];
            if (!u["max_file_size_mb"].isNull()) {
                const int64_t max_bytes = u["max_file_size_mb"].as<int64_t>() * 1024 * 1024;
                if (total_size > max_bytes) {
                    co_return createJsonErrorResponse(
                        fmt::format("File size ({:.1f} MB) exceeds your account maximum allowed file size ({:.1f} MB)",
                            static_cast<double>(total_size) / (1024.0 * 1024.0), static_cast<double>(max_bytes) / (1024.0 * 1024.0)),
                        k400BadRequest);
                }
            }
            if (!u["storage_limit"].isNull()) {
                const int64_t limit_bytes = u["storage_limit"].as<int64_t>();
                const int64_t used_bytes = u["total_real_size"].as<int64_t>();
                if (used_bytes + total_size > limit_bytes) {
                    co_return createJsonErrorResponse(
                        "Storage quota exceeded. Please delete unused files or request a quota increase.", k413RequestEntityTooLarge);
                }
            }
            if (!u["preferred_chunk_size_mb"].isNull() && u["preferred_chunk_size_mb"].as<int32_t>() > 0) {
                chunk_size = static_cast<int64_t>(u["preferred_chunk_size_mb"].as<int32_t>()) * 1024 * 1024;
            }
        }

        // Recalculate total_chunks if chunk_size was adjusted
        total_chunks = static_cast<int32_t>((total_size + chunk_size - 1) / chunk_size);

        co_await db->execSqlCoro(
            "INSERT INTO storage.upload_sessions (id, user_id, target_path, filename, mime_type, total_size, chunk_size, total_chunks) "
            "VALUES ($1, $2, $3, $4, $5, $6, $7, $8)",
            upload_id, user_id, target_path, filename, mime_type, total_size, chunk_size, total_chunks);

        co_return createJsonResponse(fmt::format(
            R"({{"upload_id":"{}","chunk_size":{},"total_chunks":{},"status":"active"}})", upload_id, chunk_size, total_chunks));
    } catch (const std::exception& e) {
        ERROR_LOG("Init upload session error: {}", e.what());
        co_return createJsonErrorResponse("Failed to initialize upload session", k500InternalServerError);
    }
}

// ── Segment size for the streaming upload pipeline ────────────────────────────
// Chunks received from the client are compressed and buffered; once the
// compressed buffer reaches kSegmentBytes, it is flushed as its own Garage
// object.  8 MiB is a good default: above S3's 5 MiB part minimum, small
// enough to keep memory bounded even for many concurrent uploads.
static constexpr size_t kSegmentBytes = 8ULL * 1024ULL * 1024ULL;
static constexpr uint8_t kCompressionLevel = 3;

// ── UploadSessionState helpers ─────────────────────────────────────────────
std::shared_ptr<StorageApiHandler::UploadSessionState> StorageApiHandler::getOrCreateUploadState(
    const std::string& t_upload_id, const std::string& t_mime_type, uint8_t t_level) {
    std::lock_guard<std::mutex> lock(upload_states_mutex_);
    auto it = upload_states_.find(t_upload_id);
    if (it != upload_states_.end())
        return it->second;

    auto state = std::make_shared<UploadSessionState>();

    // SHA-256 context
    state->hash_ctx = EVP_MD_CTX_new();
    if (!state->hash_ctx)
        return nullptr;
    if (EVP_DigestInit_ex(state->hash_ctx, EVP_sha256(), nullptr) != 1)
        return nullptr;

    // Only compress if the MIME type is compressible (e.g. text/json/xml).
    // For already-compressed or binary formats, store verbatim.
    if (services::storage::helpers::isCompressibleMimeType(t_mime_type)) {
        state->cstream = ZSTD_createCStream();
        if (!state->cstream)
            return nullptr;
        ZSTD_initCStream(state->cstream, t_level);
        state->server_compressed = true;
    }

    state->mime_type = t_mime_type;
    upload_states_[t_upload_id] = state;
    return state;
}

void StorageApiHandler::removeUploadState(const std::string& t_upload_id) {
    std::lock_guard<std::mutex> lock(upload_states_mutex_);
    upload_states_.erase(t_upload_id);
}

// ── Flush one full segment from seg_buf to Garage ─────────────────────────
// Returns the new storage.objects id, or error.
static drogon::Task<sgrn::datastore::BackendResult<int64_t>> flushSegment(sgrn::datastore::plugins::aws::S3Client* s3,
    const drogon::orm::DbClientPtr& db, const std::string& bucket, const std::string& upload_id, int32_t seg_idx,
    std::vector<char>& seg_buf, const std::string& mime_type) {
    using namespace sgrn::datastore;
    using namespace sgrn::datastore::services::storage;

    // Build provisional key: pending/<upload_id>/<seg_idx>
    std::string seg_key = "pending/" + upload_id + "/" + std::to_string(seg_idx);
    std::string seg_data(seg_buf.begin(), seg_buf.end());
    const int64_t compressed_size = static_cast<int64_t>(seg_data.size());

    // Push segment to Garage
    auto put_res = co_await s3->uploadFromMemory(bucket, seg_key, seg_data, mime_type);
    if (put_res.hasError()) {
        co_return put_res.error();
    }

    // original_size for a provisional segment is not known yet (we only know
    // total original_size at complete time). We store compressed_size as a
    // placeholder; it is corrected after the rename in handleCompleteUploadSession.
    // Compute SHA-256 of the compressed segment data as a placeholder.
    auto sha256_res = helpers::computeSha256InMemory(seg_data);
    if (sha256_res.hasError()) {
        co_return sha256_res.error();
    }
    auto obj_res =
        co_await helpers::insertObject(db, bucket, seg_key, static_cast<size_t>(compressed_size), static_cast<size_t>(compressed_size),
            *sha256_res, /*is_compressed=*/true, std::string{"zstd"}, static_cast<std::optional<uint8_t>>(kCompressionLevel));
    co_return obj_res;
}

drogon::Task<drogon::HttpResponsePtr> StorageApiHandler::handleUploadChunk(drogon::HttpRequestPtr tsp_req) {
    std::string upload_id = tsp_req->getParameter("upload_id");
    std::string chunk_idx_str = tsp_req->getParameter("chunk_index");
    if (upload_id.empty() || chunk_idx_str.empty()) {
        co_return createJsonErrorResponse("upload_id and chunk_index parameters are required", k400BadRequest);
    }

    int32_t chunk_index = 0;
    try {
        chunk_index = std::stoi(chunk_idx_str);
    } catch (...) {
        co_return createJsonErrorResponse("chunk_index must be an integer", k400BadRequest);
    }

    const std::string_view body = tsp_req->getBody();
    if (body.empty()) {
        co_return createJsonErrorResponse("Chunk payload is empty", k400BadRequest);
    }

    try {
        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        // ── 1. Load session to get mime_type ────────────────────────────────
        auto s_rows = co_await db->execSqlCoro("SELECT mime_type, status FROM storage.upload_sessions WHERE id = $1", upload_id);
        if (s_rows.empty())
            co_return createJsonErrorResponse("Upload session not found", k404NotFound);
        if (s_rows[0]["status"].as<std::string>() != "active")
            co_return createJsonErrorResponse("Upload session is not active", k400BadRequest);
        const std::string mime_type = s_rows[0]["mime_type"].as<std::string>();

        // ── 2. Get-or-create the per-session streaming state ────────────────
        auto state = getOrCreateUploadState(upload_id, mime_type, kCompressionLevel);
        if (!state) {
            co_return createJsonErrorResponse("Failed to allocate upload stream state", k500InternalServerError);
        }

        auto s3_res = storage_service_.S3Client();
        if (s3_res.hasError())
            co_return createJsonErrorResponse(fmt::format("S3 unavailable: {}", s3_res.error().message_), k500InternalServerError);
        auto* s3 = s3_res.value();
        const services::storage::StorageConfig live_cfg = services::storage::currentStorageConfig();
        const std::string bucket = live_cfg.default_bucket;

        // ── 3. Feed raw bytes through hash + compressor ─────────────────────
        // (No mutex needed here — Drogon serialises requests per upload_id
        //  because the client sends chunks sequentially per session.)
        EVP_DigestUpdate(state->hash_ctx, body.data(), body.size());
        state->original_bytes += static_cast<int64_t>(body.size());

        if (state->server_compressed) {
            // Feed body into zstd streaming compressor
            ZSTD_inBuffer zstd_in{body.data(), body.size(), 0};
            const size_t out_buf_size = ZSTD_CStreamOutSize();
            std::vector<char> out_buf(out_buf_size);

            while (zstd_in.pos < zstd_in.size) {
                ZSTD_outBuffer zstd_out{out_buf.data(), out_buf_size, 0};
                size_t ret = ZSTD_compressStream(state->cstream, &zstd_out, &zstd_in);
                if (ZSTD_isError(ret)) {
                    co_return createJsonErrorResponse(
                        fmt::format("Compression error: {}", ZSTD_getErrorName(ret)), k500InternalServerError);
                }
                state->seg_buf.insert(state->seg_buf.end(), out_buf.data(), out_buf.data() + zstd_out.pos);
            }
        } else {
            // Non-compressible MIME: buffer raw bytes directly
            state->seg_buf.insert(state->seg_buf.end(), body.data(), body.data() + body.size());
        }

        // ── 4. Flush complete segments ───────────────────────────────────────
        // Record object_ids for each segment pushed this chunk.
        std::vector<std::pair<int32_t, int64_t>> new_segments; // {seg_idx, object_id}

        while (state->seg_buf.size() >= kSegmentBytes) {
            // Cut exactly kSegmentBytes from the front of seg_buf.
            std::vector<char> seg_data(state->seg_buf.begin(), state->seg_buf.begin() + kSegmentBytes);
            state->seg_buf.erase(state->seg_buf.begin(), state->seg_buf.begin() + kSegmentBytes);

            int32_t seg_idx = state->next_seg_idx++;
            // Temporarily swap into the flushSegment-friendly overwrite
            auto flush_res = co_await flushSegment(s3, db, bucket, upload_id, seg_idx, seg_data, mime_type);
            if (flush_res.hasError()) {
                co_return createJsonErrorResponse(
                    fmt::format("Failed to store segment {}: {}", seg_idx, flush_res.error().message_), k500InternalServerError);
            }
            new_segments.emplace_back(seg_idx, flush_res.value());
        }

        // ── 5. Persist chunk record and update session ───────────────────────
        if (!new_segments.empty()) {
            for (const auto& [seg_idx, seg_obj_id] : new_segments) {
                co_await db->execSqlCoro(
                    "INSERT INTO storage.upload_chunks (upload_id, chunk_index, chunk_size, storage_key, object_id, compressed_size) "
                    "VALUES ($1, $2, $3, $4, $5, $6) ON CONFLICT (upload_id, chunk_index) DO UPDATE "
                    "SET chunk_size = EXCLUDED.chunk_size, object_id = EXCLUDED.object_id, "
                    "    compressed_size = EXCLUDED.compressed_size",
                    upload_id, seg_idx, static_cast<int64_t>(body.size()), "pending/" + upload_id + "/" + std::to_string(seg_idx),
                    seg_obj_id, static_cast<int64_t>(kSegmentBytes));
            }
        } else {
            // No segment flushed yet (compressed bytes still buffering)
            co_await db->execSqlCoro("INSERT INTO storage.upload_chunks (upload_id, chunk_index, chunk_size, storage_key) "
                                     "VALUES ($1, $2, $3, $4) ON CONFLICT (upload_id, chunk_index) DO UPDATE "
                                     "SET chunk_size = EXCLUDED.chunk_size",
                upload_id, chunk_index, static_cast<int64_t>(body.size()), "pending/" + upload_id + "/buffering");
        }

        state->processed_chunks++;

        co_await db->execSqlCoro("UPDATE storage.upload_sessions "
                                 "SET uploaded_chunks_count = $2, "
                                 "    updated_at = NOW() WHERE id = $1",
            upload_id, state->processed_chunks);

        co_return createJsonResponse(fmt::format(
            R"({{"upload_id":"{}","chunk_index":{},"received":true,"segments_flushed":{}}})", upload_id, chunk_index, new_segments.size()));

    } catch (const std::exception& e) {
        ERROR_LOG("Upload chunk error: {}", e.what());
        co_return createJsonErrorResponse("Failed storing chunk", k500InternalServerError);
    }
}

drogon::Task<drogon::HttpResponsePtr> StorageApiHandler::handleGetUploadStatus(drogon::HttpRequestPtr tsp_req) {
    std::string upload_id = tsp_req->getParameter("upload_id");
    if (upload_id.empty()) {
        co_return createJsonErrorResponse("upload_id is required", k400BadRequest);
    }

    try {
        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        auto s_rows = co_await db->execSqlCoro("SELECT filename, target_path, total_size, chunk_size, total_chunks, uploaded_chunks_count, "
                                               "status FROM storage.upload_sessions WHERE id = $1",
            upload_id);

        if (s_rows.empty()) {
            co_return createJsonErrorResponse("Upload session not found", k404NotFound);
        }

        const auto& s = s_rows[0];

        auto idx_rows = co_await db->execSqlCoro(
            "SELECT chunk_index FROM storage.upload_chunks WHERE upload_id = $1 ORDER BY chunk_index ASC", upload_id);
        std::string indices_json = "[";
        bool first_idx = true;
        for (const auto& idx_row : idx_rows) {
            if (!first_idx)
                indices_json += ',';
            first_idx = false;
            indices_json += std::to_string(idx_row["chunk_index"].as<int32_t>());
        }
        indices_json += "]";

        co_return createJsonResponse(fmt::format(
            R"({{"upload_id":"{}","filename":"{}","target_path":"{}","total_size":{},"chunk_size":{},"total_chunks":{},"uploaded_chunks_count":{},"status":"{}","uploaded_chunk_indices":{}}})",
            upload_id, jsonEscape(s["filename"].as<std::string>()), jsonEscape(s["target_path"].as<std::string>()),
            s["total_size"].as<int64_t>(), s["chunk_size"].as<int64_t>(), s["total_chunks"].as<int32_t>(),
            s["uploaded_chunks_count"].as<int32_t>(), s["status"].as<std::string>(), indices_json));

    } catch (const std::exception& e) {
        ERROR_LOG("Get upload status error: {}", e.what());
        co_return createJsonErrorResponse("Failed fetching upload status", k500InternalServerError);
    }
}

drogon::Task<drogon::HttpResponsePtr> StorageApiHandler::handleCompleteUploadSession(drogon::HttpRequestPtr tsp_req) {
    auto p_json = tsp_req->getJsonObject();
    if (!p_json || !p_json->isMember("upload_id"))
        co_return createJsonErrorResponse("upload_id is required", k400BadRequest);

    std::string upload_id = (*p_json)["upload_id"].asString();

    try {
        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        // -- 1. Load and validate session
        auto s_rows =
            co_await db->execSqlCoro("SELECT user_id, total_chunks, uploaded_chunks_count, filename, target_path, total_size, mime_type "
                                     "FROM storage.upload_sessions WHERE id = $1",
                upload_id);
        if (s_rows.empty())
            co_return createJsonErrorResponse("Upload session not found", k404NotFound);

        const auto& s = s_rows[0];
        const int32_t total_chunks = s["total_chunks"].as<int32_t>();
        if (s["uploaded_chunks_count"].as<int32_t>() < total_chunks)
            co_return createJsonErrorResponse("Cannot complete: not all chunks have been uploaded", k400BadRequest);

        const int32_t user_id = s["user_id"].as<int32_t>();
        const std::string filename = s["filename"].as<std::string>();
        const std::string target_path = s["target_path"].as<std::string>();
        const std::string mime_type = s["mime_type"].isNull() ? "application/octet-stream" : s["mime_type"].as<std::string>();

        const Json::Value& auth_session = tsp_req->attributes()->get<Json::Value>("session_json");
        const int64_t session_id = auth_session.isMember("session_id") ? auth_session["session_id"].asInt64() : 0;

        // -- 2. Recover streaming pipeline state
        auto state = getOrCreateUploadState(upload_id, mime_type, kCompressionLevel);
        if (!state)
            co_return createJsonErrorResponse("Failed to recover upload stream state", k500InternalServerError);

        auto s3_res = storage_service_.S3Client();
        if (s3_res.hasError())
            co_return createJsonErrorResponse(fmt::format("S3 unavailable: {}", s3_res.error().message_), k500InternalServerError);
        auto* s3 = s3_res.value();
        const services::storage::StorageConfig live_cfg = services::storage::currentStorageConfig();
        const std::string bucket = live_cfg.default_bucket;

        // -- 3. Collect already-flushed segments from DB
        auto seg_rows = co_await db->execSqlCoro("SELECT uc.object_id, o.key "
                                                 "FROM storage.upload_chunks uc "
                                                 "JOIN storage.objects o ON o.id = uc.object_id "
                                                 "WHERE uc.upload_id = $1 AND uc.object_id IS NOT NULL",
            upload_id);

        std::map<int32_t, int64_t> existing_segs;
        for (const auto& row : seg_rows) {
            std::string key = row["key"].as<std::string>();
            int64_t oid = row["object_id"].as<int64_t>();
            auto slash = key.rfind('/');
            if (slash != std::string::npos) {
                try {
                    existing_segs.emplace(std::stoi(key.substr(slash + 1)), oid);
                } catch (...) {
                }
            }
        }

        // -- 4. Flush remaining compressed bytes (zstd endStream)
        if (state->server_compressed && state->cstream) {
            const size_t out_buf_size = ZSTD_CStreamOutSize();
            std::vector<char> out_buf(out_buf_size);
            bool done = false;
            while (!done) {
                ZSTD_outBuffer zstd_out{out_buf.data(), out_buf_size, 0};
                size_t ret = ZSTD_endStream(state->cstream, &zstd_out);
                if (ZSTD_isError(ret)) {
                    removeUploadState(upload_id);
                    co_return createJsonErrorResponse(
                        fmt::format("Compression flush error: {}", ZSTD_getErrorName(ret)), k500InternalServerError);
                }
                state->seg_buf.insert(state->seg_buf.end(), out_buf.data(), out_buf.data() + zstd_out.pos);
                done = (ret == 0);
            }
        }

        if (!state->seg_buf.empty()) {
            int32_t seg_idx = state->next_seg_idx++;
            auto flush_res = co_await flushSegment(s3, db, bucket, upload_id, seg_idx, state->seg_buf, mime_type);
            if (flush_res.hasError()) {
                removeUploadState(upload_id);
                co_return createJsonErrorResponse(
                    fmt::format("Failed to store final segment: {}", flush_res.error().message_), k500InternalServerError);
            }
            existing_segs.emplace(seg_idx, flush_res.value());
            state->seg_buf.clear();
        }

        if (existing_segs.empty()) {
            removeUploadState(upload_id);
            co_return createJsonErrorResponse("No segments produced", k500InternalServerError);
        }

        // -- 5. Finalize SHA-256 hash over original bytes
        std::array<unsigned char, EVP_MAX_MD_SIZE> hash_bytes{};
        unsigned int hash_len = 0;
        if (EVP_DigestFinal_ex(state->hash_ctx, hash_bytes.data(), &hash_len) != 1) {
            removeUploadState(upload_id);
            co_return createJsonErrorResponse("Failed to finalize file hash", k500InternalServerError);
        }
        static constexpr char kHexChars[] = "0123456789abcdef";
        std::string file_hash;
        file_hash.reserve(hash_len * 2);
        for (unsigned int i = 0; i < hash_len; ++i) {
            file_hash.push_back(kHexChars[hash_bytes[i] >> 4]);
            file_hash.push_back(kHexChars[hash_bytes[i] & 0x0F]);
        }
        const int64_t original_bytes = state->original_bytes;
        const bool server_compressed = state->server_compressed;
        removeUploadState(upload_id);

        // Extract declared extension from filename for sniffing validation
        std::string ext(services::storage::helpers::extractExtension(filename));

        // -- 5b. Content sniffing: read first 4096 bytes from S3 for magic detection.
        // Skip when the server itself compressed the payload: the bytes on S3 will
        // have a zstd magic header regardless of the declared extension (e.g. json),
        // which is correct and expected — the sniff check would produce a false positive.
        std::string sniffed_ext;
        if (!server_compressed) {
            auto s3_res2 = storage_service_.S3Client();
            if (s3_res2.has_value()) {
                auto* s3_sniff = s3_res2.value();
                // Download first segment to read magic bytes (use existing_segs which has provisional segments)
                std::string first_key = existing_segs.empty() ? "" : "pending/" + upload_id + "/0";
                if (!first_key.empty()) {
                    BackendResult<std::string> content_res = co_await s3_sniff->getObjectContent(bucket, first_key, 4096);
                    if (content_res.has_value()) {
                        std::string_view data(*content_res);
                        std::string preview(data.substr(0, std::min<size_t>(data.size(), 4096)));
                        auto sniff_res = ::sgrn::datastore::services::helpers::sniffExtension(preview);
                        if (sniff_res.has_value()) {
                            sniffed_ext = *sniff_res;
                        }
                    }
                }
            }
        }

        // -- 5c. Validate sniffed extension against declared extension
        if (!sniffed_ext.empty()) {
            // Normalize both to lowercase for comparison
            std::string declared_lower = ext;
            std::transform(declared_lower.begin(), declared_lower.end(), declared_lower.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::string sniffed_lower = sniffed_ext;
            std::transform(sniffed_lower.begin(), sniffed_lower.end(), sniffed_lower.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (!::sgrn::datastore::services::helpers::isCompatibleSniffedFormat(declared_lower, sniffed_lower)) {
                // Check if sniffed extension is a known format in registry
                auto format_res = co_await services::storage::helpers::getFormat(db, sniffed_lower);
                if (format_res.has_value()) {
                    // Sniffed type is a known format but differs from declared → reject
                    co_return createJsonErrorResponse(
                        fmt::format("Content type mismatch: file content appears to be '{}' but was declared as '{}'", sniffed_lower,
                            declared_lower),
                        k422UnprocessableEntity);
                }
                // Sniffed type unknown → fall back to declared extension (registry will handle)
            }
        }

        // -- 6. Rename provisional keys to final content-addressed keys
        // Final key: uploads/<file_hash>/<part_index>
        std::vector<std::pair<int32_t, int64_t>> final_segments;
        int32_t part_idx = 0;
        for (const auto& [old_seg_idx, old_obj_id] : existing_segs) {
            std::string prov_key = "pending/" + upload_id + "/" + std::to_string(old_seg_idx);
            std::string final_key = "uploads/" + file_hash + "/" + std::to_string(part_idx);

            auto dedup_rows = co_await db->execSqlCoro("SELECT id FROM storage.objects WHERE bucket = $1 AND key = $2", bucket, final_key);

            int64_t final_obj_id = 0;
            if (!dedup_rows.empty()) {
                // Deduplicated: reuse existing object
                final_obj_id = dedup_rows[0]["id"].as<int64_t>();
                co_await s3->deleteFile(bucket, prov_key);
                co_await db->execSqlCoro("DELETE FROM storage.objects WHERE id = $1", old_obj_id);
            } else {
                auto copy_res = co_await s3->copyFile(bucket, prov_key, bucket, final_key);
                if (copy_res.hasError())
                    co_return createJsonErrorResponse(
                        fmt::format("Failed to rename segment {}: {}", part_idx, copy_res.error().message_), k500InternalServerError);
                co_await s3->deleteFile(bucket, prov_key);
                co_await db->execSqlCoro(
                    "UPDATE storage.objects SET key = $1, sha256 = $2 WHERE id = $3", final_key, file_hash, old_obj_id);
                final_obj_id = old_obj_id;
            }
            // Update sha256 for deduplicated objects too
            co_await db->execSqlCoro("UPDATE storage.objects SET sha256 = $1 WHERE id = $2", file_hash, final_obj_id);
            final_segments.emplace_back(part_idx, final_obj_id);
            ++part_idx;
        }

        // -- 7. Write DB records (only after all S3 operations confirmed)
        std::string virtual_file_path;
        if (target_path.empty() || target_path == "/") {
            virtual_file_path = "/" + filename;
        } else {
            virtual_file_path = target_path;
            if (virtual_file_path.back() != '/')
                virtual_file_path += '/';
            virtual_file_path += filename;
        }

        auto dir_res =
            co_await services::storage::helpers::resolveDirectoryPath(db, user_id, std::nullopt, session_id, virtual_file_path, "");
        std::optional<int64_t> directory_id = (dir_res.hasValue() && dir_res.value().has_value()) ? dir_res.value() : std::nullopt;

        // Compression-aware suffix (same rule as the single-upload path);
        // insertFile() stores it only when the formats registry acknowledges
        // it, otherwise the extension stays NULL and the suffix stays part of
        // the name. (ext already extracted earlier for sniffing validation)
        // std::string ext(services::storage::helpers::extractExtension(filename));

        auto file_res = co_await services::storage::helpers::insertFile(
            db, filename, final_segments[0].second, user_id, std::nullopt, session_id, ext, directory_id, "");
        if (file_res.hasError()) {
            const BackendError& err = file_res.error();
            if (err.kind_ == BackendErrorKind::AlreadyExists)
                co_return createJsonErrorResponse("A file with that name already exists in this location.", k409Conflict);
            co_return createJsonErrorResponse(err.message_, k500InternalServerError);
        }
        const int64_t file_id = file_res.value();

        // Insert ordered segment manifest
        const std::string seg_role = (final_segments.size() == 1) ? "primary" : "part";
        for (const auto& [pidx, obj_id] : final_segments) {
            co_await db->execSqlCoro("INSERT INTO storage.file_objects (file_id, object_id, part_index, role) "
                                     "VALUES ($1, $2, $3, $4) ON CONFLICT (file_id, part_index) DO NOTHING",
                file_id, obj_id, pidx, seg_role);
        }

        // -- 8. Mark session completed
        co_await db->execSqlCoro("UPDATE storage.upload_sessions "
                                 "SET status = 'completed', file_hash = $2, "
                                 "    compressed_size = (SELECT COALESCE(SUM(o.size), 0) "
                                 "                       FROM storage.file_objects fo "
                                 "                       JOIN storage.objects o ON o.id = fo.object_id "
                                 "                       WHERE fo.file_id = $3), "
                                 "    updated_at = NOW() WHERE id = $1",
            upload_id, file_hash, file_id);

        co_return createJsonResponse(fmt::format(
            R"({{"success":true,"upload_id":"{}","file_id":{},"file_hash":"{}","segment_count":{},"original_bytes":{},"message":"Upload completed successfully"}})",
            upload_id, file_id, file_hash, final_segments.size(), original_bytes));

    } catch (const std::exception& e) {
        removeUploadState(upload_id);
        ERROR_LOG("Complete upload error: {}", e.what());
        co_return createJsonErrorResponse("Failed completing upload", k500InternalServerError);
    }
}

drogon::Task<drogon::HttpResponsePtr> StorageApiHandler::handleAbortUploadSession(drogon::HttpRequestPtr tsp_req) {
    std::string upload_id = tsp_req->getParameter("upload_id");
    if (upload_id.empty()) {
        auto p_json = tsp_req->getJsonObject();
        if (p_json && p_json->isMember("upload_id")) {
            upload_id = (*p_json)["upload_id"].asString();
        }
    }

    if (upload_id.empty()) {
        co_return createJsonErrorResponse("upload_id is required", k400BadRequest);
    }

    try {
        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError())
            co_return sgrn::createJsonResponse(db_res);
        auto db = db_res.value();

        co_await db->execSqlCoro("UPDATE storage.upload_sessions SET status = 'aborted', updated_at = NOW() WHERE id = $1", upload_id);

        co_return createJsonResponse(R"({"success":true,"message":"Upload session aborted"})");

    } catch (const std::exception& e) {
        ERROR_LOG("Abort upload error: {}", e.what());
        co_return createJsonErrorResponse("Failed aborting upload", k500InternalServerError);
    }
}

} // namespace sgrn::datastore::handlers::storage

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
