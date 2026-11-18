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

// storage-drive.cpp — Drive directory listing + directory object operations
// (create/move/delete/bulk/presigned/stats; split out of storage.cpp).

#include "storage-shared.hpp" // shared upload-target path helpers

namespace sgrn::datastore::handlers::storage
{
using namespace drogon;

using sgrn::datastore::services::storage::StorageScope;

namespace detail
{
struct MoveItemRequest {
    std::optional<std::string> name;
    std::optional<std::string> new_name;
    std::optional<int64_t> parent_id;
};
struct BulkItem {
    std::variant<int64_t, std::string> id{0};
    std::string type;

    int64_t numeric_id() const {
        if (std::holds_alternative<int64_t>(id)) {
            return std::get<int64_t>(id);
        }
        try {
            return std::stoll(std::get<std::string>(id));
        } catch (...) {
            return -1;
        }
    }
};
struct BulkActionRequest {
    std::string action;
    std::vector<BulkItem> items;
    std::optional<int64_t> target_parent_id;
};
struct MoveItemResponse {
    int64_t id{0};
    std::string type;
    std::optional<int64_t> parent_id;
    std::optional<std::string> name;
};
struct PresignedUrlResponse {
    std::string url;
    std::string action;
    uint32_t expiry{0};
    int64_t file_id{0};
};
struct DeleteResponse {
    int64_t id{0};
    std::string type;
};
struct BulkResult {
    int64_t id{0};
    std::string type;
    bool success{false};
    std::optional<std::string> error;
};
struct BulkResponse {
    std::string action;
    int32_t total{0};
    int32_t success_count{0};
    std::vector<BulkResult> results;
};
using StorageStatsResponse = ::sgrn::datastore::storage::StorageUsage;

struct TrailNode {
    std::string path;
    std::string name;
    std::optional<int64_t> id;
    std::optional<std::string> display_name;
};

struct VirtualRootEntry {
    std::string name;
    std::optional<std::string> display_name;
};

struct DirectoryTrailRow {
    int64_t id{0};
    std::string name;
    std::string path;
};

struct DriveListingRow {
    int32_t item_type{0};
    int64_t ord{0};
    int64_t id{0};
    std::string name;
    std::string path;
    std::string created_at;
    int64_t virtual_size{0};
    int64_t real_size{0};
    int64_t count_sub_files{0};
    int64_t count_sub_directories{0};
    std::string extension;
    int64_t size{0};
    int64_t original_size{0};
    std::string upload_mode;
    std::optional<int64_t> part_count;
    bool is_encrypted{false};
    std::optional<std::string> key_id;
    int64_t total_folders{0};
    int64_t total_files{0};
};

struct FileMoveStateRow {
    std::optional<int32_t> user_id;
    std::optional<int32_t> automated_service_id;
    std::optional<int64_t> directory_id;
};

struct FolderMoveStateRow {
    std::optional<int32_t> user_id;
    std::optional<int32_t> automated_service_id;
    std::optional<int64_t> parent_id;
    std::string path;
};

struct DirectoryPathRow {
    std::string path;
};

struct DeletePermissionRow {
    std::optional<int32_t> user_id;
    std::optional<std::string> domain;
    std::optional<bool> domain_can_delete;
    std::optional<bool> can_delete_personal;
};

struct StoredObjectRow {
    int64_t id{0};
    std::string bucket;
    std::string key;
};

struct FileEncryptionRow {
    std::optional<std::string> key_id;
    bool is_encrypted{false};
};

struct PresignedFileRow {
    int64_t object_id{0};
    std::string bucket;
    std::string key;
    bool is_encrypted{false};
};

static constexpr char kDomainDirectoryTrailQuery[] = "SELECT d.id, d.name, d.path FROM storage.directories d "
                                                     "WHERE d.domain = $1 AND d.path = ANY($2::text[])";
static constexpr char kUserDirectoryTrailQuery[] = "SELECT d.id, d.name, d.path FROM storage.directories d "
                                                   "WHERE d.user_id = $1 AND d.path = ANY($2::text[])";
static constexpr char kServiceDirectoryTrailQuery[] = "SELECT d.id, d.name, d.path FROM storage.directories d "
                                                      "WHERE d.automated_service_id = $1 AND d.path = ANY($2::text[])";
static constexpr char kServiceVirtualRootQuery[] = "SELECT token::text AS name, name AS display_name FROM core.automated_services "
                                                   "WHERE organisation = $1 AND deleted_at IS NULL ORDER BY name";
static constexpr char kDomainVirtualRootQuery[] =
    "SELECT name, NULL::text AS display_name FROM core.domains WHERE organisation = $1 ORDER BY name";
static constexpr char kUserVirtualRootQuery[] = "SELECT email AS name, NULL::text AS display_name FROM core.users "
                                                "WHERE organisation = $1 AND deleted_at IS NULL ORDER BY name";
static constexpr char kAdminFileMoveStateQuery[] =
    "SELECT id, user_id, automated_service_id, directory_id FROM storage.files WHERE id = $1";
static constexpr char kUserFileMoveStateQuery[] =
    "SELECT id, user_id, automated_service_id, directory_id FROM storage.files WHERE id = $1 AND user_id = $2";
static constexpr char kAdminFolderMoveStateQuery[] =
    "SELECT id, user_id, automated_service_id, parent_id, path FROM storage.directories WHERE id = $1";
static constexpr char kUserFolderMoveStateQuery[] =
    "SELECT id, user_id, automated_service_id, parent_id, path FROM storage.directories WHERE id = $1 AND user_id = $2";
static constexpr char kServiceDirectoryPathQuery[] = "SELECT id, path FROM storage.directories WHERE id = $1 AND automated_service_id = $2";
static constexpr char kUserDirectoryPathQuery[] = "SELECT id, path FROM storage.directories WHERE id = $1 AND user_id = $2";

struct Capabilities {
    bool can_read{false};
    bool can_write{false};
    bool can_delete{false};
    std::string allowed_subpath;
};

template <typename FolderT>
struct GenericDriveListResponse {
    std::string path;
    std::vector<TrailNode> trail;
    std::vector<FolderT> folders;
    std::vector<FileItem> files;
    Capabilities capabilities;
    int32_t total_folders{0};
    int32_t total_files{0};
    int32_t total_items{0};
    int32_t page{1};
    int32_t page_size{20};
    int32_t total_pages{1};
    std::string sort;
    std::string order;
};

using VirtualDriveListResponse = GenericDriveListResponse<VirtualFolderItem>;
using NormalDriveListResponse = GenericDriveListResponse<FolderItem>;
} // namespace detail

namespace
{
struct DriveActor {
    int32_t user_id{0};
    std::string role{"user"};
    bool is_admin{false};
    int64_t session_id{0};
    std::string organisation;
};

std::optional<DriveActor> resolveDriveActor(const drogon::HttpRequestPtr& tsp_req) {
    if (!tsp_req) {
        return std::nullopt;
    }
    if (tsp_req->attributes()->find("auth_context")) {
        const auto& auth = tsp_req->attributes()->get<filters::AuthContext>("auth_context");
        if (auth.user.id > 0) {
            DriveActor a;
            a.user_id = auth.user.id;
            a.role = auth.user.role;
            a.is_admin = (auth.user.role == "admin" || auth.user.is_platform_admin);
            a.session_id = auth.session_id;
            a.organisation = auth.user.organisation;
            return a;
        }
    }
    if (tsp_req->attributes()->find("session_claims")) {
        const auto& claims = tsp_req->attributes()->get<::sgrn::datastore::auth::SessionClaims>("session_claims");
        if (claims.user.id > 0) {
            DriveActor a;
            a.user_id = claims.user.id;
            a.role = claims.user.role.name;
            a.is_admin = (a.role == "admin");
            a.session_id = claims.session_id;
            a.organisation = claims.user.organisation;
            return a;
        }
    }
    return std::nullopt;
}
} // namespace

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

Task<HttpResponsePtr> StorageApiHandler::handleAutomatedServiceFileRequest(HttpRequestPtr tsp_req) {
    try {
        auto norm = normalizePath(tsp_req->getOptionalParameter<std::string>("path").value_or(""));
        if (!norm.has_value()) {
            co_return createTypedResponse(BackendErrorResponse{"Invalid path", k400BadRequest, "StorageApi"});
        }
        std::string path_str = std::move(*norm);
        const HttpMethod method = tsp_req->getMethod();

        const auto session = tsp_req->getAttributes()->get<::sgrn::datastore::auth::SessionClaims>("session_claims");
        if (session.user.id <= 0) {
            co_return createTypedResponse(BackendErrorResponse{"No session found", k401Unauthorized});
        }

        if (method == Get) {
            DEBUG_LOG("[StorageApiHandler::handleAutomatedServiceFileRequest] GET - path: {}", path_str);
            co_return co_await storage_service_.handleDownloadFileRequest(std::move(session), "personal", path_str);

        } else if (method == Post) {
            drogon::MultiPartParser parser;
            if (parser.parse(tsp_req) == -1) {
                const std::string& ct = tsp_req->getHeader("Content-Type");
                ERROR_LOG("[handleAutomatedServiceFileRequest] Failed to parse multipart: Content-Type='{}'", ct);
                co_return createTypedResponse(
                    BackendErrorResponse{"Failed to parse automated service multipart request body", k400BadRequest});
            }
            const auto& files = parser.getFiles();
            if (files.empty()) {
                co_return createTypedResponse(
                    BackendErrorResponse{"No file parts found in automated service multipart request", k400BadRequest});
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
                    co_return createTypedResponse(BackendErrorResponse{"Invalid filename: path traversal detected", k400BadRequest});
                }
                file_rel_path = std::move(*safe_rel);

                std::string path_error;
                auto target_opt = resolveSingleUploadTarget(parser.getParameters(), path_str, file_rel_path, path_error);
                if (!target_opt) {
                    co_return createTypedResponse(BackendErrorResponse{path_error, k400BadRequest, "StorageApi"});
                }
                std::string target_path = std::move(*target_opt);
                co_return co_await storage_service_.handleUploadFileRequest(std::move(session), "personal", target_path, files[0]);
            } else {
                co_return co_await storage_service_.handleUploadFilesBatchRequest(std::move(session), "personal", path_str, files);
            }
        }

        co_return createTypedResponse(BackendErrorResponse{"Method not allowed", k405MethodNotAllowed});

    } catch (const drogon::orm::DrogonDbException& e) {
        ERROR_LOG("AutomatedServiceApiHandler::handleFileRequest DB exception: {}", e.base().what());
        co_return createTypedResponse(BackendErrorResponse{GenericApiError::InternalServerError});
    } catch (const std::exception& ex) {
        ERROR_LOG("AutomatedServiceApiHandler::handleFileRequest exception: {}", ex.what());
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
