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

#include "storage-admin-internal.hpp"

namespace sgrn::datastore::handlers::storage_admin
{
using namespace drogon;
using namespace detail;

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
