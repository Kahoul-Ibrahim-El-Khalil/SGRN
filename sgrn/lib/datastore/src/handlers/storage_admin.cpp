#include <sgrn/datastore/handlers/storage_admin.hpp>

#include <sgrn/datastore/core/db.hpp>
#include <sgrn/datastore/plugins/aws/S3Client.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe_access.hpp>

#include <drogon/HttpAppFramework.h>
#include <drogon/utils/coroutine.h>
#include <fmt/core.h>
#include <json/json.h>

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{

// Upstream (MinIO) failures surface as 502, distinct from our own 500s.
struct S3UpstreamError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

using drogon::Task;
using sgrn::datastore::BackendResult;

template <typename T>
Task<T> s3unwrap(Task<BackendResult<T>> t_task) {
    BackendResult<T> res = co_await std::move(t_task);
    if (res.hasError()) {
        throw S3UpstreamError(res.error().message_);
    }
    co_return std::move(res).value();
}

Task<void> s3check(Task<BackendResult<void>> t_task) {
    BackendResult<void> res = co_await std::move(t_task);
    if (res.hasError()) {
        throw S3UpstreamError(res.error().message_);
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

struct MinioKey {
    std::string key;
    Json::Int64 size = 0;
    std::string etag;
};

struct MinioScan {
    std::vector<MinioKey> keys;
    bool truncated = false;
    uint32_t pages = 0;
};

// Page through ListObjectsV2 (server-side prefix filter) up to max_pages.
Task<MinioScan> scanBucketKeys(
    ::sgrn::datastore::plugins::aws::S3Client* s3, const std::string& t_bucket, const std::string& t_prefix, uint32_t t_max_pages) {
    MinioScan scan;
    std::string token;
    for (uint32_t page = 0; page < t_max_pages; ++page) {
        const Json::Value res = co_await s3unwrap(s3->listObjects(t_bucket, t_prefix, token, 1000));
        for (const auto& obj : res["objects"]) {
            MinioKey k;
            k.key = obj["key"].asString();
            k.size = obj["size"].asInt64();
            k.etag = obj["etag"].asString();
            scan.keys.push_back(std::move(k));
        }
        ++scan.pages;
        if (!res["is_truncated"].asBool()) {
            co_return scan;
        }
        token = res["next_continuation_token"].asString();
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

Task<HttpResponsePtr> StorageAdminHandler::handleOverview(HttpRequestPtr tsp_req) {
    try {
        auto s3 = s3plugin();
        const uint32_t max_pages = static_cast<uint32_t>(clampParam(queryParam(tsp_req, "max_pages", ""), 10, 100));

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        const Json::Value buckets_res = co_await s3unwrap(s3->listBuckets());
        Json::Value buckets = Json::arrayValue;
        for (const auto& b : buckets_res["buckets"]) {
            const std::string name = b["name"].asString();
            Json::Value entry;
            entry["name"] = name;

            const MinioScan scan = co_await scanBucketKeys(s3, name, "", max_pages);
            Json::Int64 bytes = 0;
            for (const auto& k : scan.keys) {
                bytes += k.size;
            }
            entry["minio"]["objects"] = static_cast<Json::Int64>(scan.keys.size());
            entry["minio"]["bytes"] = static_cast<Json::Int64>(bytes);
            entry["minio"]["truncated"] = scan.truncated;
            entry["minio"]["pages"] = scan.pages;

            auto objs = co_await sgrn::datastore::core::execSqlCoroVec(
                db, "SELECT COUNT(*), COALESCE(SUM(size),0) FROM storage.objects WHERE bucket = $1", {name});
            auto files = co_await sgrn::datastore::core::execSqlCoroVec(
                db, "SELECT COUNT(*) FROM storage.files f JOIN storage.objects o ON o.id = f.object_id WHERE o.bucket = $1", {name});
            entry["db"]["objects"] = static_cast<Json::Int64>(fieldInt(objs[0][0]));
            entry["db"]["bytes"] = static_cast<Json::Int64>(fieldInt(objs[0][1]));
            entry["db"]["files"] = static_cast<Json::Int64>(fieldInt(files[0][0]));
            buckets.append(std::move(entry));
        }
        Json::Value out;
        out["buckets"] = std::move(buckets);
        co_return sgrn::createJsonResponse(out, k200OK);
    } catch (const S3UpstreamError& e) {
        co_return sgrn::createErrorResponse(e.what(), k502BadGateway, "StorageAdmin");
    } catch (const std::exception& e) {
        co_return sgrn::createErrorResponse(e.what(), k500InternalServerError, "StorageAdmin");
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
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        const MinioScan scan = co_await scanBucketKeys(s3, bucket, prefix, max_pages);

        auto db_rows = co_await sgrn::datastore::core::execSqlCoroVec(
            db, "SELECT key, size FROM storage.objects WHERE bucket = $1 AND key LIKE $2 || '%' ESCAPE '\\'", {bucket, escapeLike(prefix)});
        std::unordered_map<std::string, int64_t> db_sizes;
        db_sizes.reserve(db_rows.size() * 2);
        for (const auto& row : db_rows) {
            db_sizes[row["key"].as<std::string>()] = fieldInt(row["size"]);
        }

        Json::Value minio_only = Json::arrayValue;
        int64_t minio_only_bytes = 0;
        std::size_t minio_only_count = 0;
        std::unordered_set<std::string> seen;
        seen.reserve(scan.keys.size() * 2);
        for (const auto& k : scan.keys) {
            seen.insert(k.key);
            if (db_sizes.find(k.key) == db_sizes.end()) {
                ++minio_only_count;
                minio_only_bytes += k.size;
                if (minio_only.size() < limit) {
                    Json::Value e;
                    e["key"] = k.key;
                    e["size"] = static_cast<Json::Int64>(k.size);
                    e["etag"] = k.etag;
                    minio_only.append(std::move(e));
                }
            }
        }

        // DB rows with no MinIO object: HEAD-verify each (bounded by limit)
        // so the report is exact even when the MinIO scan truncated.
        Json::Value db_missing = Json::arrayValue;
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
                Json::Value e;
                e["key"] = key;
                e["size"] = static_cast<Json::Int64>(size);
                db_missing.append(std::move(e));
            }
        }

        Json::Value out;
        out["bucket"] = bucket;
        out["prefix"] = prefix;
        out["minio_scanned"] = static_cast<Json::Int64>(scan.keys.size());
        out["minio_truncated"] = scan.truncated;
        out["minio_only"] = std::move(minio_only);
        out["minio_only_count"] = static_cast<Json::Int64>(minio_only_count);
        out["minio_only_bytes"] = static_cast<Json::Int64>(minio_only_bytes);
        out["db_missing"] = std::move(db_missing);
        out["db_missing_unchecked"] = static_cast<Json::Int64>(db_missing_unchecked);
        co_return sgrn::createJsonResponse(out, k200OK);
    } catch (const S3UpstreamError& e) {
        co_return sgrn::createErrorResponse(e.what(), k502BadGateway, "StorageAdmin");
    } catch (const std::exception& e) {
        co_return sgrn::createErrorResponse(e.what(), k500InternalServerError, "StorageAdmin");
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handlePurgeOrphans(HttpRequestPtr tsp_req) {
    try {
        auto json = tsp_req->getJsonObject();
        if (!json || !json->isObject()) {
            co_return sgrn::createErrorResponse("Expected JSON body", k400BadRequest, "StorageAdmin");
        }
        auto s3 = s3plugin();
        std::string bucket = defaultBucket();
        if (json->isMember("bucket") && (*json)["bucket"].isString() && !(*json)["bucket"].asString().empty()) {
            bucket = (*json)["bucket"].asString();
        }
        // dry_run defaults true (fail closed): only an explicit JSON false
        // arms deletion.
        bool dry_run = true;
        if (json->isMember("dry_run") && (*json)["dry_run"].isBool()) {
            dry_run = (*json)["dry_run"].asBool();
        }
        std::size_t limit = 500;
        if (json->isMember("limit") && (*json)["limit"].isUInt()) {
            limit = std::min<std::size_t>((*json)["limit"].asUInt(), 5000);
            if (limit == 0) {
                limit = 500;
            }
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        // Candidate set: explicit keys, or a classified prefix scan.
        std::vector<std::string> candidates;
        if (json->isMember("keys") && (*json)["keys"].isArray() && !(*json)["keys"].empty()) {
            for (const auto& k : (*json)["keys"]) {
                if (!k.isString() || k.asString().empty()) {
                    continue;
                }
                if (candidates.size() >= limit) {
                    break;
                }
                candidates.push_back(k.asString());
            }
        } else if (json->isMember("prefix") && (*json)["prefix"].isString()) {
            const uint32_t max_pages = static_cast<uint32_t>(clampParam(queryParam(tsp_req, "max_pages", ""), 10, 100));
            const MinioScan scan = co_await scanBucketKeys(s3, bucket, (*json)["prefix"].asString(), max_pages);
            auto prefix_rows = co_await sgrn::datastore::core::execSqlCoroVec(db,
                "SELECT key FROM storage.objects WHERE bucket = $1 AND key LIKE $2 || '%' ESCAPE '\\'",
                {bucket, escapeLike((*json)["prefix"].asString())});
            std::unordered_set<std::string> db_keys;
            db_keys.reserve(prefix_rows.size() * 2);
            for (const auto& row : prefix_rows) {
                db_keys.insert(row["key"].as<std::string>());
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
            co_return sgrn::createErrorResponse(
                "Supply non-empty keys[] or a prefix (unbounded purge refused)", k400BadRequest, "StorageAdmin");
        }

        // Re-verify every candidate against the DB immediately before acting:
        // a concurrent upload must never lose its bytes to an admin purge.
        Json::Value skipped = Json::arrayValue;
        std::vector<std::string> doomed;
        doomed.reserve(candidates.size());
        for (const auto& key : candidates) {
            auto hit = co_await sgrn::datastore::core::execSqlCoroVec(
                db, "SELECT 1 FROM storage.objects WHERE bucket = $1 AND key = $2 LIMIT 1", {bucket, key});
            if (!hit.empty()) {
                Json::Value e;
                e["key"] = key;
                e["reason"] = "referenced by storage.objects";
                skipped.append(std::move(e));
            } else {
                doomed.push_back(key);
            }
        }

        Json::Value out;
        out["bucket"] = bucket;
        out["dry_run"] = dry_run;
        if (dry_run) {
            Json::Value would = Json::arrayValue;
            for (const auto& key : doomed) {
                Json::Value e;
                e["key"] = key;
                would.append(std::move(e));
            }
            out["would_delete"] = std::move(would);
            out["would_delete_count"] = static_cast<Json::Int64>(doomed.size());
            out["skipped"] = std::move(skipped);
            co_return sgrn::createJsonResponse(out, k200OK);
        }

        Json::Value deleted = Json::arrayValue;
        Json::Value errors = Json::arrayValue;
        if (!doomed.empty()) {
            const Json::Value res = co_await s3unwrap(s3->deleteFiles(bucket, doomed));
            if (res.isMember("deleted")) {
                deleted = res["deleted"];
            } else {
                for (const auto& key : doomed) {
                    deleted.append(key);
                }
            }
            if (res.isMember("errors")) {
                errors = res["errors"];
            }
        }
        out["deleted"] = std::move(deleted);
        out["errors"] = std::move(errors);
        out["skipped"] = std::move(skipped);
        co_return sgrn::createJsonResponse(out, k200OK);
    } catch (const S3UpstreamError& e) {
        co_return sgrn::createErrorResponse(e.what(), k502BadGateway, "StorageAdmin");
    } catch (const std::exception& e) {
        co_return sgrn::createErrorResponse(e.what(), k500InternalServerError, "StorageAdmin");
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
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        if (!key.empty()) {
            Json::Value out;
            out["bucket"] = bucket;
            out["key"] = key;
            auto objs = co_await sgrn::datastore::core::execSqlCoroVec(db,
                "SELECT id, bucket, key, size, original_size, is_compressed, compression_algorithm, compression_level,"
                " provider, created_at FROM storage.objects WHERE bucket = $1 AND key = $2",
                {bucket, key});
            if (objs.empty()) {
                out["db"] = Json::Value::null;
            } else {
                const auto& row = objs[0];
                Json::Value obj;
                obj["id"] = fieldInt(row["id"]);
                obj["bucket"] = row["bucket"].as<std::string>();
                obj["object_key"] = row["key"].as<std::string>();
                obj["size"] = fieldInt(row["size"]);
                obj["original_size"] = fieldInt(row["original_size"]);
                obj["is_compressed"] = row["is_compressed"].as<std::string>() == "t";
                if (row["compression_algorithm"].isNull()) {
                    obj["compression_algorithm"] = Json::Value::null;
                } else {
                    obj["compression_algorithm"] = row["compression_algorithm"].as<std::string>();
                }
                if (row["compression_level"].isNull()) {
                    obj["compression_level"] = Json::Value::null;
                } else {
                    obj["compression_level"] = fieldInt(row["compression_level"]);
                }
                obj["provider"] = row["provider"].as<std::string>();
                obj["created_at"] = row["created_at"].as<std::string>();
                out["db"] = std::move(obj);

                auto files = co_await sgrn::datastore::core::execSqlCoroVec(db,
                    "SELECT f.id, f.full_path, f.user_id, f.automated_service_id, f.created_at"
                    " FROM storage.files f JOIN storage.objects o ON o.id = f.object_id"
                    " WHERE o.bucket = $1 AND o.key = $2 ORDER BY f.id LIMIT 100",
                    {bucket, key});
                Json::Value refs = Json::arrayValue;
                for (const auto& f : files) {
                    Json::Value e;
                    e["file_id"] = fieldInt(f["id"]);
                    e["full_path"] = f["full_path"].as<std::string>();
                    e["user_id"] = f["user_id"].isNull() ? Json::Value::null : Json::Value(fieldInt(f["user_id"]));
                    e["automated_service_id"] =
                        f["automated_service_id"].isNull() ? Json::Value::null : Json::Value(fieldInt(f["automated_service_id"]));
                    e["created_at"] = f["created_at"].as<std::string>();
                    refs.append(std::move(e));
                }
                out["files"] = std::move(refs);
            }
            try {
                out["minio"] = co_await s3unwrap(s3->statObject(bucket, key));
            } catch (const S3UpstreamError& e) {
                out["minio"] = Json::Value::null;
                out["minio_error"] = e.what();
            }
            co_return sgrn::createJsonResponse(out, k200OK);
        }

        if (!prefix.empty()) {
            const Json::Value res = co_await s3unwrap(s3->listObjects(bucket, prefix, "", static_cast<uint32_t>(limit)));
            auto match_rows = co_await sgrn::datastore::core::execSqlCoroVec(db,
                fmt::format("SELECT key FROM storage.objects WHERE bucket = $1 AND key LIKE $2 || '%' ESCAPE '\\' LIMIT {}", limit),
                {bucket, escapeLike(prefix)});
            std::unordered_set<std::string> db_keys;
            db_keys.reserve(match_rows.size() * 2);
            for (const auto& row : match_rows) {
                db_keys.insert(row["key"].as<std::string>());
            }
            Json::Value matches = Json::arrayValue;
            for (const auto& obj : res["objects"]) {
                Json::Value e;
                e["key"] = obj["key"].asString();
                e["size"] = obj["size"].asInt64();
                e["etag"] = obj["etag"].asString();
                e["in_db"] = db_keys.find(obj["key"].asString()) != db_keys.end();
                matches.append(std::move(e));
            }
            Json::Value out;
            out["bucket"] = bucket;
            out["prefix"] = prefix;
            out["matches"] = std::move(matches);
            out["truncated"] = res["is_truncated"].asBool();
            co_return sgrn::createJsonResponse(out, k200OK);
        }

        co_return sgrn::createErrorResponse(
            "Supply exactly one of key=<exact hash> or prefix=<hash prefix>", k400BadRequest, "StorageAdmin");
    } catch (const S3UpstreamError& e) {
        co_return sgrn::createErrorResponse(e.what(), k502BadGateway, "StorageAdmin");
    } catch (const std::exception& e) {
        co_return sgrn::createErrorResponse(e.what(), k500InternalServerError, "StorageAdmin");
    }
}

} // namespace sgrn::datastore::handlers::storage_admin
