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

// Garage storage administration. Every route requires UserAuthFilter +
// AdminFilter: object-layer garbage collection and raw key inspection are
// operator privileges, never tenant-user operations.
//
// Object keys ARE content hashes (base64url SHA-512, see services/helpers),
// so "search by hash" is a key / key-prefix lookup against storage.objects
// joined with a live Garage stat.
class StorageAdminHandler : public ::sgrn::IHandler<StorageAdminHandler> {
public:
    StorageAdminHandler()
        : IHandler(this, kRoutes, kItemRoutes) {
    }

    // GET /api/v1/admin/storage/overview?max_pages=10
    // Per-bucket Garage census (object count + bytes, paged ListObjectsV2)
    // next to the DB census (storage.objects / storage.files aggregates).
    drogon::Task<drogon::HttpResponsePtr> handleOverview(drogon::HttpRequestPtr tsp_req);

    // GET /api/v1/admin/storage/orphans?bucket=&prefix=&limit=500&max_pages=10
    // Cross-reference Garage against storage.objects, both directions:
    //   garage_only: keys in Garage with no DB row (purge candidates);
    //   db_missing: DB rows with no Garage object, each HEAD-verified
    //               (broken references — report only, never auto-delete).
    drogon::Task<drogon::HttpResponsePtr> handleOrphans(drogon::HttpRequestPtr tsp_req);

    // POST /api/v1/admin/storage/orphans/purge
    // Body: {bucket, keys?: [...], prefix?: "...", dry_run?: true, limit?: 500}
    // Deletes garage_only keys (batch DeleteObjects) after re-verifying each
    // key is still absent from the DB. dry_run defaults true: report only.
    drogon::Task<drogon::HttpResponsePtr> handlePurgeOrphans(drogon::HttpRequestPtr tsp_req);

    // GET /api/v1/admin/storage/search?bucket=&key=<exact>&prefix=<prefix>&limit=100
    // Hash lookup: exact key returns the DB object row + referencing files +
    // live Garage stat; prefix returns matches with an in_db flag each.
    drogon::Task<drogon::HttpResponsePtr> handleSearch(drogon::HttpRequestPtr tsp_req);

    // GET /api/v1/admin/system/config
    // Effective tunable configuration for the System Config dashboard tab:
    // sanitized values (secrets redacted — secrets are file-only), the field
    // schema (type + hot/restart metadata, server-driven so the UI needs no
    // per-key code), and paths changed since boot that still need a restart.
    drogon::Task<drogon::HttpResponsePtr> handleGetSystemConfig(drogon::HttpRequestPtr tsp_req);

    // PUT /api/v1/admin/system/config
    // Body: {"values": {<nested partial mirroring sgrn.json>}}. Validates
    // (unknown/secret keys rejected, ranges + S3 multipart band enforced),
    // backs the live file up to sgrn.json.bak, writes atomically (tmp +
    // rename), hot-applies s3.* + rate_limiting.* and reports which changed
    // paths applied live vs need a restart.
    drogon::Task<drogon::HttpResponsePtr> handleUpdateSystemConfig(drogon::HttpRequestPtr tsp_req);

    // GET /api/v1/admin/storage/formats?search=&allowed=true&limit=500
    // The formats registry (extension → mime/flags) — the source of truth for
    // which suffixes count as formats. Files whose suffix has no row here are
    // stored with a NULL extension (the suffix simply stays part of the name);
    // is_allowed=false additionally vetoes uploads at the service layer
    // (enforced in StorageService::uploadFile).
    drogon::Task<drogon::HttpResponsePtr> handleListFormats(drogon::HttpRequestPtr tsp_req);

    // POST /api/v1/admin/storage/formats
    // Body: {extension, mime_type, is_compressed?, is_allowed?, description?}.
    // Upsert by extension (lowercased): adds a new format or curates an
    // existing one. Extension renames propagate to storage.files via
    // ON UPDATE CASCADE — there is no rename endpoint by design; delete +
    // re-add instead so the blast radius stays explicit.
    drogon::Task<drogon::HttpResponsePtr> handleUpsertFormat(drogon::HttpRequestPtr tsp_req);

    // DELETE /api/v1/admin/storage/formats?extension=
    // Refused with 409 (plus referencing file count) while storage.files
    // still references the extension — the FK has no ON DELETE action, so a
    // forced delete would corrupt file metadata.
    drogon::Task<drogon::HttpResponsePtr> handleDeleteFormat(drogon::HttpRequestPtr tsp_req);

    // GET /api/v1/admin/quotas?kind=user|service|organisation&search=&limit=100
    // Quota ledger: live usage (total_virtual_size, total_entry_count) next
    // to the enforced caps (storage_limit bytes, entry_count_limit; null =
    // unlimited). Enforcement lives in the storage.enforce_storage_limit
    // trigger — this surface only reads and tunes the caps, so no upload
    // code changes when limits move.
    drogon::Task<drogon::HttpResponsePtr> handleListQuotas(drogon::HttpRequestPtr tsp_req);

    // PUT /api/v1/admin/quotas
    // Body: {kind, id?, name?, storage_limit_bytes?: uint|null,
    //        entry_count_limit?: uint|null}. Omitted keys are left alone;
    // explicit null lifts the cap. No self-exclusion: an admin tunes their
    // own quota through the same call.
    drogon::Task<drogon::HttpResponsePtr> handleUpdateQuota(drogon::HttpRequestPtr tsp_req);

    // GET /api/v1/admin/analytics/overview
    // Fleet-wide picture in one round trip: actor/object counts, virtual vs
    // stored bytes (compression savings), 30-day upload timeseries, top
    // extensions, top uploaders, most recent files. All read-only aggregates.
    drogon::Task<drogon::HttpResponsePtr> handleAnalyticsOverview(drogon::HttpRequestPtr tsp_req);

    // GET /api/v1/admin/analytics/breakdown?kind=domain|organisation|user|service&search=&limit=
    // Per-slice ledger: files, virtual bytes, actors/quota usage depending on
    // kind. Backs the analytics dashboard tables.
    drogon::Task<drogon::HttpResponsePtr> handleAnalyticsBreakdown(drogon::HttpRequestPtr tsp_req);

private:
    inline static const std::array<::sgrn::IHandler<StorageAdminHandler>::route_config, 13> kRoutes = {{
        {"/api/v1/admin/storage/overview", &StorageAdminHandler::handleOverview, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/storage/orphans", &StorageAdminHandler::handleOrphans, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/storage/orphans/purge", &StorageAdminHandler::handlePurgeOrphans, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/storage/search", &StorageAdminHandler::handleSearch, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/system/config", &StorageAdminHandler::handleGetSystemConfig, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/system/config", &StorageAdminHandler::handleUpdateSystemConfig, {drogon::Put},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/storage/formats", &StorageAdminHandler::handleListFormats, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/storage/formats", &StorageAdminHandler::handleUpsertFormat, {drogon::Post},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/storage/formats", &StorageAdminHandler::handleDeleteFormat, {drogon::Delete},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/quotas", &StorageAdminHandler::handleListQuotas, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/quotas", &StorageAdminHandler::handleUpdateQuota, {drogon::Put},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/analytics/overview", &StorageAdminHandler::handleAnalyticsOverview, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
        {"/api/v1/admin/analytics/breakdown", &StorageAdminHandler::handleAnalyticsBreakdown, {drogon::Get},
            {"sgrn::datastore::filters::UserAuthFilter", "sgrn::datastore::filters::AdminFilter"}},
    }};

    inline static const std::array<::sgrn::IHandler<StorageAdminHandler>::item_route_config, 0> kItemRoutes = {};
};

} // namespace sgrn::datastore::handlers::storage_admin
