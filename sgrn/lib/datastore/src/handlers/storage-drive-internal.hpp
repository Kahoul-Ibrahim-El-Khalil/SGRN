// storage-drive-internal.hpp — declarations shared by the storage-drive*.cpp
// split files (storage-drive.cpp, -mutate.cpp, -access.cpp).
//
// The wire structs / SQL constants in `detail` and the request-actor resolver
// used to live in storage-drive.cpp; they are shared by all three translation
// units now. NOT part of the public handler interface — never include from
// outside sgrn/lib/datastore/src/handlers/.
#pragma once

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

struct DriveActor {
    int32_t user_id{0};
    std::string role{"user"};
    bool is_admin{false};
    int64_t session_id{0};
    std::string organisation;
};

inline std::optional<DriveActor> resolveDriveActor(const drogon::HttpRequestPtr& tsp_req) {
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
} // namespace sgrn::datastore::handlers::storage
