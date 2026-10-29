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

// storage_files.cpp — Files Metadata queries + File Upload/Download request
// handling (split out of storage.cpp; see section banners below).

#include "storage_shared.hpp" // shared upload-target path helpers

namespace sgrn::datastore::handlers::storage
{
using namespace drogon;

using sgrn::datastore::services::storage::StorageScope;

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

// ── SQL-side JSON envelope (no per-row C++ serialization) ───────────────────
// The metadata response is built by Postgres itself: jsonb_build_object
// renders each row directly with the same wire types as the old libjson
// rendering (integers as JSON numbers, booleans as booleans, text/timestamps
// as strings, SQL NULL as null), and the S3-compatible etag (quoted sha256)
// is appended server-side. A jsonb column's text is a valid JSON document by
// construction, so the handler splices it into the HTTP body verbatim — no
// Json::Value DOM, no re-serialization pass. (The validity gate used for
// arbitrary text JSONB columns in the CRUD engine is unnecessary for a
// jsonb-typed column.)

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
// t_out_sql uses the view column name (usable in the inner query's ORDER BY);
// t_out_key keeps the client-facing alias (the subquery's output column, used
// by the outer jsonb_agg scope, which sees no view columns).
bool parseFileOrder(const std::string& t_raw, std::string& t_out_sql, std::string& t_out_key) {
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
    t_out_key = col + (desc ? " DESC" : " ASC");
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
// filters, ordering and pagination onto t_sql/t_binds, then wraps the inner
// query into a SQL-side JSON envelope (to_jsonb + jsonb_agg) and runs it.
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
    std::string order_key = "id ASC";
    if (auto order_opt = tsp_req->getOptionalParameter<std::string>("order"); order_opt.has_value()) {
        if (!parseFileOrder(*order_opt, order_sql, order_key)) {
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
        // Inner query: same shape as always (filters accumulated above onto
        // t_sql), now ordered and paginated in a subquery so LIMIT/OFFSET cut
        // the correct page before aggregation. The final array order is
        // enforced again inside jsonb_agg by the client-facing alias (which
        // is the raw column in the subquery output), so ordering is
        // guaranteed, not an artifact of subquery scan order.
        const std::string inner_sql = fmt::format("{} ORDER BY {} LIMIT {} OFFSET {}", t_sql, order_sql, limit, offset);
        const std::string envelope_sql =
            fmt::format("SELECT COALESCE(jsonb_agg("
                        // Row document built per column (jsonb nests integers as numbers,
                        // booleans as booleans, strings as strings) with timestamps cast
                        // to text so they render exactly like the old libjson wire body.
                        "jsonb_build_object("
                        "'id', s.id, 'name', s.name, 'full_path', s.full_path, 'directory_path', s.directory_path, "
                        "'directory_id', s.directory_id, 'extension', s.extension, 'created_at', s.created_at::text, "
                        "'is_compressed', s.is_compressed, 'compression_algorithm', s.compression_algorithm, "
                        "'compression_level', s.compression_level, 'session_id', s.session_id, 'user_id', s.user_id, "
                        "'automated_service_id', s.automated_service_id, 'domain', s.domain, "
                        "'organisation', s.organisation, 'object_id', s.object_id, 'bucket', s.bucket, "
                        "'key', s.key, 'size', s.size, 'object_created_at', s.object_created_at::text, "
                        "'mime_type', s.mime_type, 'upload_mode', s.upload_mode, 'part_count', s.part_count, "
                        "'part_size_bytes', s.part_size_bytes, 'sha256', s.sha256) "
                        // ETag derived from sha256 (S3-compatible format: "sha256-<hex>")
                        // appended server-side. s.sha256 (bpchar) splices with its padding,
                        // exactly like the old Field::as<std::string> rendering.
                        "|| CASE WHEN s.sha256 IS NULL THEN '{{}}'::jsonb "
                        "ELSE jsonb_build_object('etag', '\"' || s.sha256 || '\"') END "
                        "ORDER BY {}), '[]'::jsonb) AS envelope "
                        "FROM ({}) s",
                order_key, inner_sql);
        auto res = co_await sgrn::datastore::core::execSqlCoroVec(db_res.value(), envelope_sql, t_binds);
        // The envelope is one jsonb column: libpq delivers its canonical
        // JSON text, spliced verbatim into the body.
        co_return createJsonResponse(res[0]["envelope"].as<std::string>());
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
std::optional<std::string> normalizePath(std::string t_path) {
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
std::string joinUploadTargetPath(std::string t_base_dir, std::string t_rel_name) {
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

} // namespace sgrn::datastore::handlers::storage

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
