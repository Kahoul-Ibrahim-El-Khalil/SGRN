// sgrn/lib/datastore/src/handlers/storage-admin-internal.hpp
//
// Private surface shared by the storage-admin handler translation units
// (storage-admin.cpp, -config.cpp, -catalog.cpp, -analytics.cpp). Not a public
// header: nothing outside src/handlers/ should include it.
//
// The wire structs and query constants live in `detail`; the former
// anonymous-namespace helpers are `inline` so every TU gets one definition.
#pragma once

#include <sgrn/datastore/handlers/storage-admin.hpp>

#include <sgrn/datastore/audit/AuditLogger.hpp>
#include <sgrn/datastore/config/config.hpp>
#include <sgrn/datastore/core/db.hpp>
#include <sgrn/datastore/core/orm-glaze.hpp>
#include <sgrn/datastore/init/rate-limit.hpp>
#include <sgrn/datastore/plugins/aws/S3Client.hpp>
#include <sgrn/datastore/services/storage.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe-access.hpp>
#include <sgrn/datastore/utils/system-config.hpp>
#include <sgrn/datastore/wire/storage.hpp>

#include <drogon/HttpAppFramework.h>
#include <drogon/utils/coroutine.h>
#include <fmt/core.h>
#include <json/json.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sgrn::datastore::handlers::storage_admin::detail
{

struct PurgeOrphansRequest {
    std::string bucket;
    bool dry_run{true};
    std::size_t limit{500};
    std::vector<std::string> keys;
    std::optional<std::string> prefix;
};

struct UpdateSystemConfigRequest {
    sgrn::json::RawJson values{"{}"};
};

struct UpsertFormatRequest {
    std::string extension;
    std::string mime_type;
    bool is_compressed{true};
    bool is_allowed{true};
    std::optional<std::string> description;
};

struct StorageAdminUpdateQuotaRequest {
    std::string kind;
    std::optional<int32_t> id;
    std::optional<std::string> name;
    std::optional<std::string> domain;
    std::optional<std::optional<int64_t>> storage_limit_bytes;
    std::optional<std::optional<int64_t>> entry_count_limit;
};

struct StorageObjectInfo {
    int64_t id{0};
    std::string bucket;
    std::string object_key;
    int64_t size{0};
    int64_t original_size{0};
    bool is_compressed{false};
    std::optional<std::string> compression_algorithm;
    std::optional<int64_t> compression_level;
    std::string provider;
    std::string created_at;
};

struct StorageFileReference {
    int64_t file_id{0};
    std::string full_path;
    std::optional<int64_t> user_id;
    std::optional<int64_t> automated_service_id;
    std::string created_at;
};

static constexpr char kOrganisationStorageFileReferencesQuery[] =
    "SELECT f.id AS file_id, f.full_path, f.user_id, f.automated_service_id, f.created_at "
    "FROM storage.files f JOIN storage.file_objects fo ON fo.file_id = f.id AND fo.part_index = 0 "
    "JOIN storage.objects o ON o.id = fo.object_id "
    "LEFT JOIN core.users u ON u.id = f.user_id "
    "LEFT JOIN core.automated_services s ON s.id = f.automated_service_id "
    "WHERE o.bucket = $1 AND o.key = $2 AND COALESCE(u.organisation, s.organisation) = $3 "
    "ORDER BY f.id LIMIT 100";
static constexpr char kPlatformStorageFileReferencesQuery[] =
    "SELECT f.id AS file_id, f.full_path, f.user_id, f.automated_service_id, f.created_at "
    "FROM storage.files f JOIN storage.file_objects fo ON fo.file_id = f.id AND fo.part_index = 0 "
    "JOIN storage.objects o ON o.id = fo.object_id "
    "WHERE o.bucket = $1 AND o.key = $2 ORDER BY f.id LIMIT 100";

struct KeySearchResponse {
    std::string bucket;
    std::string key;
    std::optional<StorageObjectInfo> db;
    std::optional<std::vector<StorageFileReference>> files;
    std::optional<::sgrn::datastore::plugins::aws::S3ObjectStat> garage;
    std::optional<std::string> garage_error;
};

struct PrefixMatch {
    std::string key;
    int64_t size{0};
    std::string etag;
    bool in_db{false};
};

struct PrefixSearchResponse {
    std::string bucket;
    std::string prefix;
    std::vector<PrefixMatch> matches;
    bool truncated{false};
};

struct ObjectKeyRow {
    std::string key;
};

struct ObjectKeySizeRow {
    std::string key;
    int64_t size{0};
};

static constexpr char kObjectKeySizesQuery[] = "SELECT key, size FROM storage.objects WHERE bucket = $1 AND key LIKE $2 || '%' ESCAPE '\\'";
static constexpr char kObjectKeysByPrefixQuery[] = "SELECT key FROM storage.objects WHERE bucket = $1 AND key LIKE $2 || '%' ESCAPE '\\'";
static constexpr char kStorageObjectInfoQuery[] =
    "SELECT id, bucket, key AS object_key, size, original_size, is_compressed, compression_algorithm, compression_level,"
    " provider, created_at FROM storage.objects WHERE bucket = $1 AND key = $2";

using PurgeSkipped = ::sgrn::datastore::storage::PurgeSkipped;

using PurgeKey = ::sgrn::datastore::storage::PurgeKey;

using PurgeDryRunResponse = ::sgrn::datastore::storage::PurgeDryRunResponse;

struct PurgeResponse {
    std::string bucket;
    bool dry_run{false};
    std::vector<std::string> deleted;
    std::vector<::sgrn::datastore::plugins::aws::S3DeleteError> errors;
    std::vector<PurgeSkipped> skipped;
};

struct QuotaUpdateWithIdResponse {
    std::string kind;
    int64_t id{0};
    std::string label;
    std::optional<std::string> organisation;
    int64_t storage_used_bytes{0};
    std::optional<int64_t> storage_limit_bytes;
    int64_t entries_used{0};
    std::optional<int64_t> entry_count_limit;
};

struct QuotaUpdateOrganisationResponse {
    std::string kind;
    std::string label;
    std::optional<std::string> organisation;
    int64_t storage_used_bytes{0};
    std::optional<int64_t> storage_limit_bytes;
    int64_t entries_used{0};
    std::optional<int64_t> entry_count_limit;
};

struct QuotaUpdateRow {
    std::optional<int64_t> id;
    std::string label;
    std::optional<std::string> organisation;
    int64_t storage_used_bytes{0};
    std::optional<int64_t> storage_limit_bytes;
    int64_t entries_used{0};
    std::optional<int64_t> entry_count_limit;
};

struct UserQuotaRow {
    int64_t id{0};
    std::string email;
    std::string kind;
    std::optional<std::string> organisation;
    int64_t storage_used_bytes{0};
    std::optional<int64_t> storage_limit_bytes;
    int64_t entries_used{0};
    std::optional<int64_t> entry_count_limit;
    std::optional<int64_t> rate_limit_rpm;
    std::optional<int64_t> max_file_size_mb;
    std::optional<int64_t> preferred_chunk_size_mb;
    std::optional<int64_t> rate_limit_upload_rpm;
};

struct ServiceQuotaRow {
    int64_t id{0};
    std::string name;
    std::string kind;
    std::optional<std::string> organisation;
    int64_t storage_used_bytes{0};
    std::optional<int64_t> storage_limit_bytes;
    int64_t entries_used{0};
    std::optional<int64_t> entry_count_limit;
    std::optional<int64_t> rate_limit_rpm;
    std::optional<int64_t> max_file_size_mb;
    std::optional<int64_t> preferred_chunk_size_mb;
    std::optional<int64_t> rate_limit_upload_rpm;
};

struct DomainQuotaRow {
    int64_t id{0};
    std::string name;
    std::string path;
    std::string kind;
    std::optional<std::string> organisation;
    int64_t storage_used_bytes{0};
    std::optional<int64_t> storage_limit_bytes;
    int64_t entries_used{0};
    std::optional<int64_t> entry_count_limit;
};

struct OrganisationQuotaRow {
    std::string name;
    std::string kind;
    std::optional<std::string> organisation;
    int64_t storage_used_bytes{0};
    std::optional<int64_t> storage_limit_bytes;
    int64_t entries_used{0};
    std::optional<int64_t> entry_count_limit;
    std::optional<int64_t> rate_limit_rpm;
};

struct UserQuotasResponse {
    std::string kind;
    std::vector<UserQuotaRow> rows;
    int64_t count{0};
};
struct ServiceQuotasResponse {
    std::string kind;
    std::vector<ServiceQuotaRow> rows;
    int64_t count{0};
};
struct DomainQuotasResponse {
    std::string kind;
    std::vector<DomainQuotaRow> rows;
    int64_t count{0};
};
struct OrganisationQuotasResponse {
    std::string kind;
    std::vector<OrganisationQuotaRow> rows;
    int64_t count{0};
};

struct AnalyticsCounts {
    int64_t users{0};
    int64_t automated_services{0};
    int64_t organisations{0};
    int64_t domains{0};
    int64_t files{0};
    int64_t objects{0};
    int64_t formats{0};
    int64_t permission_grants{0};
};

struct AnalyticsEntityTotalsRow {
    int64_t count{0};
    int64_t storage_used_bytes{0};
    int64_t entries_used{0};
};

struct AnalyticsObjectTotalsRow {
    int64_t count{0};
    int64_t bytes_stored{0};
    int64_t bytes_virtual{0};
};

struct AnalyticsFormatTotalsRow {
    int64_t count{0};
    int64_t allowed{0};
};

struct AnalyticsCountRow {
    int64_t count{0};
};

struct AnalyticsBytes {
    int64_t virtual_bytes{0};
    int64_t stored{0};
    int64_t saved_by_compression{0};
};

struct AnalyticsTimeseriesEntry {
    std::string day;
    int64_t files{0};
    int64_t bytes_virtual{0};
};

struct AnalyticsExtensionEntry {
    std::string extension;
    int64_t files{0};
    int64_t bytes_virtual{0};
};

struct AnalyticsUploaderEntry {
    std::string actor;
    int64_t files{0};
    int64_t bytes_virtual{0};
};

struct AnalyticsRecentFile {
    std::string name;
    std::string extension;
    int64_t bytes_virtual{0};
    std::string actor;
    std::string at;
};

static constexpr char kPlatformAnalyticsTimeseriesQuery[] =
    "SELECT to_char(date_trunc('day', f.created_at), 'YYYY-MM-DD') AS day, COUNT(DISTINCT f.id) AS files, "
    "COALESCE(SUM(o.original_size),0) AS bytes_virtual "
    "FROM storage.files f JOIN storage.file_objects fo ON fo.file_id = f.id JOIN storage.objects o ON o.id = fo.object_id "
    "WHERE f.created_at >= now() - interval '30 days' "
    "GROUP BY 1 ORDER BY 1";
static constexpr char kOrganisationAnalyticsTimeseriesQuery[] =
    "SELECT to_char(date_trunc('day', f.created_at), 'YYYY-MM-DD') AS day, COUNT(DISTINCT f.id) AS files, "
    "COALESCE(SUM(o.original_size),0) AS bytes_virtual "
    "FROM storage.files f JOIN storage.file_objects fo ON fo.file_id = f.id JOIN storage.objects o ON o.id = fo.object_id "
    "LEFT JOIN core.users u ON u.id = f.user_id LEFT JOIN core.automated_services s ON s.id = f.automated_service_id "
    "WHERE f.created_at >= now() - interval '30 days' AND COALESCE(u.organisation, s.organisation) = $1 "
    "GROUP BY 1 ORDER BY 1";
static constexpr char kPlatformAnalyticsExtensionsQuery[] =
    "SELECT COALESCE(f.extension, '(none)') AS extension, COUNT(DISTINCT f.id) AS files, "
    "COALESCE(SUM(o.original_size),0) AS bytes_virtual "
    "FROM storage.files f JOIN storage.file_objects fo ON fo.file_id = f.id JOIN storage.objects o ON o.id = fo.object_id "
    "GROUP BY 1 ORDER BY 2 DESC LIMIT 10";
static constexpr char kOrganisationAnalyticsExtensionsQuery[] =
    "SELECT COALESCE(f.extension, '(none)') AS extension, COUNT(DISTINCT f.id) AS files, "
    "COALESCE(SUM(o.original_size),0) AS bytes_virtual "
    "FROM storage.files f JOIN storage.file_objects fo ON fo.file_id = f.id JOIN storage.objects o ON o.id = fo.object_id "
    "LEFT JOIN core.users u ON u.id = f.user_id LEFT JOIN core.automated_services s ON s.id = f.automated_service_id "
    "WHERE COALESCE(u.organisation, s.organisation) = $1 "
    "GROUP BY 1 ORDER BY 2 DESC LIMIT 10";
static constexpr char kPlatformAnalyticsUploadersQuery[] =
    "SELECT 'user:' || u.email AS actor, COUNT(DISTINCT f.id) AS files, COALESCE(SUM(o.original_size),0) AS bytes_virtual "
    "FROM storage.files f JOIN storage.file_objects fo ON fo.file_id = f.id JOIN storage.objects o ON o.id = fo.object_id "
    "JOIN core.users u ON u.id = f.user_id "
    "GROUP BY 1 "
    "UNION ALL "
    "SELECT 'service:' || s.name AS actor, COUNT(DISTINCT f.id) AS files, COALESCE(SUM(o.original_size),0) AS bytes_virtual "
    "FROM storage.files f JOIN storage.file_objects fo ON fo.file_id = f.id JOIN storage.objects o ON o.id = fo.object_id "
    "JOIN core.automated_services s ON s.id = f.automated_service_id "
    "GROUP BY 1 ORDER BY 2 DESC LIMIT 10";
static constexpr char kOrganisationAnalyticsUploadersQuery[] =
    "SELECT 'user:' || u.email AS actor, COUNT(DISTINCT f.id) AS files, COALESCE(SUM(o.original_size),0) AS bytes_virtual "
    "FROM storage.files f JOIN storage.file_objects fo ON fo.file_id = f.id JOIN storage.objects o ON o.id = fo.object_id "
    "JOIN core.users u ON u.id = f.user_id "
    "WHERE u.organisation = $1 "
    "GROUP BY 1 "
    "UNION ALL "
    "SELECT 'service:' || s.name AS actor, COUNT(DISTINCT f.id) AS files, COALESCE(SUM(o.original_size),0) AS bytes_virtual "
    "FROM storage.files f JOIN storage.file_objects fo ON fo.file_id = f.id JOIN storage.objects o ON o.id = fo.object_id "
    "JOIN core.automated_services s ON s.id = f.automated_service_id "
    "WHERE s.organisation = $1 "
    "GROUP BY 1 ORDER BY 2 DESC LIMIT 10";
static constexpr char kPlatformAnalyticsRecentFilesQuery[] =
    "SELECT f.name, COALESCE(f.extension, '') AS extension, "
    "(SELECT COALESCE(SUM(so.original_size), 0) FROM storage.file_objects fo JOIN storage.objects so ON so.id = fo.object_id "
    "WHERE fo.file_id = f.id) AS bytes_virtual, "
    "COALESCE('user:' || u.email, 'service:' || s.name, '(unknown)') AS actor, "
    "to_char(f.created_at, 'YYYY-MM-DD HH24:MI:SS') AS at "
    "FROM storage.files f "
    "LEFT JOIN core.users u ON u.id = f.user_id LEFT JOIN core.automated_services s ON s.id = f.automated_service_id "
    "ORDER BY f.id DESC LIMIT 10";
static constexpr char kOrganisationAnalyticsRecentFilesQuery[] =
    "SELECT f.name, COALESCE(f.extension, '') AS extension, "
    "(SELECT COALESCE(SUM(so.original_size), 0) FROM storage.file_objects fo JOIN storage.objects so ON so.id = fo.object_id "
    "WHERE fo.file_id = f.id) AS bytes_virtual, "
    "COALESCE('user:' || u.email, 'service:' || s.name, '(unknown)') AS actor, "
    "to_char(f.created_at, 'YYYY-MM-DD HH24:MI:SS') AS at "
    "FROM storage.files f "
    "LEFT JOIN core.users u ON u.id = f.user_id LEFT JOIN core.automated_services s ON s.id = f.automated_service_id "
    "WHERE COALESCE(u.organisation, s.organisation) = $1 "
    "ORDER BY f.id DESC LIMIT 10";

struct AnalyticsOverviewResponse {
    AnalyticsCounts counts;
    AnalyticsBytes bytes;
    std::vector<AnalyticsTimeseriesEntry> timeseries_30d;
    std::vector<AnalyticsExtensionEntry> top_extensions;
    std::vector<AnalyticsUploaderEntry> top_uploaders;
    std::vector<AnalyticsRecentFile> recent_files;
};

struct AnalyticsBreakdownActorRow {
    std::string slice;
    int64_t files{0};
    int64_t bytes_virtual{0};
    int64_t actors{0};
};

struct AnalyticsBreakdownQuotaRow {
    std::string slice;
    int64_t files{0};
    int64_t bytes_virtual{0};
    std::optional<int64_t> storage_limit_bytes;
    std::optional<int64_t> entry_count_limit;
};

struct AnalyticsBreakdownQuotaDbRow {
    std::string slice;
    int64_t files{0};
    int64_t bytes_virtual{0};
    int64_t storage_cap{0};
    int64_t entry_cap{0};
};

struct AnalyticsBreakdownActorResponse {
    std::string kind;
    std::vector<AnalyticsBreakdownActorRow> rows;
    int64_t count{0};
};

struct AnalyticsBreakdownQuotaResponse {
    std::string kind;
    std::vector<AnalyticsBreakdownQuotaRow> rows;
    int64_t count{0};
};

} // namespace sgrn::datastore::handlers::storage_admin::detail

template <>
struct glz::meta<sgrn::datastore::handlers::storage_admin::detail::AnalyticsBytes> {
    using T = sgrn::datastore::handlers::storage_admin::detail::AnalyticsBytes;
    static constexpr auto value =
        glz::object("virtual", &T::virtual_bytes, "stored", &T::stored, "saved_by_compression", &T::saved_by_compression);
};

namespace sgrn::datastore::handlers::storage_admin::detail
{
using drogon::Task;


// Upstream (Garage) failures surface as 502, distinct from our own 500s.
struct S3UpstreamError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

using drogon::Task;

template <typename Outcome>
Task<typename Outcome::value_type> s3unwrap(Task<Outcome> t_task) {
    auto res = co_await std::move(t_task);
    if (res.hasError()) {
        throw S3UpstreamError(res.error().message);
    }
    co_return std::move(res).value();
}

template <typename Outcome>
[[maybe_unused]] Task<void> s3check(Task<Outcome> t_task) {
    auto res = co_await std::move(t_task);
    if (res.hasError()) {
        throw S3UpstreamError(res.error().message);
    }
    co_return;
}

inline std::string defaultBucket() {
    const Json::Value cfg = drogon::app().getCustomConfig();
    if (cfg.isMember("s3") && cfg["s3"].isMember("default_bucket")) {
        return cfg["s3"]["default_bucket"].asString();
    }
    return "sgrn-uploads";
}

inline ::sgrn::datastore::plugins::aws::S3Client* s3plugin() {
    auto s3 = drogon::app().getPlugin<::sgrn::datastore::plugins::aws::S3Client>();
    if (!s3) {
        throw std::runtime_error("S3 plugin not initialised");
    }
    return s3;
}

inline std::string queryParam(const drogon::HttpRequestPtr& tsp_req, const std::string& t_key, const std::string& t_dflt) {
    const auto& params = tsp_req->getParameters();
    const auto it = params.find(t_key);
    return it == params.end() ? t_dflt : it->second;
}

inline std::size_t clampParam(const std::string& t_raw, std::size_t t_dflt, std::size_t t_max) {
    if (t_raw.empty()) {
        return t_dflt;
    }
    try {
        const auto v = static_cast<std::size_t>(std::stoull(t_raw));
        return std::min(v, t_max) == 0 ? t_dflt : std::min(v, t_max);
    } catch (const std::exception&) {
        return t_dflt;
    }
}

// Escape LIKE wildcards in an admin-supplied prefix (object keys are
// base64url and routinely contain '_').
inline std::string escapeLike(std::string_view t_in) {
    std::string out;
    out.reserve(t_in.size());
    for (char c : t_in) {
        if (c == '\\' || c == '%' || c == '_') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

// COUNT(*)/SUM(size) come back as int8 text; Json::Int64 keeps the full
// range onto the wire (and selects the unambiguous Value overload).
inline Json::Int64 fieldInt(const drogon::orm::Field& t_field) {
    if (t_field.isNull()) {
        return 0;
    }
    try {
        return std::stoll(t_field.as<std::string>());
    } catch (const std::exception&) {
        return 0;
    }
}

struct GarageKey {
    std::string key;
    int64_t size = 0;
    std::string etag;
};

struct GarageScan {
    std::vector<GarageKey> keys;
    bool truncated = false;
    uint32_t pages = 0;
};

using StorageStats = ::sgrn::datastore::storage::DbCensus;

using OverviewBucket = ::sgrn::datastore::storage::StorageBucketCensus;

using OverviewResponse = ::sgrn::datastore::storage::StorageOverview;

using OrphanObject = ::sgrn::datastore::storage::StorageOrphanKey;

using OrphanSize = ::sgrn::datastore::storage::StorageOrphanSize;

using OrphansResponse = ::sgrn::datastore::storage::StorageOrphansReport;

struct FormatRecord {
    std::string extension;
    std::string mime_type;
    bool is_compressed{false};
    bool is_allowed{false};
    std::optional<std::string> description;
    std::string created_at;
};

struct FormatsResponse {
    std::vector<FormatRecord> formats;
    int64_t count{0};
};

struct FormatUpsertResponse {
    std::string extension;
    std::string mime_type;
    bool is_compressed{false};
    bool is_allowed{false};
    std::optional<std::string> description;
    std::string created_at;
    bool created{false};
};

struct FormatDeleteResponse {
    std::string extension;
};

struct FormatConflictResponse {
    std::string extension;
    int64_t referencing_files{0};
    std::string hint;
};

// Page through ListObjectsV2 (server-side prefix filter) up to max_pages.
inline Task<GarageScan> scanBucketKeys(
    ::sgrn::datastore::plugins::aws::S3Client* s3, const std::string& t_bucket, const std::string& t_prefix, uint32_t t_max_pages) {
    GarageScan scan;
    std::string token;
    for (uint32_t page = 0; page < t_max_pages; ++page) {
        const auto res = co_await s3unwrap(s3->listObjects(t_bucket, t_prefix, token, 1000));
        for (const auto& obj : res.objects) {
            GarageKey k;
            k.key = obj.key;
            k.size = obj.size;
            k.etag = obj.etag;
            scan.keys.push_back(std::move(k));
        }
        ++scan.pages;
        if (!res.is_truncated) {
            co_return scan;
        }
        token = res.next_continuation_token;
        if (token.empty()) {
            break;
        }
    }
    // Loop ended on the page budget with more data behind the token.
    scan.truncated = true;
    co_return scan;
}


} // namespace sgrn::datastore::handlers::storage_admin::detail
