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

// storage_drive.cpp — Drive directory listing + directory object operations
// (create/move/delete/bulk/presigned/stats; split out of storage.cpp).

#include "storage_shared.hpp" // shared upload-target path helpers

namespace sgrn::datastore::handlers::storage
{
using namespace drogon;

using sgrn::datastore::services::storage::StorageScope;

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

        // folders_json/files_json arrive as complete SQL-side jsonb arrays
        // (built by buildVirtualRootListing / buildNormalDriveListing); only
        // the trail is still assembled row by row here.
        std::string folders_json;
        // Virtual-root listings contain folders only.  Keep this valid JSON
        // when buildVirtualRootListing intentionally does not populate files.
        std::string files_json = "[]";
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

            BackendResult<void> vrl_res =
                co_await buildVirtualRootListing(folders_json, total_folders, namespace_prefix, scope, organisation, db_client);
            if (vrl_res.hasError()) {
                co_return createJsonErrorResponse(
                    std::format("Failed to list virtual root: {}", vrl_res.error().message_), drogon::k500InternalServerError);
            }
        } else {
            auto stats = co_await buildNormalDriveListing(
                folders_json, files_json, namespace_prefix, current_path, user_id, target_owner_id, scope, db_client, limit, page, search);
            total_folders = stats.first;
            total_files = stats.second;
        }

        // Close the trail array (folders/files arrive complete from SQL).
        trail_json += ']';

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

Task<BackendResult<void>> StorageApiHandler::buildVirtualRootListing(std::string& t_folders_json, int32_t& t_out_total_folders,
    const std::string& t_namespace_prefix, StorageScope t_scope, const std::string& t_organisation,
    const drogon::orm::DbClientPtr& tsp_db_client) {

    try {
        // SQL-side JSON envelope: Postgres assembles the complete folders
        // array (aggregated with jsonb_agg, ordered by name) plus the total
        // count in one round trip, and C++ splices the returned text
        // verbatim — no per-row field extraction, no fmt/jsonEscape pass.
        // jsonb_strip_nulls drops the display_name key for non-service rows
        // so the wire shape matches the old per-row fmt::format output
        // exactly (display_name existed only for automated services).
        const std::string sql = (t_scope == StorageScope::AutomatedServices)
                                    ? "WITH items AS (SELECT token::text AS name, name AS display_name FROM core.automated_services "
                                      "WHERE organisation = $1 AND deleted_at IS NULL) "
                                      "SELECT COALESCE(jsonb_agg(jsonb_strip_nulls(jsonb_build_object("
                                      "'name', it.name, 'display_name', it.display_name, 'path', $2 || '/' || it.name)) "
                                      "ORDER BY it.name), '[]'::jsonb) AS folders, count(*)::bigint AS total FROM items it"
                                : (t_scope == StorageScope::Domain)
                                    ? "WITH items AS (SELECT name FROM core.domains WHERE organisation = $1) "
                                      "SELECT COALESCE(jsonb_agg(jsonb_strip_nulls(jsonb_build_object("
                                      "'name', it.name, 'display_name', NULL::text, 'path', $2 || '/' || it.name)) "
                                      "ORDER BY it.name), '[]'::jsonb) AS folders, count(*)::bigint AS total FROM items it"
                                    : "WITH items AS (SELECT email AS name FROM core.users WHERE organisation = $1 AND deleted_at IS NULL) "
                                      "SELECT COALESCE(jsonb_agg(jsonb_strip_nulls(jsonb_build_object("
                                      "'name', it.name, 'display_name', NULL::text, 'path', $2 || '/' || it.name)) "
                                      "ORDER BY it.name), '[]'::jsonb) AS folders, count(*)::bigint AS total FROM items it";

        drogon::orm::Result res = co_await tsp_db_client->execSqlCoro(sql, t_organisation, t_namespace_prefix);
        t_folders_json = res[0]["folders"].as<std::string>();
        t_out_total_folders = static_cast<int32_t>(res[0]["total"].as<int64_t>());
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

    // ── One-round-trip SQL-side JSON envelope ─────────────────────────────────
    //
    // The previous implementation issued 3 queries (counts, folders, files)
    // and re-serialized every row into JSON with fmt::format + jsonEscape.
    // One statement now produces the page directly as jsonb: folder and file
    // rows are rendered by jsonb_build_object with the exact same wire keys
    // and types as the old fmt output (integers as JSON numbers, timestamps
    // as strings via ::text, NULL extension as "", NULL part_count as null),
    // and the totals are computed from the same CTEs that feed the page —
    // C++ only splices the returned text into the response, no per-row
    // field extraction or escaping at all.
    //
    // Pagination semantics are preserved exactly:
    //   folder page   : LIMIT min(limit, total_folders - offset) OFFSET offset
    //   folders used  : min(max(total_folders - offset, 0), limit)
    //   file page     : LIMIT (limit - folders_used) OFFSET max(0, offset - total_folders)
    //
    // PARAMETERS:
    //   {0} owner column (directories), {1} owner column with alias `f`
    //   (files_cte), {2} unqualified owner column (file_totals has no alias
    //   on storage.files — the aliased form cannot resolve there), then
    //   $2/$3 predicate arguments:
    //     plain  = current_path (root guard `$2 = '/'`), $3 = parent path
    //     search = ILIKE pattern, $3 = recursive LIKE pattern
    //   $4 limit, $5 page offset, $6 namespace prefix for path fields.
    //
    // TO ADD A NEW COLUMN: extend the relevant CTE SELECT list and the
    // matching jsonb_build_object() call in the final SELECT.
    static const std::string kListingPlainTemplate = R"SQL(
WITH parent_dirs AS (
  SELECT id FROM storage.directories WHERE {0} = $1 AND path = $3
),
folders_cte AS (
  SELECT id, name, created_at::text AS created_at, virtual_size, real_size,
         count_sub_files, count_sub_directories, path
  FROM storage.directories
  WHERE {0} = $1 AND ((parent_id IS NULL AND $2 = '/') OR (parent_id IN (SELECT id FROM parent_dirs)))
),
folder_totals AS (SELECT count(*)::bigint AS n FROM folders_cte),
folder_page AS (
  SELECT * FROM folders_cte
  ORDER BY name
  LIMIT greatest(least($4::int, (SELECT n::int - $5::int FROM folder_totals)), 0)
  OFFSET $5::int
),
files_cte AS (
  SELECT f.id, f.name, f.full_path, f.extension, f.created_at::text AS created_at,
    (SELECT COALESCE(SUM(so_sum.size), 0) FROM storage.file_objects fo_sum JOIN storage.objects so_sum ON so_sum.id = fo_sum.object_id WHERE fo_sum.file_id = f.id) AS compressed_size,
    (SELECT COALESCE(SUM(so_sum.original_size), 0) FROM storage.file_objects fo_sum JOIN storage.objects so_sum ON so_sum.id = fo_sum.object_id WHERE fo_sum.file_id = f.id) AS original_size,
    so.upload_mode,
    (SELECT COUNT(*) FROM storage.file_objects WHERE file_id = f.id) AS part_count
  FROM storage.files f
  JOIN storage.file_objects fo0 ON fo0.file_id = f.id AND fo0.part_index = 0
  JOIN storage.objects so ON so.id = fo0.object_id
  WHERE {1} = $1 AND ((f.directory_id IS NULL AND $2 = '/') OR (f.directory_id IN (SELECT id FROM parent_dirs)))
),
file_totals AS (
  SELECT count(*)::bigint AS n FROM storage.files
  WHERE {2} = $1 AND ((directory_id IS NULL AND $2 = '/') OR (directory_id IN (SELECT id FROM parent_dirs)))
),
file_page AS (
  SELECT * FROM files_cte
  ORDER BY name
  LIMIT greatest($4::int - least(greatest((SELECT n::int FROM folder_totals) - $5::int, 0), $4::int), 0)
  OFFSET greatest($5::int - (SELECT n::int FROM folder_totals), 0)
)
SELECT
  COALESCE((SELECT jsonb_agg(jsonb_build_object(
    'id', s.id, 'name', s.name, 'path', $6 || s.path, 'created_at', s.created_at,
    'virtual_size', s.virtual_size, 'real_size', s.real_size,
    'count_sub_files', s.count_sub_files, 'count_sub_directories', s.count_sub_directories)
    ORDER BY s.name) FROM folder_page s), '[]'::jsonb) AS folders,
  COALESCE((SELECT jsonb_agg(jsonb_build_object(
    'id', s.id, 'name', s.name, 'path', $6 || s.full_path, 'extension', coalesce(s.extension, ''),
    'size', s.compressed_size, 'original_size', s.original_size, 'upload_mode', s.upload_mode,
    'part_count', s.part_count, 'created_at', s.created_at)
    ORDER BY s.name) FROM file_page s), '[]'::jsonb) AS files,
  (SELECT n FROM folder_totals) AS total_folders,
  (SELECT n FROM file_totals) AS total_files
)SQL";

    static const std::string kListingSearchTemplate = R"SQL(
WITH folders_cte AS (
  SELECT id, name, created_at::text AS created_at, virtual_size, real_size,
         count_sub_files, count_sub_directories, path
  FROM storage.directories
  WHERE {0} = $1 AND name ILIKE $2 AND path LIKE $3
),
folder_totals AS (SELECT count(*)::bigint AS n FROM folders_cte),
folder_page AS (
  SELECT * FROM folders_cte
  ORDER BY name
  LIMIT greatest(least($4::int, (SELECT n::int - $5::int FROM folder_totals)), 0)
  OFFSET $5::int
),
files_cte AS (
  SELECT f.id, f.name, f.full_path, f.extension, f.created_at::text AS created_at,
    (SELECT COALESCE(SUM(so_sum.size), 0) FROM storage.file_objects fo_sum JOIN storage.objects so_sum ON so_sum.id = fo_sum.object_id WHERE fo_sum.file_id = f.id) AS compressed_size,
    (SELECT COALESCE(SUM(so_sum.original_size), 0) FROM storage.file_objects fo_sum JOIN storage.objects so_sum ON so_sum.id = fo_sum.object_id WHERE fo_sum.file_id = f.id) AS original_size,
    so.upload_mode,
    (SELECT COUNT(*) FROM storage.file_objects WHERE file_id = f.id) AS part_count
  FROM storage.files f
  JOIN storage.file_objects fo0 ON fo0.file_id = f.id AND fo0.part_index = 0
  JOIN storage.objects so ON so.id = fo0.object_id
  WHERE {1} = $1 AND f.name ILIKE $2 AND f.full_path LIKE $3
),
file_totals AS (
  SELECT count(*)::bigint AS n FROM storage.files
  WHERE {2} = $1 AND name ILIKE $2 AND full_path LIKE $3
),
file_page AS (
  SELECT * FROM files_cte
  ORDER BY name
  LIMIT greatest($4::int - least(greatest((SELECT n::int FROM folder_totals) - $5::int, 0), $4::int), 0)
  OFFSET greatest($5::int - (SELECT n::int FROM folder_totals), 0)
)
SELECT
  COALESCE((SELECT jsonb_agg(jsonb_build_object(
    'id', s.id, 'name', s.name, 'path', $6 || s.path, 'created_at', s.created_at,
    'virtual_size', s.virtual_size, 'real_size', s.real_size,
    'count_sub_files', s.count_sub_files, 'count_sub_directories', s.count_sub_directories)
    ORDER BY s.name) FROM folder_page s), '[]'::jsonb) AS folders,
  COALESCE((SELECT jsonb_agg(jsonb_build_object(
    'id', s.id, 'name', s.name, 'path', $6 || s.full_path, 'extension', coalesce(s.extension, ''),
    'size', s.compressed_size, 'original_size', s.original_size, 'upload_mode', s.upload_mode,
    'part_count', s.part_count, 'created_at', s.created_at)
    ORDER BY s.name) FROM file_page s), '[]'::jsonb) AS files,
  (SELECT n FROM folder_totals) AS total_folders,
  (SELECT n FROM file_totals) AS total_files
)SQL";

    // Owner predicates are substituted from the shared templates once at
    // program start (thread-safe static init) instead of maintaining six
    // hand-copied statement strings. Same precomputed-SQL rationale as the
    // constants this replaces.
    static const std::string kListingUserPlain = fmt::format(fmt::runtime(kListingPlainTemplate), "user_id", "f.user_id", "user_id");
    static const std::string kListingUserSearch = fmt::format(fmt::runtime(kListingSearchTemplate), "user_id", "f.user_id", "user_id");
    static const std::string kListingSvcPlain =
        fmt::format(fmt::runtime(kListingPlainTemplate), "automated_service_id", "f.automated_service_id", "automated_service_id");
    static const std::string kListingSvcSearch =
        fmt::format(fmt::runtime(kListingSearchTemplate), "automated_service_id", "f.automated_service_id", "automated_service_id");
    static const std::string kListingDomainPlain = fmt::format(fmt::runtime(kListingPlainTemplate), "domain", "f.domain", "domain");
    static const std::string kListingDomainSearch = fmt::format(fmt::runtime(kListingSearchTemplate), "domain", "f.domain", "domain");

    const std::string& listing_sql = t_search.empty() ? static_cast<const std::string&>(is_domain      ? kListingDomainPlain
                                                                                        : is_automated ? kListingSvcPlain
                                                                                                       : kListingUserPlain)
                                                      : static_cast<const std::string&>(is_domain      ? kListingDomainSearch
                                                                                        : is_automated ? kListingSvcSearch
                                                                                                       : kListingUserSearch);

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

    // $2/$3 carry different semantics per variant: exact-parent predicates in
    // plain mode, ILIKE/LIKE patterns in search mode.
    const std::string path_for_parent_lookup = (current_path.length() > 1 ? current_path.substr(0, current_path.length() - 1) : "/");
    const std::string search_pattern = t_search.empty() ? "" : "%" + t_search + "%";
    const std::string recursive_pattern = current_path + "%";
    const std::string& predicate_2 = t_search.empty() ? current_path : search_pattern;
    const std::string& predicate_3 = t_search.empty() ? path_for_parent_lookup : recursive_pattern;

    const int32_t owner_id = (t_scope == StorageScope::Personal) ? t_user_id : t_target_owner_id;

    // drogon::orm::Result has no default constructor — use an IIFE.
    drogon::orm::Result listing_res = co_await [&]() -> Task<drogon::orm::Result> {
        if (is_domain) {
            co_return co_await tsp_db_client->execSqlCoro(
                listing_sql, target_domain, predicate_2, predicate_3, t_limit, offset, t_namespace_prefix);
        }
        co_return co_await tsp_db_client->execSqlCoro(listing_sql, owner_id, predicate_2, predicate_3, t_limit, offset, t_namespace_prefix);
    }();

    // Complete jsonb arrays + totals from one row; spliced verbatim by the
    // caller into the response envelope.
    t_folders_json = listing_res[0]["folders"].as<std::string>();
    t_files_json = listing_res[0]["files"].as<std::string>();
    co_return {static_cast<int32_t>(listing_res[0]["total_folders"].as<int64_t>()),
        static_cast<int32_t>(listing_res[0]["total_files"].as<int64_t>())};
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
                    res_item["error"] = std::string(res->getBody());
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
                    res_item["error"] = std::string(res->getBody());
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

} // namespace sgrn::datastore::handlers::storage

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
