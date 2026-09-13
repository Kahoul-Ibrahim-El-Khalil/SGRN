/*sgrn/datastore/include/sgrn/datastore/handlers/storage_admin.hpp*/
#pragma once
#include <drogon/HttpAppFramework.h>
#include <drogon/drogon.h>
#include <fmt/core.h>
#include <sgrn/datastore/utils/IHandler.hpp>

#include <array>
#include <string>

namespace sgrn::datastore::handlers::storage_admin
{

// MinIO storage administration. Every route requires UserAuthFilter +
// AdminFilter: object-layer garbage collection and raw key inspection are
// operator privileges, never tenant-user operations.
//
// Object keys ARE content hashes (base64url SHA-512, see services/helpers),
// so "search by hash" is a key / key-prefix lookup against storage.objects
// joined with a live MinIO stat.
class StorageAdminHandler : public ::sgrn::IHandler<StorageAdminHandler> {
public:
    StorageAdminHandler()
        : IHandler(this, kRoutes, kItemRoutes) {
    }

    // GET /api/v1/admin/storage/overview?max_pages=10
    // Per-bucket MinIO census (object count + bytes, paged ListObjectsV2)
    // next to the DB census (storage.objects / storage.files aggregates).
    drogon::Task<drogon::HttpResponsePtr> handleOverview(drogon::HttpRequestPtr tsp_req);

    // GET /api/v1/admin/storage/orphans?bucket=&prefix=&limit=500&max_pages=10
    // Cross-reference MinIO against storage.objects, both directions:
    //   minio_only: keys in MinIO with no DB row (purge candidates);
    //   db_missing: DB rows with no MinIO object, each HEAD-verified
    //               (broken references — report only, never auto-delete).
    drogon::Task<drogon::HttpResponsePtr> handleOrphans(drogon::HttpRequestPtr tsp_req);

    // POST /api/v1/admin/storage/orphans/purge
    // Body: {bucket, keys?: [...], prefix?: "...", dry_run?: true, limit?: 500}
    // Deletes minio_only keys (batch DeleteObjects) after re-verifying each
    // key is still absent from the DB. dry_run defaults true: report only.
    drogon::Task<drogon::HttpResponsePtr> handlePurgeOrphans(drogon::HttpRequestPtr tsp_req);

    // GET /api/v1/admin/storage/search?bucket=&key=<exact>&prefix=<prefix>&limit=100
    // Hash lookup: exact key returns the DB object row + referencing files +
    // live MinIO stat; prefix returns matches with an in_db flag each.
    drogon::Task<drogon::HttpResponsePtr> handleSearch(drogon::HttpRequestPtr tsp_req);

private:
    inline static const std::array<::sgrn::IHandler<StorageAdminHandler>::route_config, 4> kRoutes = {{
        {"/api/v1/admin/storage/overview", &StorageAdminHandler::handleOverview, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/storage/orphans", &StorageAdminHandler::handleOrphans, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/storage/orphans/purge", &StorageAdminHandler::handlePurgeOrphans, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/storage/search", &StorageAdminHandler::handleSearch, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
    }};

    inline static const std::array<::sgrn::IHandler<StorageAdminHandler>::item_route_config, 0> kItemRoutes = {};
};

} // namespace sgrn::datastore::handlers::storage_admin
