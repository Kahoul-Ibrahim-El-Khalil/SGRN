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
        auto actor_opt = resolveDriveActor(tsp_req);
        if (!actor_opt) {
            co_return createTypedResponse(BackendErrorResponse{"No valid session found", k401Unauthorized});
        }

        const auto& actor = *actor_opt;
        const int32_t user_id = actor.user_id;
        const std::string& role = actor.role;
        const bool is_admin = actor.is_admin;
        const auto& session = tsp_req->attributes()->get<::sgrn::datastore::auth::SessionClaims>("session_claims");

        // 2. Scope and Path Resolution
        std::string scope_str = tsp_req->getOptionalParameter<std::string>("scope").value_or("personal");
        auto path_param = tsp_req->getOptionalParameter<std::string>("path");
        std::string req_path = path_param.value_or("/");

        StorageScope scope = storage_service_.parseScope(scope_str);

        auto scope_res = co_await storage_service_.resolveScopeSession(session, scope, req_path);

        if (!scope_res.has_value()) {
            co_return createTypedResponse(BackendErrorResponse{scope_res.error()});
        }

        if (!scope_res->can_read) {
            co_return createTypedResponse(
                BackendErrorResponse{"Access Denied: Read capability is not granted for this scope.", drogon::k403Forbidden});
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
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db_client = db_res.value();

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

        std::vector<detail::TrailNode> trail;
        auto append_trail_node = [&trail](std::string t_path, std::string t_name, std::optional<int64_t> t_id = std::nullopt,
                                     std::optional<std::string> t_display_name = std::nullopt) {
            trail.push_back(detail::TrailNode{
                .path = std::move(t_path), .name = std::move(t_name), .id = t_id, .display_name = std::move(t_display_name)});
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
                auto batch_rows = co_await [&]() -> Task<std::vector<detail::DirectoryTrailRow>> {
                    if (scope == StorageScope::Domain) {
                        auto batch = co_await db_client->execSqlCoro(detail::kDomainDirectoryTrailQuery, target_domain, pg_arr);
                        co_return sgrn::orm::fromResult<detail::DirectoryTrailRow, detail::kDomainDirectoryTrailQuery>(batch);
                    }
                    if (scope == StorageScope::Personal || scope == StorageScope::Users) {
                        const int32_t trail_owner = (scope == StorageScope::Personal) ? user_id : target_owner_id;
                        auto batch = co_await db_client->execSqlCoro(detail::kUserDirectoryTrailQuery, trail_owner, pg_arr);
                        co_return sgrn::orm::fromResult<detail::DirectoryTrailRow, detail::kUserDirectoryTrailQuery>(batch);
                    }
                    auto batch = co_await db_client->execSqlCoro(detail::kServiceDirectoryTrailQuery, target_owner_id, pg_arr);
                    co_return sgrn::orm::fromResult<detail::DirectoryTrailRow, detail::kServiceDirectoryTrailQuery>(batch);
                }();
                for (const auto& row : batch_rows) {
                    dir_map[row.path] = {row.id, row.name};
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

        // 5. Pagination, Search & Ordering Parameters
        int32_t limit = tsp_req->getOptionalParameter<int32_t>("limit").value_or(20);
        int32_t page = tsp_req->getOptionalParameter<int32_t>("page").value_or(1);
        std::string search = tsp_req->getOptionalParameter<std::string>("search").value_or("");
        std::string sort_key = tsp_req->getOptionalParameter<std::string>("sort").value_or("name");
        std::string sort_order = tsp_req->getOptionalParameter<std::string>("order").value_or("asc");

        if (limit < 1)
            limit = 20;
        if (page < 1)
            page = 1;

        // Whitelist-validate sort key/dir; unknown values fall back silently
        // to the default instead of erroring so older clients are unaffected.
        std::transform(sort_key.begin(), sort_key.end(), sort_key.begin(), [](unsigned char c) { return std::tolower(c); });
        std::transform(sort_order.begin(), sort_order.end(), sort_order.begin(), [](unsigned char c) { return std::tolower(c); });
        const bool sort_desc = (sort_order == "desc");
        if (sort_key != "name" && sort_key != "size" && sort_key != "modified")
            sort_key = "name";
        sort_order = sort_desc ? "desc" : "asc";

        int32_t total_folders = 0;
        int32_t total_files = 0;

        // Strip trailing slash from current_path for display
        std::string display_path = current_path;
        if (display_path.length() > 1 && display_path.back() == '/') {
            display_path.pop_back();
        }
        const std::string display_path_val =
            (display_path == "/" && !namespace_prefix.empty()) ? namespace_prefix : namespace_prefix + display_path;

        // 6. Build Content Listing (Virtual Root vs Normal Drive)
        if (is_virtual_root) {
            const std::string& organisation = actor.organisation;

            auto vrl_res = co_await buildVirtualRootListing(namespace_prefix, scope, organisation, db_client);
            if (vrl_res.hasError()) {
                co_return createTypedResponse(BackendErrorResponse{
                    std::format("Failed to list virtual root: {}", vrl_res.error().message), drogon::k500InternalServerError});
            }
            auto v_folders = std::move(vrl_res).value();
            total_folders = static_cast<int32_t>(v_folders.size());
            total_files = 0;

            const int32_t total_pages = std::max(1, (int32_t)std::ceil((double)(total_folders + total_files) / limit));

            detail::VirtualDriveListResponse resp{
                .path = display_path_val,
                .trail = std::move(trail),
                .folders = std::move(v_folders),
                .files = {},
                .capabilities =
                    {
                        .can_read = scope_res->can_read,
                        .can_write = scope_res->can_write,
                        .can_delete = scope_res->can_delete,
                        .allowed_subpath = scope_res->allowed_subpath,
                    },
                .total_folders = total_folders,
                .total_files = total_files,
                .total_items = total_folders + total_files,
                .page = page,
                .page_size = limit,
                .total_pages = total_pages,
                .sort = sort_key,
                .order = sort_order,
            };
            co_return createTypedResponse(tsp_req, resp, k200OK);
        } else {
            auto listing = co_await buildNormalDriveListing(
                namespace_prefix, current_path, user_id, target_owner_id, scope, db_client, limit, page, search, sort_key, sort_desc);
            total_folders = listing.total_folders;
            total_files = listing.total_files;

            const int32_t total_pages = std::max(1, (int32_t)std::ceil((double)(total_folders + total_files) / limit));

            detail::NormalDriveListResponse resp{
                .path = display_path_val,
                .trail = std::move(trail),
                .folders = std::move(listing.folders),
                .files = std::move(listing.files),
                .capabilities =
                    {
                        .can_read = scope_res->can_read,
                        .can_write = scope_res->can_write,
                        .can_delete = scope_res->can_delete,
                        .allowed_subpath = scope_res->allowed_subpath,
                    },
                .total_folders = total_folders,
                .total_files = total_files,
                .total_items = total_folders + total_files,
                .page = page,
                .page_size = limit,
                .total_pages = total_pages,
                .sort = sort_key,
                .order = sort_order,
            };
            co_return createTypedResponse(tsp_req, resp, k200OK);
        }
    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("Drive list DB exception: {}", e.base().what());
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    } catch (const std::exception& ex) {
        ERROR_LOG("Drive list handler exception: {}", ex.what());
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    }
}

Task<Result<std::vector<detail::VirtualFolderItem>, BackendError>> StorageApiHandler::buildVirtualRootListing(
    const std::string& t_namespace_prefix, StorageScope t_scope, const std::string& t_organisation,
    const drogon::orm::DbClientPtr& tsp_db_client) {

    try {
        auto res = co_await [&]() -> Task<drogon::orm::Result> {
            if (t_scope == StorageScope::AutomatedServices) {
                co_return co_await tsp_db_client->execSqlCoro(detail::kServiceVirtualRootQuery, t_organisation);
            }
            if (t_scope == StorageScope::Domain) {
                co_return co_await tsp_db_client->execSqlCoro(detail::kDomainVirtualRootQuery, t_organisation);
            }
            co_return co_await tsp_db_client->execSqlCoro(detail::kUserVirtualRootQuery, t_organisation);
        }();
        auto entries = t_scope == StorageScope::AutomatedServices
                           ? sgrn::orm::fromResult<detail::VirtualRootEntry, detail::kServiceVirtualRootQuery>(res)
                       : t_scope == StorageScope::Domain
                           ? sgrn::orm::fromResult<detail::VirtualRootEntry, detail::kDomainVirtualRootQuery>(res)
                           : sgrn::orm::fromResult<detail::VirtualRootEntry, detail::kUserVirtualRootQuery>(res);
        std::vector<detail::VirtualFolderItem> items;
        items.reserve(entries.size());
        for (auto& entry : entries) {
            detail::VirtualFolderItem item;
            item.name = std::move(entry.name);
            item.path = t_namespace_prefix + "/" + item.name;
            item.display_name = std::move(entry.display_name);
            items.push_back(std::move(item));
        }
        co_return items;

    } catch (const std::exception& ex) {
        co_return BackendError(BackendErrorKind::Database, std::string("buildVirtualRootListing failed: ") + ex.what());
    }
}

Task<detail::NormalDriveListingResult> StorageApiHandler::buildNormalDriveListing(const std::string& t_namespace_prefix,
    const std::string& t_current_path_in, int32_t t_user_id, int32_t t_target_owner_id, StorageScope t_scope,
    const drogon::orm::DbClientPtr& tsp_db_client, int32_t t_limit, int32_t t_page, const std::string& t_search,
    const std::string& t_sort_key, bool t_sort_desc) {

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
  ORDER BY %FOLDER_PAGE_ORDER%
  LIMIT greatest(least($4::int, (SELECT n::int - $5::int FROM folder_totals)), 0)
  OFFSET $5::int
),
files_cte AS (
  SELECT f.id, f.name, f.full_path, f.extension, f.created_at::text AS created_at,
    f.is_encrypted, f.key_id,
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
  ORDER BY %FILE_PAGE_ORDER%
  LIMIT greatest($4::int - least(greatest((SELECT n::int FROM folder_totals) - $5::int, 0), $4::int), 0)
  OFFSET greatest($5::int - (SELECT n::int FROM folder_totals), 0)
)
SELECT * FROM (
  SELECT
    1 AS item_type,
    row_number() OVER (ORDER BY %FOLDER_PAGE_ORDER%) AS ord,
    s.id,
    s.name,
    $6 || s.path AS path,
    s.created_at,
    s.virtual_size,
    s.real_size,
    s.count_sub_files,
    s.count_sub_directories,
    '' AS extension,
    0::bigint AS size,
    0::bigint AS original_size,
    '' AS upload_mode,
    NULL::bigint AS part_count,
    false AS is_encrypted,
    NULL::text AS key_id,
    (SELECT n FROM folder_totals) AS total_folders,
    (SELECT n FROM file_totals) AS total_files
  FROM folder_page s
  UNION ALL
  SELECT
    2 AS item_type,
    row_number() OVER (ORDER BY %FILE_PAGE_ORDER%) AS ord,
    s.id,
    s.name,
    $6 || s.full_path AS path,
    s.created_at,
    0::bigint AS virtual_size,
    0::bigint AS real_size,
    0::bigint AS count_sub_files,
    0::bigint AS count_sub_directories,
    coalesce(s.extension, '') AS extension,
    s.compressed_size AS size,
    s.original_size AS original_size,
    s.upload_mode AS upload_mode,
    s.part_count AS part_count,
    coalesce(s.is_encrypted, false) AS is_encrypted,
    s.key_id AS key_id,
    (SELECT n FROM folder_totals) AS total_folders,
    (SELECT n FROM file_totals) AS total_files
  FROM file_page s
  UNION ALL
  SELECT
    0 AS item_type,
    0::bigint AS ord,
    0::bigint AS id,
    '' AS name,
    '' AS path,
    '' AS created_at,
    0::bigint AS virtual_size,
    0::bigint AS real_size,
    0::bigint AS count_sub_files,
    0::bigint AS count_sub_directories,
    '' AS extension,
    0::bigint AS size,
    0::bigint AS original_size,
    '' AS upload_mode,
    NULL::bigint AS part_count,
    false AS is_encrypted,
    NULL::text AS key_id,
    (SELECT n FROM folder_totals) AS total_folders,
    (SELECT n FROM file_totals) AS total_files
  WHERE NOT EXISTS (SELECT 1 FROM folder_page) AND NOT EXISTS (SELECT 1 FROM file_page)
) u ORDER BY item_type, ord
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
  ORDER BY %FOLDER_PAGE_ORDER%
  LIMIT greatest(least($4::int, (SELECT n::int - $5::int FROM folder_totals)), 0)
  OFFSET $5::int
),
files_cte AS (
  SELECT f.id, f.name, f.full_path, f.extension, f.created_at::text AS created_at,
    f.is_encrypted, f.key_id,
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
  ORDER BY %FILE_PAGE_ORDER%
  LIMIT greatest($4::int - least(greatest((SELECT n::int FROM folder_totals) - $5::int, 0), $4::int), 0)
  OFFSET greatest($5::int - (SELECT n::int FROM folder_totals), 0)
)
SELECT * FROM (
  SELECT
    1 AS item_type,
    row_number() OVER (ORDER BY %FOLDER_PAGE_ORDER%) AS ord,
    s.id,
    s.name,
    $6 || s.path AS path,
    s.created_at,
    s.virtual_size,
    s.real_size,
    s.count_sub_files,
    s.count_sub_directories,
    '' AS extension,
    0::bigint AS size,
    0::bigint AS original_size,
    '' AS upload_mode,
    NULL::bigint AS part_count,
    false AS is_encrypted,
    NULL::text AS key_id,
    (SELECT n FROM folder_totals) AS total_folders,
    (SELECT n FROM file_totals) AS total_files
  FROM folder_page s
  UNION ALL
  SELECT
    2 AS item_type,
    row_number() OVER (ORDER BY %FILE_PAGE_ORDER%) AS ord,
    s.id,
    s.name,
    $6 || s.full_path AS path,
    s.created_at,
    0::bigint AS virtual_size,
    0::bigint AS real_size,
    0::bigint AS count_sub_files,
    0::bigint AS count_sub_directories,
    coalesce(s.extension, '') AS extension,
    s.compressed_size AS size,
    s.original_size AS original_size,
    s.upload_mode AS upload_mode,
    s.part_count AS part_count,
    coalesce(s.is_encrypted, false) AS is_encrypted,
    s.key_id AS key_id,
    (SELECT n FROM folder_totals) AS total_folders,
    (SELECT n FROM file_totals) AS total_files
  FROM file_page s
  UNION ALL
  SELECT
    0 AS item_type,
    0::bigint AS ord,
    0::bigint AS id,
    '' AS name,
    '' AS path,
    '' AS created_at,
    0::bigint AS virtual_size,
    0::bigint AS real_size,
    0::bigint AS count_sub_files,
    0::bigint AS count_sub_directories,
    '' AS extension,
    0::bigint AS size,
    0::bigint AS original_size,
    '' AS upload_mode,
    NULL::bigint AS part_count,
    false AS is_encrypted,
    NULL::text AS key_id,
    (SELECT n FROM folder_totals) AS total_folders,
    (SELECT n FROM file_totals) AS total_files
  WHERE NOT EXISTS (SELECT 1 FROM folder_page) AND NOT EXISTS (SELECT 1 FROM file_page)
) u ORDER BY item_type, ord
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

    // ── Sort key resolution (whitelist → no user text reaches the SQL) ────────
    // The templates carry %ORDER% tokens (cannot collide with fmt braces);
    // they are substituted here per request with a whitelisted column and
    // direction plus a stable tiebreaker (name, then id) so pagination
    // stays consistent across pages regardless of sort key.
    struct SortColumns {
        std::string_view folders;
        std::string_view files;
    };
    static const std::unordered_map<std::string_view, SortColumns> kSortKeys{
        {"name", {"name", "name"}},                    //
        {"size", {"virtual_size", "compressed_size"}}, // folders size ≠ files size
        {"modified", {"created_at", "created_at"}},    // tables have no updated_at
    };

    const auto key_it = kSortKeys.find(t_sort_key);
    const SortColumns& sort_cols = (key_it != kSortKeys.end()) ? key_it->second : kSortKeys.at("name");

    auto make_order = [t_sort_desc](std::string_view t_column, bool t_qualified) {
        const std::string prefix = t_qualified ? "s." : "";
        if (t_column == "name")
            return fmt::format("{0}name{1}, {0}id ASC", prefix, t_sort_desc ? " DESC" : " ASC");
        return fmt::format("{0}{1}{2}, {0}name ASC, {0}id ASC", prefix, t_column, t_sort_desc ? " DESC" : " ASC");
    };

    // One copy of the 6 KB statement per request — the token substitution
    // keeps the owner-variant strings static/thread-safe as before.
    std::string order_sql = listing_sql;
    auto replace_token = [](std::string& t_sql, std::string_view t_token, const std::string& t_value) {
        for (std::string::size_type pos = 0; (pos = t_sql.find(t_token, pos)) != std::string::npos; pos += t_value.size())
            t_sql.replace(pos, t_token.size(), t_value);
    };
    replace_token(order_sql, "%FOLDER_PAGE_ORDER%", make_order(sort_cols.folders, false));
    replace_token(order_sql, "%FILE_PAGE_ORDER%", make_order(sort_cols.files, false));

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
                order_sql, target_domain, predicate_2, predicate_3, t_limit, offset, t_namespace_prefix);
        }
        co_return co_await tsp_db_client->execSqlCoro(order_sql, owner_id, predicate_2, predicate_3, t_limit, offset, t_namespace_prefix);
    }();

    detail::NormalDriveListingResult result;
    if (listing_res.empty()) {
        co_return result;
    }

    const auto rows = sgrn::orm::fromResultByName<detail::DriveListingRow>(listing_res);
    result.total_folders = static_cast<int32_t>(rows[0].total_folders);
    result.total_files = static_cast<int32_t>(rows[0].total_files);

    for (const auto& row : rows) {
        if (row.item_type == 1) {
            detail::FolderItem folder;
            folder.id = row.id;
            folder.name = row.name;
            folder.path = row.path;
            folder.created_at = row.created_at;
            folder.virtual_size = row.virtual_size;
            folder.real_size = row.real_size;
            folder.count_sub_files = row.count_sub_files;
            folder.count_sub_directories = row.count_sub_directories;
            result.folders.push_back(std::move(folder));
        } else if (row.item_type == 2) {
            detail::FileItem file;
            file.id = row.id;
            file.name = row.name;
            file.path = row.path;
            file.extension = row.extension;
            file.created_at = row.created_at;
            file.size = row.size;
            file.original_size = row.original_size;
            file.upload_mode = row.upload_mode;
            file.part_count = row.part_count;
            file.is_encrypted = row.is_encrypted;
            file.key_id = row.key_id;
            result.files.push_back(std::move(file));
        }
    }
    co_return result;
}

} // namespace sgrn::datastore::handlers::storage

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
