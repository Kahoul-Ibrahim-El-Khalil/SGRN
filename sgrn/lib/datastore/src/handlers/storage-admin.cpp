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

} // namespace sgrn::datastore::handlers::storage_admin
