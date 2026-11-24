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

namespace
{

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

std::string defaultBucket() {
    const Json::Value cfg = drogon::app().getCustomConfig();
    if (cfg.isMember("s3") && cfg["s3"].isMember("default_bucket")) {
        return cfg["s3"]["default_bucket"].asString();
    }
    return "sgrn-uploads";
}

::sgrn::datastore::plugins::aws::S3Client* s3plugin() {
    auto s3 = drogon::app().getPlugin<::sgrn::datastore::plugins::aws::S3Client>();
    if (!s3) {
        throw std::runtime_error("S3 plugin not initialised");
    }
    return s3;
}

std::string queryParam(const drogon::HttpRequestPtr& tsp_req, const std::string& t_key, const std::string& t_dflt) {
    const auto& params = tsp_req->getParameters();
    const auto it = params.find(t_key);
    return it == params.end() ? t_dflt : it->second;
}

std::size_t clampParam(const std::string& t_raw, std::size_t t_dflt, std::size_t t_max) {
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
std::string escapeLike(std::string_view t_in) {
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
Json::Int64 fieldInt(const drogon::orm::Field& t_field) {
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
Task<GarageScan> scanBucketKeys(
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

} // namespace

namespace sgrn::datastore::handlers::storage_admin
{
using namespace drogon;
using namespace detail;

Task<HttpResponsePtr> StorageAdminHandler::handleOverview(HttpRequestPtr tsp_req) {
    try {
        auto s3 = s3plugin();
        const uint32_t max_pages = static_cast<uint32_t>(clampParam(queryParam(tsp_req, "max_pages", ""), 10, 100));
        const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        const auto buckets_res = co_await s3unwrap(s3->listBuckets());
        std::vector<OverviewBucket> buckets;
        buckets.reserve(buckets_res.buckets.size());
        for (const auto& b : buckets_res.buckets) {
            const std::string& name = b.name;
            OverviewBucket entry;
            entry.name = name;

            const GarageScan scan = co_await scanBucketKeys(s3, name, "", max_pages);
            Json::Int64 bytes = 0;
            for (const auto& k : scan.keys) {
                bytes += k.size;
            }
            entry.garage.objects = static_cast<int64_t>(scan.keys.size());
            entry.garage.bytes = bytes;
            entry.garage.truncated = scan.truncated;
            entry.garage.pages = scan.pages;

            auto objs = is_platform ? co_await sgrn::datastore::core::execSqlCoroVec(
                                          db, "SELECT COUNT(*), COALESCE(SUM(size),0) FROM storage.objects WHERE bucket = $1", {name})
                                    : co_await sgrn::datastore::core::execSqlCoroVec(db,
                                          "SELECT COUNT(DISTINCT o.id), COALESCE(SUM(o.size),0) FROM storage.objects o "
                                          "JOIN storage.file_objects fo ON fo.object_id = o.id "
                                          "JOIN storage.files f ON f.id = fo.file_id "
                                          "LEFT JOIN core.users u ON u.id = f.user_id "
                                          "LEFT JOIN core.automated_services s ON s.id = f.automated_service_id "
                                          "WHERE o.bucket = $1 AND COALESCE(u.organisation, s.organisation) = $2",
                                          {name, session_org});

            auto files = is_platform ? co_await sgrn::datastore::core::execSqlCoroVec(db,
                                           "SELECT COUNT(*) FROM storage.files f JOIN storage.file_objects fo ON fo.file_id = f.id AND "
                                           "fo.part_index = 0 JOIN "
                                           "storage.objects o ON o.id = fo.object_id WHERE o.bucket = $1",
                                           {name})
                                     : co_await sgrn::datastore::core::execSqlCoroVec(db,
                                           "SELECT COUNT(*) FROM storage.files f "
                                           "JOIN storage.file_objects fo ON fo.file_id = f.id AND fo.part_index = 0 "
                                           "JOIN storage.objects o ON o.id = fo.object_id "
                                           "LEFT JOIN core.users u ON u.id = f.user_id "
                                           "LEFT JOIN core.automated_services s ON s.id = f.automated_service_id "
                                           "WHERE o.bucket = $1 AND COALESCE(u.organisation, s.organisation) = $2",
                                           {name, session_org});
            entry.db.objects = fieldInt(objs[0][0]);
            entry.db.bytes = fieldInt(objs[0][1]);
            entry.db.files = fieldInt(files[0][0]);
            buckets.push_back(std::move(entry));
        }
        co_return sgrn::createTypedResponse(tsp_req, OverviewResponse{std::move(buckets)}, k200OK);
    } catch (const S3UpstreamError& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k502BadGateway, "StorageAdmin"});
    } catch (const std::exception& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k500InternalServerError, "StorageAdmin"});
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handleOrphans(HttpRequestPtr tsp_req) {
    try {
        auto s3 = s3plugin();
        const std::string bucket = queryParam(tsp_req, "bucket", defaultBucket());
        const std::string prefix = queryParam(tsp_req, "prefix", "");
        const std::size_t limit = clampParam(queryParam(tsp_req, "limit", ""), 500, 5000);
        const uint32_t max_pages = static_cast<uint32_t>(clampParam(queryParam(tsp_req, "max_pages", ""), 10, 100));

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        const GarageScan scan = co_await scanBucketKeys(s3, bucket, prefix, max_pages);

        auto db_rows = co_await sgrn::datastore::core::execSqlCoroVec(db, detail::kObjectKeySizesQuery, {bucket, escapeLike(prefix)});
        std::unordered_map<std::string, int64_t> db_sizes;
        db_sizes.reserve(db_rows.size() * 2);
        for (const auto& row : sgrn::orm::fromResult<detail::ObjectKeySizeRow, detail::kObjectKeySizesQuery>(db_rows)) {
            db_sizes[row.key] = row.size;
        }

        std::vector<OrphanObject> garage_only;
        garage_only.reserve(limit);
        int64_t garage_only_bytes = 0;
        std::size_t garage_only_count = 0;
        std::unordered_set<std::string> seen;
        seen.reserve(scan.keys.size() * 2);
        for (const auto& k : scan.keys) {
            seen.insert(k.key);
            if (db_sizes.find(k.key) == db_sizes.end()) {
                ++garage_only_count;
                garage_only_bytes += k.size;
                if (garage_only.size() < limit) {
                    garage_only.push_back({k.key, k.size, k.etag});
                }
            }
        }

        // DB rows with no Garage object: HEAD-verify each (bounded by limit)
        // so the report is exact even when the Garage scan truncated.
        std::vector<OrphanSize> db_missing;
        db_missing.reserve(limit);
        std::size_t db_missing_unchecked = 0;
        for (const auto& [key, size] : db_sizes) {
            if (seen.find(key) != seen.end()) {
                continue;
            }
            if (db_missing.size() >= limit) {
                ++db_missing_unchecked;
                continue;
            }
            const bool present = co_await s3unwrap(s3->exists(bucket, key));
            if (!present) {
                db_missing.push_back({key, size});
            }
        }

        co_return sgrn::createTypedResponse(tsp_req,
            OrphansResponse{
                bucket,
                prefix,
                static_cast<int64_t>(scan.keys.size()),
                scan.truncated,
                std::move(garage_only),
                static_cast<int64_t>(garage_only_count),
                garage_only_bytes,
                std::move(db_missing),
                static_cast<int64_t>(db_missing_unchecked),
            },
            k200OK);
    } catch (const S3UpstreamError& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k502BadGateway, "StorageAdmin"});
    } catch (const std::exception& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k500InternalServerError, "StorageAdmin"});
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handlePurgeOrphans(HttpRequestPtr tsp_req) {
    try {
        if (!sgrn::datastore::isPlatformAdmin(tsp_req)) {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"Forbidden: Platform administrator privileges required", k403Forbidden, "StorageAdmin"});
        }

        auto req_res = parseTypedRequest<detail::PurgeOrphansRequest>(tsp_req);
        if (req_res.hasError()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{"Expected JSON body", k400BadRequest, "StorageAdmin"});
        }
        const auto& req = req_res.value();
        auto s3 = s3plugin();
        std::string bucket = req.bucket.empty() ? defaultBucket() : req.bucket;
        bool dry_run = req.dry_run;
        std::size_t limit = req.limit == 0 ? 500 : std::min<std::size_t>(req.limit, 5000);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        // Candidate set: explicit keys, or a classified prefix scan.
        std::vector<std::string> candidates;
        if (!req.keys.empty()) {
            for (const auto& k : req.keys) {
                if (k.empty()) {
                    continue;
                }
                if (candidates.size() >= limit) {
                    break;
                }
                candidates.push_back(k);
            }
        } else if (req.prefix.has_value() && !req.prefix->empty()) {
            const std::string& prefix_str = *req.prefix;
            const uint32_t max_pages = static_cast<uint32_t>(clampParam(queryParam(tsp_req, "max_pages", ""), 10, 100));
            const GarageScan scan = co_await scanBucketKeys(s3, bucket, prefix_str, max_pages);
            auto prefix_rows =
                co_await sgrn::datastore::core::execSqlCoroVec(db, detail::kObjectKeysByPrefixQuery, {bucket, escapeLike(prefix_str)});
            std::unordered_set<std::string> db_keys;
            db_keys.reserve(prefix_rows.size() * 2);
            for (const auto& row : sgrn::orm::fromResult<detail::ObjectKeyRow, detail::kObjectKeysByPrefixQuery>(prefix_rows)) {
                db_keys.insert(row.key);
            }
            for (const auto& k : scan.keys) {
                if (candidates.size() >= limit) {
                    break;
                }
                if (db_keys.find(k.key) == db_keys.end()) {
                    candidates.push_back(k.key);
                }
            }
        } else {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"Supply non-empty keys[] or a prefix (unbounded purge refused)", k400BadRequest, "StorageAdmin"});
        }

        // Re-verify every candidate against the DB immediately before acting:
        // a concurrent upload must never lose its bytes to an admin purge.
        std::vector<PurgeSkipped> skipped;
        skipped.reserve(candidates.size());
        std::vector<std::string> doomed;
        doomed.reserve(candidates.size());
        for (const auto& key : candidates) {
            auto hit = co_await sgrn::datastore::core::execSqlCoroVec(
                db, "SELECT 1 FROM storage.objects WHERE bucket = $1 AND key = $2 LIMIT 1", {bucket, key});
            if (!hit.empty()) {
                skipped.push_back({key, "referenced by storage.objects"});
            } else {
                doomed.push_back(key);
            }
        }

        if (dry_run) {
            const int64_t would_delete_count = static_cast<int64_t>(doomed.size());
            std::vector<PurgeKey> would_delete;
            would_delete.reserve(doomed.size());
            for (const auto& key : doomed) {
                would_delete.push_back({key});
            }
            co_return sgrn::createTypedResponse(tsp_req,
                PurgeDryRunResponse{
                    bucket,
                    true,
                    std::move(would_delete),
                    would_delete_count,
                    std::move(skipped),
                },
                k200OK);
        }

        std::vector<std::string> deleted;
        std::vector<::sgrn::datastore::plugins::aws::S3DeleteError> errors;
        if (!doomed.empty()) {
            auto res = co_await s3unwrap(s3->deleteFiles(bucket, doomed));
            deleted = std::move(res.deleted);
            errors = std::move(res.errors);
        }
        co_return sgrn::createTypedResponse(tsp_req,
            PurgeResponse{
                bucket,
                false,
                std::move(deleted),
                std::move(errors),
                std::move(skipped),
            },
            k200OK);
    } catch (const S3UpstreamError& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k502BadGateway, "StorageAdmin"});
    } catch (const std::exception& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k500InternalServerError, "StorageAdmin"});
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handleSearch(HttpRequestPtr tsp_req) {
    try {
        auto s3 = s3plugin();
        const std::string bucket = queryParam(tsp_req, "bucket", defaultBucket());
        const std::string key = queryParam(tsp_req, "key", "");
        const std::string prefix = queryParam(tsp_req, "prefix", "");
        const std::size_t limit = clampParam(queryParam(tsp_req, "limit", ""), 100, 1000);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        if (!key.empty()) {
            KeySearchResponse out;
            out.bucket = bucket;
            out.key = key;
            auto objs = co_await sgrn::datastore::core::execSqlCoroVec(db, detail::kStorageObjectInfoQuery, {bucket, key});
            if (objs.empty()) {
                out.db = std::nullopt;
            } else {
                out.db = sgrn::orm::fromRow<StorageObjectInfo, detail::kStorageObjectInfoQuery>(objs[0]);

                const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);
                const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

                const char* file_sql = nullptr;
                std::vector<std::string> file_binds = {bucket, key};
                if (!is_platform) {
                    file_sql = detail::kOrganisationStorageFileReferencesQuery;
                    file_binds.push_back(session_org);
                } else {
                    file_sql = detail::kPlatformStorageFileReferencesQuery;
                }

                auto files = co_await sgrn::datastore::core::execSqlCoroVec(db, file_sql, file_binds);
                out.files = is_platform
                                ? sgrn::orm::fromResult<StorageFileReference, detail::kPlatformStorageFileReferencesQuery>(files)
                                : sgrn::orm::fromResult<StorageFileReference, detail::kOrganisationStorageFileReferencesQuery>(files);
            }
            try {
                out.garage = co_await s3unwrap(s3->statObject(bucket, key));
            } catch (const S3UpstreamError& e) {
                out.garage_error = e.what();
            }
            co_return sgrn::createTypedResponse(tsp_req, out, k200OK);
        }

        if (!prefix.empty()) {
            const auto res = co_await s3unwrap(s3->listObjects(bucket, prefix, "", static_cast<uint32_t>(limit)));
            auto match_rows = co_await sgrn::datastore::core::execSqlCoroVec(db,
                fmt::format("SELECT key FROM storage.objects WHERE bucket = $1 AND key LIKE $2 || '%' ESCAPE '\\' LIMIT {}", limit),
                {bucket, escapeLike(prefix)});
            std::unordered_set<std::string> db_keys;
            db_keys.reserve(match_rows.size() * 2);
            for (const auto& row : sgrn::orm::fromResultByName<detail::ObjectKeyRow>(match_rows)) {
                db_keys.insert(row.key);
            }
            std::vector<PrefixMatch> matches;
            matches.reserve(res.objects.size());
            for (const auto& obj : res.objects) {
                matches.push_back({obj.key, obj.size, obj.etag, db_keys.find(obj.key) != db_keys.end()});
            }
            co_return sgrn::createTypedResponse(
                tsp_req, PrefixSearchResponse{bucket, prefix, std::move(matches), res.is_truncated}, k200OK);
        }

        co_return sgrn::createTypedResponse(
            BackendErrorResponse{"Supply exactly one of key=<exact hash> or prefix=<hash prefix>", k400BadRequest, "StorageAdmin"});
    } catch (const S3UpstreamError& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k502BadGateway, "StorageAdmin"});
    } catch (const std::exception& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k500InternalServerError, "StorageAdmin"});
    }
}

// ============================================================================
// System config (dashboard-tunable sgrn.json)
// ============================================================================

namespace
{

// Restart-required leaf paths changed since boot. In-memory by design: after
// a restart every saved value is live, so there is nothing pending anymore.
std::mutex g_pending_restart_mutex;
std::set<std::string> g_pending_restart;

std::vector<std::string> splitConfigPath(const std::string& t_dotted) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : t_dotted) {
        if (c == '.') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    parts.push_back(cur);
    return parts;
}

const Json::Value* lookupPath(const Json::Value& t_root, const std::string& t_dotted) {
    const Json::Value* node = &t_root;
    for (const auto& part : splitConfigPath(t_dotted)) {
        if (!node->isObject() || !node->isMember(part)) {
            return nullptr;
        }
        node = &(*node)[part];
    }
    return node;
}

void assignPath(Json::Value& t_root, const std::string& t_dotted, const Json::Value& t_value) {
    Json::Value* node = &t_root;
    const auto parts = splitConfigPath(t_dotted);
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
        Json::Value& child = (*node)[parts[i]];
        if (!child.isObject()) {
            child = Json::Value(Json::objectValue);
        }
        node = &child;
    }
    (*node)[parts.back()] = t_value;
}

std::string readConfigFile(const std::string& t_path, Json::Value& o_root) {
    std::ifstream in(t_path, std::ios::binary);
    if (!in) {
        return "cannot open config file: " + t_path;
    }
    const std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(raw.data(), raw.data() + raw.size(), &o_root, &errors)) {
        return "config file is not valid JSON: " + errors;
    }
    if (!o_root.isObject()) {
        return "config file root is not a JSON object";
    }
    return {};
}

// Backup current file to `<path>.bak`, then replace atomically via tmp +
// rename so a crash mid-save never leaves a half-written sgrn.json.
std::string atomicWriteConfig(const std::string& t_path, const Json::Value& t_root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::copy_file(t_path, t_path + ".bak", fs::copy_options::overwrite_existing, ec);
    ec.clear();
    const std::string tmp = t_path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) {
            return "cannot write temp config file (check ownership/permissions): " + tmp;
        }
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "  ";
        out << Json::writeString(writer, t_root);
        out.flush();
        if (!out) {
            return "failed while writing temp config file: " + tmp;
        }
    }
    fs::rename(tmp, t_path, ec);
    if (ec) {
        std::error_code rm_ec;
        fs::remove(tmp, rm_ec);
        return "atomic replace failed: " + ec.message();
    }
    return {};
}

std::string fieldTypeName(sgrn::datastore::sysconfig::FieldType t_type) {
    using sgrn::datastore::sysconfig::FieldType;
    switch (t_type) {
        case FieldType::UInt:
            return "uint";
        case FieldType::Double:
            return "double";
        case FieldType::String:
            return "string";
        case FieldType::Bool:
            return "bool";
        case FieldType::StringArray:
            return "string[]";
    }
    return "string";
}

Json::Value systemConfigSchemaJson() {
    Json::Value schema = Json::arrayValue;
    for (const auto& spec : sgrn::datastore::sysconfig::systemConfigSchema()) {
        Json::Value e;
        e["path"] = spec.path;
        e["type"] = fieldTypeName(spec.type);
        e["hot"] = spec.hot;
        e["section"] = spec.section;
        e["label"] = spec.label;
        if (spec.type == sgrn::datastore::sysconfig::FieldType::UInt) {
            e["min"] = Json::UInt64(spec.min_value);
            if (spec.max_value != 0) {
                e["max"] = Json::UInt64(spec.max_value);
            }
        }
        schema.append(std::move(e));
    }
    return schema;
}

// Dashboard values: exactly the schema leaves, read from the sanitized file
// (secrets can never appear here — sanitizeForAdmin redacts them first).
Json::Value systemConfigValuesJson(const Json::Value& t_sanitized_root) {
    Json::Value values = Json::Value(Json::objectValue);
    for (const auto& spec : sgrn::datastore::sysconfig::systemConfigSchema()) {
        if (const Json::Value* node = lookupPath(t_sanitized_root, spec.path)) {
            if (!node->isNull()) {
                assignPath(values, spec.path, *node);
            }
        }
    }
    return values;
}

bool startsWithPath(const std::string& t_path, const std::string& t_prefix) {
    return t_path.size() > t_prefix.size() && t_path.compare(0, t_prefix.size(), t_prefix) == 0 && t_path[t_prefix.size()] == '.';
}

struct SystemConfigResponse {
    std::string config_path;
    sgrn::json::RawJson values{"{}"};
    sgrn::json::RawJson schema{"[]"};
    sgrn::json::RawJson live{"{}"};
    std::vector<std::string> pending_restart;
};

struct SystemConfigUpdateResponse {
    std::string config_path;
    std::string backup_path;
    std::vector<std::string> applied_hot;
    std::vector<std::string> restart_required;
    std::vector<std::string> pending_restart;
    std::vector<std::string> warnings;
};

struct SystemConfigAuditDetails {
    std::vector<std::string> applied_hot;
    std::vector<std::string> restart_required;
    std::string config_path;
};

std::string compactJson(const Json::Value& t_value) {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    return Json::writeString(writer, t_value);
}

} // namespace

Task<HttpResponsePtr> StorageAdminHandler::handleGetSystemConfig(HttpRequestPtr tsp_req) {
    if (!sgrn::datastore::isPlatformAdmin(tsp_req)) {
        co_return sgrn::createTypedResponse(
            BackendErrorResponse{"Forbidden: Platform administrator privileges required", k403Forbidden, "SystemConfig"});
    }
    try {
        const std::string path = sgrn::datastore::config::currentConfigPath();
        Json::Value root;
        if (const std::string err = readConfigFile(path, root); !err.empty()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{err, k500InternalServerError, "SystemConfig"});
        }
        const auto live_s3 = sgrn::json::write(sgrn::datastore::services::storage::currentStorageConfig().toJsonConfig());
        if (live_s3.hasError()) {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"Failed to serialize live storage configuration", k500InternalServerError, "SystemConfig"});
        }
        const std::string live_json = "{\"custom_config\":{\"s3\":" + live_s3.value() + "}}";
        // Effective-now values (live in-memory). Identical to `values` for
        // every hot key — PUT applies synchronously — and the source of truth
        // the upload path actually reads. Differs only if the file was
        // hand-edited without restarting for restart-required keys.
        std::vector<std::string> pending_restart;
        {
            std::lock_guard lock(g_pending_restart_mutex);
            pending_restart.assign(g_pending_restart.begin(), g_pending_restart.end());
        }
        const Json::Value values = systemConfigValuesJson(sgrn::datastore::sysconfig::sanitizeForAdmin(root));
        const Json::Value schema = systemConfigSchemaJson();
        co_return sgrn::createTypedResponse(tsp_req,
            SystemConfigResponse{path, sgrn::json::RawJson(compactJson(values)), sgrn::json::RawJson(compactJson(schema)),
                sgrn::json::RawJson(live_json), std::move(pending_restart)},
            k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k500InternalServerError, "SystemConfig"});
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handleUpdateSystemConfig(HttpRequestPtr tsp_req) {
    if (!sgrn::datastore::isPlatformAdmin(tsp_req)) {
        co_return sgrn::createTypedResponse(
            BackendErrorResponse{"Forbidden: Platform administrator privileges required", k403Forbidden, "SystemConfig"});
    }
    try {
        auto req_res = parseTypedRequest<detail::UpdateSystemConfigRequest>(tsp_req);
        if (req_res.hasError() || req_res->values.str.empty()) {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"Expected JSON body {\"values\": {...}}", k400BadRequest, "SystemConfig"});
        }
        Json::CharReaderBuilder builder;
        Json::Value values_json;
        std::string errs;
        const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
        const auto& val_str = req_res->values.str;
        if (!reader->parse(val_str.data(), val_str.data() + val_str.size(), &values_json, &errs) || !values_json.isObject()) {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"Expected JSON body {\"values\": {...}}", k400BadRequest, "SystemConfig"});
        }
        const std::string path = sgrn::datastore::config::currentConfigPath();
        Json::Value root;
        if (const std::string err = readConfigFile(path, root); !err.empty()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{err, k500InternalServerError, "SystemConfig"});
        }

        sgrn::datastore::sysconfig::ApplyReport report;
        if (const std::string err = sgrn::datastore::sysconfig::applyAdminUpdate(root, values_json, report); !err.empty()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{err, k400BadRequest, "SystemConfig"});
        }
        if (const std::string err = atomicWriteConfig(path, root); !err.empty()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{err, k500InternalServerError, "SystemConfig"});
        }

        // --- hot-apply -------------------------------------------------------
        // s3.*: republish the live StorageConfig (per-request snapshots pick
        // it up on the next upload — never mid-upload).
        bool touched_s3 = false;
        bool touched_rl = false;
        for (const auto& p : report.hot) {
            touched_s3 = touched_s3 || startsWithPath(p, "custom_config.s3");
            touched_rl = touched_rl || startsWithPath(p, "custom_config.rate_limiting");
        }
        for (const auto& p : report.restart) {
            touched_s3 = touched_s3 || startsWithPath(p, "custom_config.s3");
            touched_rl = touched_rl || startsWithPath(p, "custom_config.rate_limiting");
        }
        std::vector<std::string> applied_hot(report.hot.begin(), report.hot.end());
        std::vector<std::string> restart_required(report.restart.begin(), report.restart.end());
        if (touched_s3 && root.isMember("custom_config") && root["custom_config"].isMember("s3")) {
            sgrn::datastore::services::storage::publishStorageConfig(
                sgrn::datastore::services::storage::StorageConfig::fromS3Json(root["custom_config"]["s3"]));
        }
        if (touched_rl && root.isMember("custom_config")) {
            // No-op when the advice was never registered (disabled at boot):
            // report those paths as restart-required instead of pretending.
            if (sgrn::datastore::ratelimit::rateLimitHolder()->enabled_at_boot) {
                sgrn::datastore::ratelimit::publishRateLimitConfig(
                    sgrn::datastore::ratelimit::RateLimitConfig::fromJson(root["custom_config"]));
            } else {
                std::vector<std::string> still_hot;
                for (const auto& p : applied_hot) {
                    if (startsWithPath(p, "custom_config.rate_limiting")) {
                        restart_required.push_back(p);
                    } else {
                        still_hot.push_back(p);
                    }
                }
                applied_hot.swap(still_hot);
            }
        }

        {
            std::lock_guard lock(g_pending_restart_mutex);
            for (const auto& p : restart_required) {
                g_pending_restart.insert(p);
            }
        }
        std::vector<std::string> pending_restart;
        {
            std::lock_guard lock(g_pending_restart_mutex);
            pending_restart.assign(g_pending_restart.begin(), g_pending_restart.end());
        }
        std::vector<std::string> warnings(report.warnings.begin(), report.warnings.end());

        // --- Audit: system.config_updated ------------------------------------
        {
            auto db_res = sgrn::datastore::core::getDbClient();
            if (!db_res.hasError()) {
                std::string config_org;
                int32_t actor_id = 0;
                std::string actor_name;
                if (tsp_req->attributes()->find("auth_context")) {
                    const auto& auth = tsp_req->attributes()->get<filters::AuthContext>("auth_context");
                    actor_id = auth.user.id;
                    actor_name = auth.user.email;
                    config_org = auth.user.organisation;
                } else if (tsp_req->attributes()->find("session_claims")) {
                    const auto& user = tsp_req->attributes()->get<::sgrn::datastore::auth::SessionClaims>("session_claims").user;
                    actor_id = user.id;
                    actor_name = user.email;
                    config_org = user.organisation;
                }
                co_await sgrn::datastore::audit::AuditLogger::log(db_res.value(), config_org, "user", actor_id, actor_name,
                    "system.config_updated", "system_config", path, tsp_req->getPeerAddr().toIp(),
                    SystemConfigAuditDetails{applied_hot, restart_required, path}, "success");
            }
        }

        co_return sgrn::createTypedResponse(tsp_req,
            SystemConfigUpdateResponse{
                path, path + ".bak", std::move(applied_hot), std::move(restart_required), std::move(pending_restart), std::move(warnings)},
            k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k500InternalServerError, "SystemConfig"});
    }
}

// ============================================================================
// Formats registry + quota ledger
// ============================================================================

namespace
{

inline std::string lowerAscii(std::string t_in) {
    return sgrn::utils::strings::toLower(std::move(t_in));
}

[[maybe_unused]] inline bool jsonBool(const Json::Value& t_v, bool t_dflt) {
    return t_v.isBool() ? t_v.asBool() : t_dflt;
}

} // namespace

Task<HttpResponsePtr> StorageAdminHandler::handleListFormats(HttpRequestPtr tsp_req) {
    try {
        const std::string search = lowerAscii(queryParam(tsp_req, "search", ""));
        const std::string allowed = lowerAscii(queryParam(tsp_req, "allowed", ""));
        if (!allowed.empty() && allowed != "true" && allowed != "false") {
            co_return sgrn::createTypedResponse(BackendErrorResponse{"allowed must be true or false", k400BadRequest, "StorageAdmin"});
        }
        const std::size_t limit = clampParam(queryParam(tsp_req, "limit", ""), 500, 5000);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        static constexpr char kQuery[] =
            "SELECT extension, mime_type, is_compressed, is_allowed, description, created_at FROM storage.formats";

        // $1 search (empty matches all), $2 allowed filter (empty = any).
        auto rows = co_await sgrn::datastore::core::execSqlCoroVec(db,
            std::string(kQuery) +
                " WHERE (extension LIKE '%' || $1 || '%' ESCAPE '\\' OR mime_type ILIKE '%' || $1 || '%' ESCAPE '\\') "
                "AND ($2 = '' OR is_allowed = ($2 = 'true')) "
                "ORDER BY extension LIMIT " +
                std::to_string(limit),
            {escapeLike(search), allowed});

        co_return sgrn::createTypedResponse(tsp_req,
            FormatsResponse{
                .formats = sgrn::orm::fromResult<FormatRecord, kQuery>(rows),
                .count = static_cast<int64_t>(rows.size()),
            },
            k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k500InternalServerError, "StorageAdmin"});
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handleUpsertFormat(HttpRequestPtr tsp_req) {
    try {
        auto req_res = parseTypedRequest<detail::UpsertFormatRequest>(tsp_req);
        if (req_res.hasError()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{"Expected JSON body", k400BadRequest, "StorageAdmin"});
        }
        const auto& req = req_res.value();
        if (req.extension.empty()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{"extension (string) is required", k400BadRequest, "StorageAdmin"});
        }
        if (req.mime_type.empty()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{"mime_type (string) is required", k400BadRequest, "StorageAdmin"});
        }
        const std::string ext = lowerAscii(req.extension);
        if (const std::string err = sgrn::datastore::sysconfig::validateFormatExtension(ext); !err.empty()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{err, k400BadRequest, "StorageAdmin"});
        }
        const std::string mime = req.mime_type;
        if (const std::string err = sgrn::datastore::sysconfig::validateMimeType(mime); !err.empty()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{err, k400BadRequest, "StorageAdmin"});
        }
        const bool is_compressed = req.is_compressed;
        const bool is_allowed = req.is_allowed;
        const bool has_description = req.description.has_value();
        const std::string description = has_description ? *req.description : "";

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        // Read-then-write so an upsert without `description` keeps the stored
        // text instead of blanking it — unlike a blind ON CONFLICT overwrite.
        auto existing =
            co_await sgrn::datastore::core::execSqlCoroVec(db, "SELECT extension FROM storage.formats WHERE extension = $1", {ext});
        FormatRecord row;
        bool created = false;
        // NB: the bind lists are hoisted into named variables. Building a
        // std::vector<std::string> from an initializer list directly inside a
        // co_await expression crashes GCC 14 (ICE in gimple_add_tmp_var).
        const std::vector<std::string> format_binds{
            ext, mime, is_compressed ? "true" : "false", is_allowed ? "true" : "false", description};
        if (existing.empty()) {
            auto inserted = co_await sgrn::datastore::core::execSqlCoroVec(db,
                "INSERT INTO storage.formats (extension, mime_type, is_compressed, is_allowed, description) "
                "VALUES ($1, $2, ($3 = 'true'), ($4 = 'true'), NULLIF($5, '')) "
                "RETURNING extension, mime_type, is_compressed, is_allowed, description, created_at",
                format_binds);
            row = sgrn::orm::fromRow<FormatRecord>(inserted[0]);
            created = true;
        } else {
            // Only touch description when the caller sent one.
            const std::vector<std::string> update_binds{ext, mime, is_compressed ? "true" : "false", is_allowed ? "true" : "false",
                description, has_description ? "true" : "false"};
            auto updated = co_await sgrn::datastore::core::execSqlCoroVec(db,
                "UPDATE storage.formats SET mime_type = $2, is_compressed = ($3 = 'true'), is_allowed = ($4 = 'true'), "
                "description = CASE WHEN $6 = 'true' THEN NULLIF($5, '') ELSE description END "
                "WHERE extension = $1 "
                "RETURNING extension, mime_type, is_compressed, is_allowed, description, created_at",
                update_binds);
            row = sgrn::orm::fromRow<FormatRecord>(updated[0]);
        }
        co_return sgrn::createTypedResponse(tsp_req,
            FormatUpsertResponse{
                row.extension,
                row.mime_type,
                row.is_compressed,
                row.is_allowed,
                std::move(row.description),
                row.created_at,
                created,
            },
            k200OK);
    } catch (const drogon::orm::DrogonDbException& e) {
        co_return sgrn::createTypedResponse(
            BackendErrorResponse{std::string("Database error: ") + e.base().what(), k500InternalServerError, "StorageAdmin"});
    } catch (const std::exception& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k500InternalServerError, "StorageAdmin"});
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handleDeleteFormat(HttpRequestPtr tsp_req) {
    try {
        const std::string ext = lowerAscii(queryParam(tsp_req, "extension", ""));
        if (const std::string err = sgrn::datastore::sysconfig::validateFormatExtension(ext); !err.empty()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{err, k400BadRequest, "StorageAdmin"});
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        // The FK from storage.files has no ON DELETE action: deleting a
        // referenced format would fail deep in Postgres. Refuse loudly with
        // the blast radius instead (toggle is_allowed=false to block new
        // uploads while history stays intact).
        auto refs = co_await sgrn::datastore::core::execSqlCoroVec(db, "SELECT COUNT(*) FROM storage.files WHERE extension = $1", {ext});
        const long long referencing = refs.empty() ? 0 : fieldInt(refs[0][0]);
        if (referencing > 0) {
            co_return sgrn::createTypedResponse(tsp_req,
                FormatConflictResponse{
                    ext,
                    referencing,
                    "set is_allowed=false instead — history stays intact, new uploads are blocked",
                },
                k409Conflict);
        }

        auto deleted = co_await sgrn::datastore::core::execSqlCoroVec(db, "DELETE FROM storage.formats WHERE extension = $1", {ext});
        if (deleted.affectedRows() == 0) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{"Unknown format: " + ext, k404NotFound, "StorageAdmin"});
        }
        co_return sgrn::createTypedResponse(tsp_req, FormatDeleteResponse{ext}, k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k500InternalServerError, "StorageAdmin"});
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handleListQuotas(HttpRequestPtr tsp_req) {
    try {
        const std::string kind = lowerAscii(queryParam(tsp_req, "kind", "user"));
        if (kind != "user" && kind != "service" && kind != "organisation" && kind != "domain") {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"kind must be user, service, organisation or domain", k400BadRequest, "StorageAdmin"});
        }
        const std::string search = queryParam(tsp_req, "search", "");
        const std::size_t limit = clampParam(queryParam(tsp_req, "limit", ""), 100, 1000);
        const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        std::string quota_sql;
        std::vector<std::string> quota_binds = {escapeLike(search)};
        if (kind == "user") {
            quota_sql = "SELECT id, email, organisation, total_virtual_size AS storage_used_bytes, "
                        "storage_limit AS storage_limit_bytes, total_entry_count AS entries_used, entry_count_limit, "
                        "rate_limit_rpm, max_file_size_mb, preferred_chunk_size_mb, rate_limit_upload_rpm "
                        "FROM core.users WHERE deleted_at IS NULL AND (email ILIKE '%' || $1 || '%' ESCAPE '\\' "
                        "OR organisation ILIKE '%' || $1 || '%' ESCAPE '\\') ";
            if (!is_platform) {
                quota_sql += "AND organisation = $2 ";
                quota_binds.push_back(session_org);
            }
            quota_sql += "ORDER BY id LIMIT " + std::to_string(limit);
        } else if (kind == "service") {
            quota_sql = "SELECT id, name, organisation, total_virtual_size AS storage_used_bytes, "
                        "storage_limit AS storage_limit_bytes, total_entry_count AS entries_used, entry_count_limit, "
                        "rate_limit_rpm, max_file_size_mb, preferred_chunk_size_mb, rate_limit_upload_rpm "
                        "FROM core.automated_services WHERE deleted_at IS NULL AND (name ILIKE '%' || $1 || '%' ESCAPE '\\' "
                        "OR organisation ILIKE '%' || $1 || '%' ESCAPE '\\') ";
            if (!is_platform) {
                quota_sql += "AND organisation = $2 ";
                quota_binds.push_back(session_org);
            }
            quota_sql += "ORDER BY id LIMIT " + std::to_string(limit);
        } else if (kind == "domain") {
            // Namespace caps: usage is the whole subtree, computed by
            // storage.domain_usage (files + directories, virtual bytes).
            quota_sql = "SELECT d.id, d.name, d.path, d.organisation, d.storage_limit AS storage_limit_bytes, "
                        "d.entry_count_limit, coalesce(u.virtual_size, 0) AS storage_used_bytes, "
                        "coalesce(u.entry_count, 0) AS entries_used "
                        "FROM core.domains d LEFT JOIN LATERAL storage.domain_usage(d.id) u ON true "
                        "WHERE (d.name ILIKE '%' || $1 || '%' ESCAPE '\\' OR d.path ILIKE '%' || $1 || '%' ESCAPE '\\' "
                        "OR d.organisation ILIKE '%' || $1 || '%' ESCAPE '\\') ";
            if (!is_platform) {
                quota_sql += "AND d.organisation = $2 ";
                quota_binds.push_back(session_org);
            }
            quota_sql += "ORDER BY d.organisation, d.path LIMIT " + std::to_string(limit);
        } else {
            quota_sql = "SELECT name, name AS organisation, total_virtual_size AS storage_used_bytes, "
                        "storage_limit AS storage_limit_bytes, total_entry_count AS entries_used, entry_count_limit, rate_limit_rpm "
                        "FROM core.organisations WHERE (name ILIKE '%' || $1 || '%' ESCAPE '\\') ";
            if (!is_platform) {
                quota_sql += "AND name = $2 ";
                quota_binds.push_back(session_org);
            }
            quota_sql += "ORDER BY name LIMIT " + std::to_string(limit);
        }
        auto rows = co_await sgrn::datastore::core::execSqlCoroVec(db, quota_sql, quota_binds);

        std::vector<UserQuotaRow> users;
        std::vector<ServiceQuotaRow> services;
        std::vector<DomainQuotaRow> domains;
        std::vector<OrganisationQuotaRow> organisations;
        if (kind == "user") {
            users = sgrn::orm::fromResultByName<UserQuotaRow>(rows);
            for (auto& row : users) {
                row.kind = kind;
            }
        } else if (kind == "service") {
            services = sgrn::orm::fromResultByName<ServiceQuotaRow>(rows);
            for (auto& row : services) {
                row.kind = kind;
            }
        } else if (kind == "domain") {
            domains = sgrn::orm::fromResultByName<DomainQuotaRow>(rows);
            for (auto& row : domains) {
                row.kind = kind;
            }
        } else {
            organisations = sgrn::orm::fromResultByName<OrganisationQuotaRow>(rows);
            for (auto& row : organisations) {
                row.kind = kind;
            }
        }
        if (kind == "user") {
            co_return sgrn::createTypedResponse(
                tsp_req, UserQuotasResponse{kind, std::move(users), static_cast<int64_t>(rows.size())}, k200OK);
        }
        if (kind == "service") {
            co_return sgrn::createTypedResponse(
                tsp_req, ServiceQuotasResponse{kind, std::move(services), static_cast<int64_t>(rows.size())}, k200OK);
        }
        if (kind == "domain") {
            co_return sgrn::createTypedResponse(
                tsp_req, DomainQuotasResponse{kind, std::move(domains), static_cast<int64_t>(rows.size())}, k200OK);
        }
        co_return sgrn::createTypedResponse(
            tsp_req, OrganisationQuotasResponse{kind, std::move(organisations), static_cast<int64_t>(rows.size())}, k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k500InternalServerError, "StorageAdmin"});
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handleUpdateQuota(HttpRequestPtr tsp_req) {
    try {
        auto req_res = parseTypedRequest<detail::StorageAdminUpdateQuotaRequest>(tsp_req);
        if (req_res.hasError()) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{"Expected JSON body", k400BadRequest, "StorageAdmin"});
        }
        const auto& req = req_res.value();
        if (req.kind.empty()) {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"kind (user|service|organisation) is required", k400BadRequest, "StorageAdmin"});
        }
        const std::string kind = lowerAscii(req.kind);
        if (kind != "user" && kind != "service" && kind != "organisation" && kind != "domain") {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"kind must be user, service, organisation or domain", k400BadRequest, "StorageAdmin"});
        }
        const bool has_storage = req.storage_limit_bytes.has_value();
        const bool has_entries = req.entry_count_limit.has_value();
        if (!has_storage && !has_entries) {
            co_return sgrn::createTypedResponse(BackendErrorResponse{
                "supply storage_limit_bytes and/or entry_count_limit (null lifts the cap)", k400BadRequest, "StorageAdmin"});
        }
        const bool storage_is_null = has_storage && !req.storage_limit_bytes->has_value();
        const bool entries_is_null = has_entries && !req.entry_count_limit->has_value();
        if (has_storage && !storage_is_null && **req.storage_limit_bytes < 0) {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"storage_limit_bytes must be a non-negative integer or null", k400BadRequest, "StorageAdmin"});
        }
        if (has_entries && !entries_is_null && **req.entry_count_limit < 0) {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"entry_count_limit must be a non-negative integer or null", k400BadRequest, "StorageAdmin"});
        }

        std::string sets;
        std::vector<std::string> binds;
        auto push_set = [&](const char* t_col, bool t_is_null, const std::string& t_bind) {
            if (!sets.empty()) {
                sets += ", ";
            }
            if (t_is_null) {
                sets += std::string(t_col) + " = NULL";
            } else {
                binds.push_back(t_bind);
                sets += std::string(t_col) + " = $" + std::to_string(binds.size()) + "::bigint";
            }
        };
        if (has_storage) {
            push_set("storage_limit", storage_is_null, storage_is_null ? "" : std::to_string(**req.storage_limit_bytes));
        }
        if (has_entries) {
            push_set("entry_count_limit", entries_is_null, entries_is_null ? "" : std::to_string(**req.entry_count_limit));
        }

        // Deliberately no self-exclusion: an admin tunes their own quota
        // through the same call as anyone else's.
        const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        if (kind == "organisation") {
            if (!req.name.has_value() || req.name->empty()) {
                co_return sgrn::createTypedResponse(
                    BackendErrorResponse{"name is required for kind organisation", k400BadRequest, "StorageAdmin"});
            }
            const std::string& target_org = *req.name;
            if (!is_platform && target_org != session_org) {
                co_return sgrn::createTypedResponse(
                    BackendErrorResponse{"Forbidden: cannot modify quota of another organisation", k403Forbidden, "StorageAdmin"});
            }
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        std::string quota_sql;
        if (kind == "user" || kind == "service") {
            if (!req.id.has_value() || *req.id <= 0) {
                co_return sgrn::createTypedResponse(
                    BackendErrorResponse{"id (positive integer) is required for kind user|service", k400BadRequest, "StorageAdmin"});
            }
            binds.push_back(std::to_string(*req.id));
            const std::string table = kind == "user" ? "core.users" : "core.automated_services";
            const std::string idcol = kind == "user" ? "email" : "name";
            quota_sql = "UPDATE " + table + " SET " + sets + " WHERE id = $" + std::to_string(binds.size()) + " AND deleted_at IS NULL ";
            if (!is_platform) {
                binds.push_back(session_org);
                quota_sql += "AND organisation = $" + std::to_string(binds.size()) + " ";
            }
            quota_sql += "RETURNING id, " + idcol +
                         " AS label, organisation, total_virtual_size AS storage_used_bytes, "
                         "storage_limit AS storage_limit_bytes, total_entry_count AS entries_used, entry_count_limit";
        } else if (kind == "domain") {
            if (!req.id.has_value() || *req.id <= 0) {
                co_return sgrn::createTypedResponse(
                    BackendErrorResponse{"id (positive integer) is required for kind domain", k400BadRequest, "StorageAdmin"});
            }
            binds.push_back(std::to_string(*req.id));
            quota_sql = "UPDATE core.domains SET " + sets + " WHERE id = $" + std::to_string(binds.size()) + " ";
            if (!is_platform) {
                binds.push_back(session_org);
                quota_sql += "AND organisation = $" + std::to_string(binds.size()) + " ";
            }
            quota_sql += "RETURNING id, name AS label, organisation, "
                         "(SELECT u.virtual_size FROM storage.domain_usage(id) u) AS storage_used_bytes, "
                         "storage_limit AS storage_limit_bytes, "
                         "(SELECT u.entry_count FROM storage.domain_usage(id) u) AS entries_used, entry_count_limit";
        } else {
            const std::string target_org = *req.name;
            binds.push_back(target_org);
            quota_sql = "UPDATE core.organisations SET " + sets + " WHERE name = $" + std::to_string(binds.size()) +
                        " RETURNING NULL::bigint AS id, name AS label, name AS organisation, "
                        "total_virtual_size AS storage_used_bytes, storage_limit AS storage_limit_bytes, "
                        "total_entry_count AS entries_used, entry_count_limit";
        }
        auto rows = co_await sgrn::datastore::core::execSqlCoroVec(db, quota_sql, binds);
        if (rows.empty()) {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"No such " + kind + " (or it is soft-deleted)", k404NotFound, "StorageAdmin"});
        }
        const auto row = sgrn::orm::fromRowByName<QuotaUpdateRow>(rows[0]);

        // WP4c: this handler previously wrote no audit row at all (F7).
        // Details are the applied caps only — never request-echoed blobs.
        Json::Value applied(Json::objectValue);
        applied["storage_limit"] =
            row.storage_limit_bytes.has_value() ? Json::Value(Json::Int64(*row.storage_limit_bytes)) : Json::Value(Json::nullValue);
        applied["entry_count_limit"] =
            row.entry_count_limit.has_value() ? Json::Value(Json::Int64(*row.entry_count_limit)) : Json::Value(Json::nullValue);
        std::string user_org;
        int32_t user_id = 0;
        std::string user_email;
        bool has_user = false;
        if (tsp_req->attributes()->find("auth_context")) {
            const auto& auth = tsp_req->attributes()->get<filters::AuthContext>("auth_context");
            user_org = auth.user.organisation;
            user_id = auth.user.id;
            user_email = auth.user.email;
            has_user = (user_id > 0);
        } else if (tsp_req->attributes()->find("session_claims")) {
            const auto& user = tsp_req->attributes()->get<::sgrn::datastore::auth::SessionClaims>("session_claims").user;
            user_org = user.organisation;
            user_id = user.id;
            user_email = user.email;
            has_user = (user_id > 0);
        }
        if (has_user) {
            std::string target_id = row.label;
            if (kind != "organisation" && kind != "domain") {
                target_id = std::to_string(row.id.value());
            }
            co_await sgrn::datastore::audit::AuditLogger::log(db, user_org, "user", user_id, user_email, "quota.updated", kind, target_id,
                tsp_req->getPeerAddr().toIp(), applied, "success");
        }
        if (kind == "organisation") {
            co_return sgrn::createTypedResponse(tsp_req,
                QuotaUpdateOrganisationResponse{
                    kind,
                    row.label,
                    row.organisation,
                    row.storage_used_bytes,
                    row.storage_limit_bytes,
                    row.entries_used,
                    row.entry_count_limit,
                },
                k200OK);
        }
        co_return sgrn::createTypedResponse(tsp_req,
            QuotaUpdateWithIdResponse{
                kind,
                row.id.value(),
                row.label,
                row.organisation,
                row.storage_used_bytes,
                row.storage_limit_bytes,
                row.entries_used,
                row.entry_count_limit,
            },
            k200OK);
    } catch (const drogon::orm::DrogonDbException& e) {
        co_return sgrn::createTypedResponse(
            BackendErrorResponse{std::string("Database error: ") + e.base().what(), k500InternalServerError, "StorageAdmin"});
    } catch (const std::exception& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k500InternalServerError, "StorageAdmin"});
    }
}

// ============================================================================
// Analytics
// ============================================================================

Task<HttpResponsePtr> StorageAdminHandler::handleAnalyticsOverview(HttpRequestPtr tsp_req) {
    try {
        const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        auto users = is_platform ? co_await sgrn::datastore::core::execSqlCoroVec(db,
                                       "SELECT COUNT(*) AS count, COALESCE(SUM(total_virtual_size),0) AS storage_used_bytes, "
                                       "COALESCE(SUM(total_entry_count),0) AS entries_used FROM core.users "
                                       "WHERE deleted_at IS NULL",
                                       {})
                                 : co_await sgrn::datastore::core::execSqlCoroVec(db,
                                       "SELECT COUNT(*) AS count, COALESCE(SUM(total_virtual_size),0) AS storage_used_bytes, "
                                       "COALESCE(SUM(total_entry_count),0) AS entries_used FROM core.users "
                                       "WHERE deleted_at IS NULL AND organisation = $1",
                                       {session_org});

        auto services = is_platform ? co_await sgrn::datastore::core::execSqlCoroVec(db,
                                          "SELECT COUNT(*) AS count, COALESCE(SUM(total_virtual_size),0) AS storage_used_bytes, "
                                          "COALESCE(SUM(total_entry_count),0) AS entries_used FROM "
                                          "core.automated_services WHERE deleted_at IS NULL",
                                          {})
                                    : co_await sgrn::datastore::core::execSqlCoroVec(db,
                                          "SELECT COUNT(*) AS count, COALESCE(SUM(total_virtual_size),0) AS storage_used_bytes, "
                                          "COALESCE(SUM(total_entry_count),0) AS entries_used FROM "
                                          "core.automated_services WHERE deleted_at IS NULL AND organisation = $1",
                                          {session_org});

        auto orgs = is_platform ? co_await sgrn::datastore::core::execSqlCoroVec(db,
                                      "SELECT COUNT(*) AS count, COALESCE(SUM(total_virtual_size),0) AS storage_used_bytes, "
                                      "COALESCE(SUM(total_entry_count),0) AS entries_used FROM core.organisations",
                                      {})
                                : co_await sgrn::datastore::core::execSqlCoroVec(db,
                                      "SELECT COUNT(*) AS count, COALESCE(SUM(total_virtual_size),0) AS storage_used_bytes, "
                                      "COALESCE(SUM(total_entry_count),0) AS entries_used FROM core.organisations WHERE "
                                      "name = $1",
                                      {session_org});

        auto domains = is_platform ? co_await sgrn::datastore::core::execSqlCoroVec(db, "SELECT COUNT(*) AS count FROM core.domains", {})
                                   : co_await sgrn::datastore::core::execSqlCoroVec(
                                         db, "SELECT COUNT(*) AS count FROM core.domains WHERE organisation = $1", {session_org});

        auto files =
            is_platform
                ? co_await sgrn::datastore::core::execSqlCoroVec(db, "SELECT COUNT(*) AS count FROM storage.files", {})
                : co_await sgrn::datastore::core::execSqlCoroVec(db,
                      "SELECT COUNT(*) AS count FROM storage.files f "
                      "LEFT JOIN core.users u ON u.id = f.user_id LEFT JOIN core.automated_services s ON s.id = f.automated_service_id "
                      "WHERE COALESCE(u.organisation, s.organisation) = $1",
                      {session_org});

        auto objects =
            is_platform
                ? co_await sgrn::datastore::core::execSqlCoroVec(db,
                      "SELECT COUNT(*) AS count, COALESCE(SUM(size),0) AS bytes_stored, "
                      "COALESCE(SUM(original_size),0) AS bytes_virtual FROM storage.objects",
                      {})
                : co_await sgrn::datastore::core::execSqlCoroVec(db,
                      "SELECT COUNT(DISTINCT o.id) AS count, COALESCE(SUM(o.size),0) AS bytes_stored, "
                      "COALESCE(SUM(o.original_size),0) AS bytes_virtual FROM storage.objects o "
                      "JOIN storage.file_objects fo ON fo.object_id = o.id JOIN storage.files f ON f.id = fo.file_id "
                      "LEFT JOIN core.users u ON u.id = f.user_id LEFT JOIN core.automated_services s ON s.id = f.automated_service_id "
                      "WHERE COALESCE(u.organisation, s.organisation) = $1",
                      {session_org});

        auto formats = co_await sgrn::datastore::core::execSqlCoroVec(
            db, "SELECT COUNT(*) AS count, COALESCE(SUM(CASE WHEN is_allowed THEN 1 ELSE 0 END),0) AS allowed FROM storage.formats", {});

        auto grants =
            is_platform
                ? co_await sgrn::datastore::core::execSqlCoroVec(db, "SELECT COUNT(*) AS count FROM core.user_domain_permissions", {})
                : co_await sgrn::datastore::core::execSqlCoroVec(
                      db, "SELECT COUNT(*) AS count FROM core.user_domain_permissions WHERE organisation = $1", {session_org});

        const auto user_totals = sgrn::orm::fromSingleResultByName<AnalyticsEntityTotalsRow>(users).value_or(AnalyticsEntityTotalsRow{});
        const auto service_totals =
            sgrn::orm::fromSingleResultByName<AnalyticsEntityTotalsRow>(services).value_or(AnalyticsEntityTotalsRow{});
        const auto organisation_totals =
            sgrn::orm::fromSingleResultByName<AnalyticsEntityTotalsRow>(orgs).value_or(AnalyticsEntityTotalsRow{});
        const auto domain_totals = sgrn::orm::fromSingleResultByName<AnalyticsCountRow>(domains).value_or(AnalyticsCountRow{});
        const auto file_totals = sgrn::orm::fromSingleResultByName<AnalyticsCountRow>(files).value_or(AnalyticsCountRow{});
        const auto object_totals =
            sgrn::orm::fromSingleResultByName<AnalyticsObjectTotalsRow>(objects).value_or(AnalyticsObjectTotalsRow{});
        const auto format_totals =
            sgrn::orm::fromSingleResultByName<AnalyticsFormatTotalsRow>(formats).value_or(AnalyticsFormatTotalsRow{});
        const auto grant_totals = sgrn::orm::fromSingleResultByName<AnalyticsCountRow>(grants).value_or(AnalyticsCountRow{});

        detail::AnalyticsOverviewResponse out;
        out.counts.users = user_totals.count;
        out.counts.automated_services = service_totals.count;
        out.counts.organisations = organisation_totals.count;
        out.counts.domains = domain_totals.count;
        out.counts.files = file_totals.count;
        out.counts.objects = object_totals.count;
        out.counts.formats = format_totals.count;
        out.counts.permission_grants = grant_totals.count;
        out.bytes.virtual_bytes = object_totals.bytes_virtual;
        out.bytes.stored = object_totals.bytes_stored;
        out.bytes.saved_by_compression = object_totals.bytes_virtual - object_totals.bytes_stored;

        // --- 30-day upload timeseries (day, files, virtual bytes) ---
        auto series =
            is_platform ? co_await sgrn::datastore::core::execSqlCoroVec(db, detail::kPlatformAnalyticsTimeseriesQuery, {})
                        : co_await sgrn::datastore::core::execSqlCoroVec(db, detail::kOrganisationAnalyticsTimeseriesQuery, {session_org});
        out.timeseries_30d = is_platform
                                 ? sgrn::orm::fromResult<AnalyticsTimeseriesEntry, detail::kPlatformAnalyticsTimeseriesQuery>(series)
                                 : sgrn::orm::fromResult<AnalyticsTimeseriesEntry, detail::kOrganisationAnalyticsTimeseriesQuery>(series);

        // --- top extensions by file count ---
        auto exts = is_platform
                        ? co_await sgrn::datastore::core::execSqlCoroVec(db, detail::kPlatformAnalyticsExtensionsQuery, {})
                        : co_await sgrn::datastore::core::execSqlCoroVec(db, detail::kOrganisationAnalyticsExtensionsQuery, {session_org});
        out.top_extensions = is_platform
                                 ? sgrn::orm::fromResult<AnalyticsExtensionEntry, detail::kPlatformAnalyticsExtensionsQuery>(exts)
                                 : sgrn::orm::fromResult<AnalyticsExtensionEntry, detail::kOrganisationAnalyticsExtensionsQuery>(exts);

        // --- top uploaders (users + services in one list) ---
        auto uploaders =
            is_platform ? co_await sgrn::datastore::core::execSqlCoroVec(db, detail::kPlatformAnalyticsUploadersQuery, {})
                        : co_await sgrn::datastore::core::execSqlCoroVec(db, detail::kOrganisationAnalyticsUploadersQuery, {session_org});
        out.top_uploaders = is_platform
                                ? sgrn::orm::fromResult<AnalyticsUploaderEntry, detail::kPlatformAnalyticsUploadersQuery>(uploaders)
                                : sgrn::orm::fromResult<AnalyticsUploaderEntry, detail::kOrganisationAnalyticsUploadersQuery>(uploaders);

        // --- most recent files ---
        auto recent =
            is_platform ? co_await sgrn::datastore::core::execSqlCoroVec(db, detail::kPlatformAnalyticsRecentFilesQuery, {})
                        : co_await sgrn::datastore::core::execSqlCoroVec(db, detail::kOrganisationAnalyticsRecentFilesQuery, {session_org});
        out.recent_files = is_platform ? sgrn::orm::fromResult<AnalyticsRecentFile, detail::kPlatformAnalyticsRecentFilesQuery>(recent)
                                       : sgrn::orm::fromResult<AnalyticsRecentFile, detail::kOrganisationAnalyticsRecentFilesQuery>(recent);

        co_return sgrn::createTypedResponse(tsp_req, out, k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k500InternalServerError, "StorageAdmin"});
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handleAnalyticsBreakdown(HttpRequestPtr tsp_req) {
    try {
        const std::string kind = queryParam(tsp_req, "kind", "domain");
        if (kind != "domain" && kind != "organisation" && kind != "user" && kind != "service") {
            co_return sgrn::createTypedResponse(
                BackendErrorResponse{"kind must be domain, organisation, user or service", k400BadRequest, "StorageAdmin"});
        }
        const std::string search = queryParam(tsp_req, "search", "");
        const std::size_t limit = clampParam(queryParam(tsp_req, "limit", ""), 100, 1000);
        const std::string session_org = sgrn::datastore::getSessionOrganisation(tsp_req);
        const bool is_platform = sgrn::datastore::isPlatformAdmin(tsp_req);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return createTypedResponse(BackendErrorResponse{db_res.error()});
        }
        auto db = db_res.value();

        std::string sql;
        std::vector<std::string> binds = {escapeLike(search)};

        if (kind == "domain") {
            sql = "SELECT COALESCE(f.domain, '(none)') AS slice, COUNT(*) AS files, COALESCE(SUM(o.original_size),0) AS bytes_virtual, "
                  "COUNT(DISTINCT COALESCE('u' || f.user_id::text, 's' || f.automated_service_id::text)) AS actors "
                  "FROM storage.files f JOIN storage.file_objects fo ON fo.file_id = f.id AND fo.part_index = 0 JOIN storage.objects o ON "
                  "o.id = fo.object_id ";
            if (!is_platform) {
                sql += "LEFT JOIN core.users u ON u.id = f.user_id LEFT JOIN core.automated_services s ON s.id = f.automated_service_id "
                       "WHERE COALESCE(f.domain, '(none)') ILIKE '%' || $1 || '%' ESCAPE '\\' AND COALESCE(u.organisation, s.organisation) "
                       "= $2 ";
                binds.push_back(session_org);
            } else {
                sql += "WHERE COALESCE(f.domain, '(none)') ILIKE '%' || $1 || '%' ESCAPE '\\' ";
            }
            sql += "GROUP BY 1 ORDER BY 2 DESC LIMIT " + std::to_string(limit);
        } else if (kind == "organisation") {
            sql = "SELECT g.organisation AS slice, COUNT(*) AS files, COALESCE(SUM(o.original_size),0) AS bytes_virtual, "
                  "COUNT(DISTINCT COALESCE('u' || f.user_id::text, 's' || f.automated_service_id::text)) AS actors "
                  "FROM storage.files f JOIN storage.file_objects fo ON fo.file_id = f.id AND fo.part_index = 0 JOIN storage.objects o ON "
                  "o.id = fo.object_id "
                  "LEFT JOIN core.users u ON u.id = f.user_id LEFT JOIN core.automated_services s ON s.id = f.automated_service_id "
                  "LEFT JOIN LATERAL (SELECT COALESCE(u.organisation, s.organisation, '(none)') AS organisation) g ON true "
                  "WHERE g.organisation ILIKE '%' || $1 || '%' ESCAPE '\\' ";
            if (!is_platform) {
                sql += "AND g.organisation = $2 ";
                binds.push_back(session_org);
            }
            sql += "GROUP BY 1 ORDER BY 2 DESC LIMIT " + std::to_string(limit);
        } else if (kind == "user") {
            sql = "SELECT u.email AS slice, COUNT(*) AS files, COALESCE(SUM(o.original_size),0) AS bytes_virtual, "
                  "COALESCE(u.storage_limit, -1) AS storage_cap, COALESCE(u.entry_count_limit, -1) AS entry_cap "
                  "FROM storage.files f JOIN storage.file_objects fo ON fo.file_id = f.id AND fo.part_index = 0 JOIN storage.objects o ON "
                  "o.id = fo.object_id JOIN core.users u ON u.id = f.user_id "
                  "WHERE u.deleted_at IS NULL AND u.email ILIKE '%' || $1 || '%' ESCAPE '\\' ";
            if (!is_platform) {
                sql += "AND u.organisation = $2 ";
                binds.push_back(session_org);
            }
            sql += "GROUP BY u.email, u.storage_limit, u.entry_count_limit ORDER BY 2 DESC LIMIT " + std::to_string(limit);
        } else {
            sql = "SELECT s.name AS slice, COUNT(*) AS files, COALESCE(SUM(o.original_size),0) AS bytes_virtual, "
                  "COALESCE(s.storage_limit, -1) AS storage_cap, COALESCE(s.entry_count_limit, -1) AS entry_cap "
                  "FROM storage.files f JOIN storage.file_objects fo ON fo.file_id = f.id AND fo.part_index = 0 JOIN storage.objects o ON "
                  "o.id = fo.object_id JOIN core.automated_services s "
                  "ON s.id = f.automated_service_id "
                  "WHERE s.deleted_at IS NULL AND s.name ILIKE '%' || $1 || '%' ESCAPE '\\' ";
            if (!is_platform) {
                sql += "AND s.organisation = $2 ";
                binds.push_back(session_org);
            }
            sql += "GROUP BY s.name, s.storage_limit, s.entry_count_limit ORDER BY 2 DESC LIMIT " + std::to_string(limit);
        }
        auto rows = co_await sgrn::datastore::core::execSqlCoroVec(db, sql, binds);

        if (kind == "domain" || kind == "organisation") {
            detail::AnalyticsBreakdownActorResponse out;
            out.kind = kind;
            out.count = static_cast<int64_t>(rows.size());
            out.rows = sgrn::orm::fromResultByName<AnalyticsBreakdownActorRow>(rows);
            co_return sgrn::createTypedResponse(tsp_req, out, k200OK);
        }

        detail::AnalyticsBreakdownQuotaResponse out;
        out.kind = kind;
        out.count = static_cast<int64_t>(rows.size());
        const auto quota_rows = sgrn::orm::fromResultByName<AnalyticsBreakdownQuotaDbRow>(rows);
        out.rows.reserve(quota_rows.size());
        for (const auto& row : quota_rows) {
            out.rows.push_back(
                {row.slice, row.files, row.bytes_virtual, row.storage_cap < 0 ? std::nullopt : std::optional<int64_t>(row.storage_cap),
                    row.entry_cap < 0 ? std::nullopt : std::optional<int64_t>(row.entry_cap)});
        }
        co_return sgrn::createTypedResponse(tsp_req, out, k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createTypedResponse(BackendErrorResponse{e.what(), k500InternalServerError, "StorageAdmin"});
    }
}

} // namespace sgrn::datastore::handlers::storage_admin
