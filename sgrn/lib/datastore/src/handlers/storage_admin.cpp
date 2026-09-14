#include <sgrn/datastore/handlers/storage_admin.hpp>

#include <sgrn/datastore/config/config.hpp>
#include <sgrn/datastore/core/db.hpp>
#include <sgrn/datastore/init/rate_limit.hpp>
#include <sgrn/datastore/plugins/aws/S3Client.hpp>
#include <sgrn/datastore/services/storage.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe_access.hpp>
#include <sgrn/datastore/utils/system_config.hpp>

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

namespace
{

// Upstream (Garage) failures surface as 502, distinct from our own 500s.
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

struct GarageKey {
    std::string key;
    Json::Int64 size = 0;
    std::string etag;
};

struct GarageScan {
    std::vector<GarageKey> keys;
    bool truncated = false;
    uint32_t pages = 0;
};

// Page through ListObjectsV2 (server-side prefix filter) up to max_pages.
Task<GarageScan> scanBucketKeys(
    ::sgrn::datastore::plugins::aws::S3Client* s3, const std::string& t_bucket, const std::string& t_prefix, uint32_t t_max_pages) {
    GarageScan scan;
    std::string token;
    for (uint32_t page = 0; page < t_max_pages; ++page) {
        const Json::Value res = co_await s3unwrap(s3->listObjects(t_bucket, t_prefix, token, 1000));
        for (const auto& obj : res["objects"]) {
            GarageKey k;
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

            const GarageScan scan = co_await scanBucketKeys(s3, name, "", max_pages);
            Json::Int64 bytes = 0;
            for (const auto& k : scan.keys) {
                bytes += k.size;
            }
            entry["garage"]["objects"] = static_cast<Json::Int64>(scan.keys.size());
            entry["garage"]["bytes"] = static_cast<Json::Int64>(bytes);
            entry["garage"]["truncated"] = scan.truncated;
            entry["garage"]["pages"] = scan.pages;

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

        const GarageScan scan = co_await scanBucketKeys(s3, bucket, prefix, max_pages);

        auto db_rows = co_await sgrn::datastore::core::execSqlCoroVec(
            db, "SELECT key, size FROM storage.objects WHERE bucket = $1 AND key LIKE $2 || '%' ESCAPE '\\'", {bucket, escapeLike(prefix)});
        std::unordered_map<std::string, int64_t> db_sizes;
        db_sizes.reserve(db_rows.size() * 2);
        for (const auto& row : db_rows) {
            db_sizes[row["key"].as<std::string>()] = fieldInt(row["size"]);
        }

        Json::Value garage_only = Json::arrayValue;
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
                    Json::Value e;
                    e["key"] = k.key;
                    e["size"] = static_cast<Json::Int64>(k.size);
                    e["etag"] = k.etag;
                    garage_only.append(std::move(e));
                }
            }
        }

        // DB rows with no Garage object: HEAD-verify each (bounded by limit)
        // so the report is exact even when the Garage scan truncated.
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
        out["garage_scanned"] = static_cast<Json::Int64>(scan.keys.size());
        out["garage_truncated"] = scan.truncated;
        out["garage_only"] = std::move(garage_only);
        out["garage_only_count"] = static_cast<Json::Int64>(garage_only_count);
        out["garage_only_bytes"] = static_cast<Json::Int64>(garage_only_bytes);
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
            const GarageScan scan = co_await scanBucketKeys(s3, bucket, (*json)["prefix"].asString(), max_pages);
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
                out["garage"] = co_await s3unwrap(s3->statObject(bucket, key));
            } catch (const S3UpstreamError& e) {
                out["garage"] = Json::Value::null;
                out["garage_error"] = e.what();
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

Json::Value stringArrayJson(const std::set<std::string>& t_set) {
    Json::Value arr = Json::arrayValue;
    for (const auto& s : t_set) {
        arr.append(s);
    }
    return arr;
}

bool startsWithPath(const std::string& t_path, const std::string& t_prefix) {
    return t_path.size() > t_prefix.size() && t_path.compare(0, t_prefix.size(), t_prefix) == 0 && t_path[t_prefix.size()] == '.';
}

} // namespace

Task<HttpResponsePtr> StorageAdminHandler::handleGetSystemConfig(HttpRequestPtr tsp_req) {
    (void)tsp_req;
    try {
        const std::string path = sgrn::datastore::config::currentConfigPath();
        Json::Value root;
        if (const std::string err = readConfigFile(path, root); !err.empty()) {
            co_return sgrn::createErrorResponse(err, k500InternalServerError, "SystemConfig");
        }
        Json::Value out;
        out["config_path"] = path;
        out["values"] = systemConfigValuesJson(sgrn::datastore::sysconfig::sanitizeForAdmin(root));
        out["schema"] = systemConfigSchemaJson();
        // Effective-now values (live in-memory). Identical to `values` for
        // every hot key — PUT applies synchronously — and the source of truth
        // the upload path actually reads. Differs only if the file was
        // hand-edited without restarting for restart-required keys.
        {
            Json::Value live(Json::objectValue);
            Json::Value live_cc(Json::objectValue);
            live_cc["s3"] = sgrn::datastore::services::storage::currentStorageConfig().toJson();
            live["custom_config"] = std::move(live_cc);
            out["live"] = std::move(live);
        }
        {
            std::lock_guard lock(g_pending_restart_mutex);
            out["pending_restart"] = stringArrayJson(g_pending_restart);
        }
        co_return sgrn::createJsonResponse(out, k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createErrorResponse(e.what(), k500InternalServerError, "SystemConfig");
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handleUpdateSystemConfig(HttpRequestPtr tsp_req) {
    try {
        auto json = tsp_req->getJsonObject();
        if (!json || !json->isObject() || !json->isMember("values") || !(*json)["values"].isObject()) {
            co_return sgrn::createErrorResponse("Expected JSON body {\"values\": {...}}", k400BadRequest, "SystemConfig");
        }
        const std::string path = sgrn::datastore::config::currentConfigPath();
        Json::Value root;
        if (const std::string err = readConfigFile(path, root); !err.empty()) {
            co_return sgrn::createErrorResponse(err, k500InternalServerError, "SystemConfig");
        }

        sgrn::datastore::sysconfig::ApplyReport report;
        if (const std::string err = sgrn::datastore::sysconfig::applyAdminUpdate(root, (*json)["values"], report); !err.empty()) {
            co_return sgrn::createErrorResponse(err, k400BadRequest, "SystemConfig");
        }
        if (const std::string err = atomicWriteConfig(path, root); !err.empty()) {
            co_return sgrn::createErrorResponse(err, k500InternalServerError, "SystemConfig");
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
        Json::Value applied_hot = Json::arrayValue;
        Json::Value restart_required = Json::arrayValue;
        for (const auto& p : report.hot) {
            applied_hot.append(p);
        }
        for (const auto& p : report.restart) {
            restart_required.append(p);
        }
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
                Json::Value still_hot = Json::arrayValue;
                for (const auto& p : applied_hot) {
                    if (startsWithPath(p.asString(), "custom_config.rate_limiting")) {
                        restart_required.append(p.asString());
                    } else {
                        still_hot.append(p.asString());
                    }
                }
                applied_hot.swap(still_hot);
            }
        }

        {
            std::lock_guard lock(g_pending_restart_mutex);
            for (const auto& p : restart_required) {
                g_pending_restart.insert(p.asString());
            }
        }

        Json::Value out;
        out["success"] = true;
        out["config_path"] = path;
        out["backup_path"] = path + ".bak";
        out["applied_hot"] = std::move(applied_hot);
        out["restart_required"] = std::move(restart_required);
        {
            std::lock_guard lock(g_pending_restart_mutex);
            out["pending_restart"] = stringArrayJson(g_pending_restart);
        }
        Json::Value warnings = Json::arrayValue;
        for (const auto& w : report.warnings) {
            warnings.append(w);
        }
        out["warnings"] = std::move(warnings);
        co_return sgrn::createJsonResponse(out, k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createErrorResponse(e.what(), k500InternalServerError, "SystemConfig");
    }
}

// ============================================================================
// Formats registry + quota ledger
// ============================================================================

namespace
{

std::string lowerAscii(std::string t_in) {
    for (char& c : t_in) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return t_in;
}

bool jsonBool(const Json::Value& t_v, bool t_dflt) {
    return t_v.isBool() ? t_v.asBool() : t_dflt;
}

Json::Value formatRowJson(const drogon::orm::Row& t_row) {
    Json::Value e;
    e["extension"] = t_row["extension"].as<std::string>();
    e["mime_type"] = t_row["mime_type"].as<std::string>();
    e["is_compressed"] = t_row["is_compressed"].as<std::string>() == "t";
    e["is_allowed"] = t_row["is_allowed"].as<std::string>() == "t";
    if (t_row["description"].isNull()) {
        e["description"] = Json::Value::null;
    } else {
        e["description"] = t_row["description"].as<std::string>();
    }
    e["created_at"] = t_row["created_at"].as<std::string>();
    return e;
}

void setNullableInt64(Json::Value& t_out, const char* t_key, const drogon::orm::Field& t_field) {
    if (t_field.isNull()) {
        t_out[t_key] = Json::Value::null;
    } else {
        t_out[t_key] = fieldInt(t_field);
    }
}

} // namespace

Task<HttpResponsePtr> StorageAdminHandler::handleListFormats(HttpRequestPtr tsp_req) {
    try {
        const std::string search = lowerAscii(queryParam(tsp_req, "search", ""));
        const std::string allowed = lowerAscii(queryParam(tsp_req, "allowed", ""));
        if (!allowed.empty() && allowed != "true" && allowed != "false") {
            co_return sgrn::createErrorResponse("allowed must be true or false", k400BadRequest, "StorageAdmin");
        }
        const std::size_t limit = clampParam(queryParam(tsp_req, "limit", ""), 500, 5000);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        // $1 search (empty matches all), $2 allowed filter (empty = any).
        auto rows = co_await sgrn::datastore::core::execSqlCoroVec(db,
            "SELECT extension, mime_type, is_compressed, is_allowed, description, created_at FROM storage.formats "
            "WHERE (extension LIKE '%' || $1 || '%' ESCAPE '\\' OR mime_type ILIKE '%' || $1 || '%' ESCAPE '\\') "
            "AND ($2 = '' OR is_allowed = ($2 = 'true')) "
            "ORDER BY extension LIMIT " +
                std::to_string(limit),
            {escapeLike(search), allowed});

        Json::Value formats = Json::arrayValue;
        for (const auto& row : rows) {
            formats.append(formatRowJson(row));
        }
        Json::Value out;
        out["formats"] = std::move(formats);
        out["count"] = static_cast<Json::Int64>(rows.size());
        co_return sgrn::createJsonResponse(out, k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createErrorResponse(e.what(), k500InternalServerError, "StorageAdmin");
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handleUpsertFormat(HttpRequestPtr tsp_req) {
    try {
        auto json = tsp_req->getJsonObject();
        if (!json || !json->isObject()) {
            co_return sgrn::createErrorResponse("Expected JSON body", k400BadRequest, "StorageAdmin");
        }
        if (!json->isMember("extension") || !(*json)["extension"].isString()) {
            co_return sgrn::createErrorResponse("extension (string) is required", k400BadRequest, "StorageAdmin");
        }
        if (!json->isMember("mime_type") || !(*json)["mime_type"].isString()) {
            co_return sgrn::createErrorResponse("mime_type (string) is required", k400BadRequest, "StorageAdmin");
        }
        const std::string ext = lowerAscii((*json)["extension"].asString());
        if (const std::string err = sgrn::datastore::sysconfig::validateFormatExtension(ext); !err.empty()) {
            co_return sgrn::createErrorResponse(err, k400BadRequest, "StorageAdmin");
        }
        const std::string mime = (*json)["mime_type"].asString();
        if (const std::string err = sgrn::datastore::sysconfig::validateMimeType(mime); !err.empty()) {
            co_return sgrn::createErrorResponse(err, k400BadRequest, "StorageAdmin");
        }
        const bool is_compressed = jsonBool((*json)["is_compressed"], true);
        const bool is_allowed = jsonBool((*json)["is_allowed"], true);
        const bool has_description = json->isMember("description") && (*json)["description"].isString();
        const std::string description = has_description ? (*json)["description"].asString() : "";

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        // Read-then-write so an upsert without `description` keeps the stored
        // text instead of blanking it — unlike a blind ON CONFLICT overwrite.
        auto existing =
            co_await sgrn::datastore::core::execSqlCoroVec(db, "SELECT extension FROM storage.formats WHERE extension = $1", {ext});
        Json::Value row;
        if (existing.empty()) {
            auto inserted = co_await sgrn::datastore::core::execSqlCoroVec(db,
                "INSERT INTO storage.formats (extension, mime_type, is_compressed, is_allowed, description) "
                "VALUES ($1, $2, ($3 = 'true'), ($4 = 'true'), NULLIF($5, '')) "
                "RETURNING extension, mime_type, is_compressed, is_allowed, description, created_at",
                {ext, mime, is_compressed ? "true" : "false", is_allowed ? "true" : "false", description});
            row = formatRowJson(inserted[0]);
            row["created"] = true;
        } else {
            // Only touch description when the caller sent one.
            auto updated = co_await sgrn::datastore::core::execSqlCoroVec(db,
                "UPDATE storage.formats SET mime_type = $2, is_compressed = ($3 = 'true'), is_allowed = ($4 = 'true'), "
                "description = CASE WHEN $6 = 'true' THEN NULLIF($5, '') ELSE description END "
                "WHERE extension = $1 "
                "RETURNING extension, mime_type, is_compressed, is_allowed, description, created_at",
                {ext, mime, is_compressed ? "true" : "false", is_allowed ? "true" : "false", description,
                    has_description ? "true" : "false"});
            row = formatRowJson(updated[0]);
            row["created"] = false;
        }
        co_return sgrn::createJsonResponse(row, k200OK);
    } catch (const drogon::orm::DrogonDbException& e) {
        co_return sgrn::createErrorResponse(std::string("Database error: ") + e.base().what(), k500InternalServerError, "StorageAdmin");
    } catch (const std::exception& e) {
        co_return sgrn::createErrorResponse(e.what(), k500InternalServerError, "StorageAdmin");
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handleDeleteFormat(HttpRequestPtr tsp_req) {
    try {
        const std::string ext = lowerAscii(queryParam(tsp_req, "extension", ""));
        if (const std::string err = sgrn::datastore::sysconfig::validateFormatExtension(ext); !err.empty()) {
            co_return sgrn::createErrorResponse(err, k400BadRequest, "StorageAdmin");
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        // The FK from storage.files has no ON DELETE action: deleting a
        // referenced format would fail deep in Postgres. Refuse loudly with
        // the blast radius instead (toggle is_allowed=false to block new
        // uploads while history stays intact).
        auto refs = co_await sgrn::datastore::core::execSqlCoroVec(db, "SELECT COUNT(*) FROM storage.files WHERE extension = $1", {ext});
        const long long referencing = refs.empty() ? 0 : fieldInt(refs[0][0]);
        if (referencing > 0) {
            Json::Value detail;
            detail["extension"] = ext;
            detail["referencing_files"] = static_cast<Json::Int64>(referencing);
            detail["hint"] = "set is_allowed=false instead — history stays intact, new uploads are blocked";
            co_return sgrn::createJsonResponse(detail, k409Conflict);
        }

        auto deleted = co_await sgrn::datastore::core::execSqlCoroVec(db, "DELETE FROM storage.formats WHERE extension = $1", {ext});
        if (deleted.affectedRows() == 0) {
            co_return sgrn::createErrorResponse("Unknown format: " + ext, k404NotFound, "StorageAdmin");
        }
        Json::Value out;
        out["success"] = true;
        out["extension"] = ext;
        co_return sgrn::createJsonResponse(out, k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createErrorResponse(e.what(), k500InternalServerError, "StorageAdmin");
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handleListQuotas(HttpRequestPtr tsp_req) {
    try {
        const std::string kind = lowerAscii(queryParam(tsp_req, "kind", "user"));
        if (kind != "user" && kind != "service" && kind != "organisation") {
            co_return sgrn::createErrorResponse("kind must be user, service or organisation", k400BadRequest, "StorageAdmin");
        }
        const std::string search = queryParam(tsp_req, "search", "");
        const std::size_t limit = clampParam(queryParam(tsp_req, "limit", ""), 100, 1000);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        std::string quota_sql;
        if (kind == "user") {
            quota_sql = "SELECT id, email, organisation, total_virtual_size, storage_limit, total_entry_count, entry_count_limit "
                        "FROM core.users WHERE deleted_at IS NULL AND (email ILIKE '%' || $1 || '%' ESCAPE '\\' "
                        "OR organisation ILIKE '%' || $1 || '%' ESCAPE '\\') "
                        "ORDER BY id LIMIT " +
                        std::to_string(limit);
        } else if (kind == "service") {
            quota_sql = "SELECT id, name, organisation, total_virtual_size, storage_limit, total_entry_count, entry_count_limit "
                        "FROM core.automated_services WHERE deleted_at IS NULL AND (name ILIKE '%' || $1 || '%' ESCAPE '\\' "
                        "OR organisation ILIKE '%' || $1 || '%' ESCAPE '\\') "
                        "ORDER BY id LIMIT " +
                        std::to_string(limit);
        } else {
            quota_sql = "SELECT name, total_virtual_size, storage_limit, total_entry_count, entry_count_limit "
                        "FROM core.organisations WHERE name ILIKE '%' || $1 || '%' ESCAPE '\\' "
                        "ORDER BY name LIMIT " +
                        std::to_string(limit);
        }
        auto rows = co_await sgrn::datastore::core::execSqlCoroVec(db, quota_sql, {escapeLike(search)});

        Json::Value list = Json::arrayValue;
        for (const auto& row : rows) {
            Json::Value e;
            if (kind == "user") {
                e["id"] = fieldInt(row["id"]);
                e["email"] = row["email"].as<std::string>();
            } else if (kind == "service") {
                e["id"] = fieldInt(row["id"]);
                e["name"] = row["name"].as<std::string>();
            } else {
                e["name"] = row["name"].as<std::string>();
            }
            e["organisation"] = row["organisation"].isNull() ? Json::Value::null : Json::Value(row["organisation"].as<std::string>());
            e["storage_used_bytes"] = fieldInt(row["total_virtual_size"]);
            setNullableInt64(e, "storage_limit_bytes", row["storage_limit"]);
            e["entries_used"] = fieldInt(row["total_entry_count"]);
            setNullableInt64(e, "entry_count_limit", row["entry_count_limit"]);
            list.append(std::move(e));
        }
        Json::Value out;
        out["kind"] = kind;
        out["rows"] = std::move(list);
        out["count"] = static_cast<Json::Int64>(rows.size());
        co_return sgrn::createJsonResponse(out, k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createErrorResponse(e.what(), k500InternalServerError, "StorageAdmin");
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handleUpdateQuota(HttpRequestPtr tsp_req) {
    try {
        auto json = tsp_req->getJsonObject();
        if (!json || !json->isObject()) {
            co_return sgrn::createErrorResponse("Expected JSON body", k400BadRequest, "StorageAdmin");
        }
        if (!json->isMember("kind") || !(*json)["kind"].isString()) {
            co_return sgrn::createErrorResponse("kind (user|service|organisation) is required", k400BadRequest, "StorageAdmin");
        }
        const std::string kind = lowerAscii((*json)["kind"].asString());
        if (kind != "user" && kind != "service" && kind != "organisation") {
            co_return sgrn::createErrorResponse("kind must be user, service or organisation", k400BadRequest, "StorageAdmin");
        }
        const bool has_storage = json->isMember("storage_limit_bytes");
        const bool has_entries = json->isMember("entry_count_limit");
        if (!has_storage && !has_entries) {
            co_return sgrn::createErrorResponse(
                "supply storage_limit_bytes and/or entry_count_limit (null lifts the cap)", k400BadRequest, "StorageAdmin");
        }
        // Explicit null lifts the cap; otherwise a non-negative integer.
        // storage_limit is bytes (bigint) — the dashboard edits megabytes and
        // converts, so large-but-sane values are normal here.
        // Returns the text bind, or "" to SET NULL; o_err set on rejection.
        auto parseCap = [](const Json::Value& t_v, const char* t_name, std::string& o_err) -> std::string {
            if (t_v.isNull()) {
                return "";
            }
            if (!(t_v.isUInt() || t_v.isUInt64() || (t_v.isInt64() && t_v.asInt64() >= 0))) {
                o_err = std::string(t_name) + " must be a non-negative integer or null";
                return "";
            }
            return std::to_string(t_v.asUInt64());
        };
        std::string cap_err;
        std::string storage_bind;
        std::string entries_bind;
        bool storage_is_null = false;
        bool entries_is_null = false;
        if (has_storage) {
            const Json::Value& v = (*json)["storage_limit_bytes"];
            storage_is_null = v.isNull();
            storage_bind = parseCap(v, "storage_limit_bytes", cap_err);
            if (!cap_err.empty()) {
                co_return sgrn::createErrorResponse(cap_err, k400BadRequest, "StorageAdmin");
            }
        }
        if (has_entries) {
            const Json::Value& v = (*json)["entry_count_limit"];
            entries_is_null = v.isNull();
            entries_bind = parseCap(v, "entry_count_limit", cap_err);
            if (!cap_err.empty()) {
                co_return sgrn::createErrorResponse(cap_err, k400BadRequest, "StorageAdmin");
            }
        }

        // Build SET with explicit NULLs (binds are text; NULLIF would conflate
        // "lift the cap" with a value, so branch instead).
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
            push_set("storage_limit", storage_is_null, storage_bind);
        }
        if (has_entries) {
            push_set("entry_count_limit", entries_is_null, entries_bind);
        }

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        // Deliberately no self-exclusion: an admin tunes their own quota
        // through the same call as anyone else's.
        std::string quota_sql;
        if (kind == "user" || kind == "service") {
            if (!json->isMember("id") || !(*json)["id"].isInt() || (*json)["id"].asInt() <= 0) {
                co_return sgrn::createErrorResponse(
                    "id (positive integer) is required for kind user|service", k400BadRequest, "StorageAdmin");
            }
            binds.push_back(std::to_string((*json)["id"].asInt()));
            const std::string table = kind == "user" ? "core.users" : "core.automated_services";
            const std::string idcol = kind == "user" ? "email" : "name";
            quota_sql = "UPDATE " + table + " SET " + sets + " WHERE id = $" + std::to_string(binds.size()) +
                        " AND deleted_at IS NULL RETURNING id, " + idcol +
                        " AS label, organisation, total_virtual_size, storage_limit, total_entry_count, entry_count_limit";
        } else {
            if (!json->isMember("name") || !(*json)["name"].isString() || (*json)["name"].asString().empty()) {
                co_return sgrn::createErrorResponse("name is required for kind organisation", k400BadRequest, "StorageAdmin");
            }
            binds.push_back((*json)["name"].asString());
            quota_sql = "UPDATE core.organisations SET " + sets + " WHERE name = $" + std::to_string(binds.size()) +
                        " RETURNING name AS label, name AS organisation, total_virtual_size, storage_limit, total_entry_count, "
                        "entry_count_limit";
        }
        auto rows = co_await sgrn::datastore::core::execSqlCoroVec(db, quota_sql, binds);
        if (rows.empty()) {
            co_return sgrn::createErrorResponse("No such " + kind + " (or it is soft-deleted)", k404NotFound, "StorageAdmin");
        }
        const auto& row = rows[0];
        Json::Value out;
        out["kind"] = kind;
        if (kind != "organisation") {
            out["id"] = fieldInt(row["id"]);
        }
        out["label"] = row["label"].as<std::string>();
        out["organisation"] = row["organisation"].isNull() ? Json::Value::null : Json::Value(row["organisation"].as<std::string>());
        out["storage_used_bytes"] = fieldInt(row["total_virtual_size"]);
        setNullableInt64(out, "storage_limit_bytes", row["storage_limit"]);
        out["entries_used"] = fieldInt(row["total_entry_count"]);
        setNullableInt64(out, "entry_count_limit", row["entry_count_limit"]);
        co_return sgrn::createJsonResponse(out, k200OK);
    } catch (const drogon::orm::DrogonDbException& e) {
        co_return sgrn::createErrorResponse(std::string("Database error: ") + e.base().what(), k500InternalServerError, "StorageAdmin");
    } catch (const std::exception& e) {
        co_return sgrn::createErrorResponse(e.what(), k500InternalServerError, "StorageAdmin");
    }
}

// ============================================================================
// Analytics
// ============================================================================

Task<HttpResponsePtr> StorageAdminHandler::handleAnalyticsOverview(HttpRequestPtr tsp_req) {
    (void)tsp_req;
    try {
        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        // --- headline counts (soft-deleted actors excluded) ---
        auto users = co_await sgrn::datastore::core::execSqlCoroVec(db,
            "SELECT COUNT(*), COALESCE(SUM(total_virtual_size),0), COALESCE(SUM(total_entry_count),0) FROM core.users "
            "WHERE deleted_at IS NULL",
            {});
        auto services = co_await sgrn::datastore::core::execSqlCoroVec(db,
            "SELECT COUNT(*), COALESCE(SUM(total_virtual_size),0), COALESCE(SUM(total_entry_count),0) FROM "
            "core.automated_services WHERE deleted_at IS NULL",
            {});
        auto orgs = co_await sgrn::datastore::core::execSqlCoroVec(
            db, "SELECT COUNT(*), COALESCE(SUM(total_virtual_size),0), COALESCE(SUM(total_entry_count),0) FROM core.organisations", {});
        auto domains = co_await sgrn::datastore::core::execSqlCoroVec(db, "SELECT COUNT(*) FROM core.domains", {});
        auto files = co_await sgrn::datastore::core::execSqlCoroVec(db, "SELECT COUNT(*) FROM storage.files", {});
        auto objects = co_await sgrn::datastore::core::execSqlCoroVec(
            db, "SELECT COUNT(*), COALESCE(SUM(size),0), COALESCE(SUM(original_size),0) FROM storage.objects", {});
        auto formats = co_await sgrn::datastore::core::execSqlCoroVec(
            db, "SELECT COUNT(*), COALESCE(SUM(CASE WHEN is_allowed THEN 1 ELSE 0 END),0) FROM storage.formats", {});
        auto grants = co_await sgrn::datastore::core::execSqlCoroVec(db, "SELECT COUNT(*) FROM core.user_domain_permissions", {});

        const int64_t bytes_stored = objects.empty() ? 0 : fieldInt(objects[0][1]);
        const int64_t bytes_virtual = objects.empty() ? 0 : fieldInt(objects[0][2]);

        Json::Value out;
        Json::Value counts;
        counts["users"] = users.empty() ? 0 : fieldInt(users[0][0]);
        counts["automated_services"] = services.empty() ? 0 : fieldInt(services[0][0]);
        counts["organisations"] = orgs.empty() ? 0 : fieldInt(orgs[0][0]);
        counts["domains"] = domains.empty() ? 0 : fieldInt(domains[0][0]);
        counts["files"] = files.empty() ? 0 : fieldInt(files[0][0]);
        counts["objects"] = objects.empty() ? 0 : fieldInt(objects[0][0]);
        counts["formats"] = formats.empty() ? 0 : fieldInt(formats[0][0]);
        counts["permission_grants"] = grants.empty() ? 0 : fieldInt(grants[0][0]);
        out["counts"] = std::move(counts);

        Json::Value bytes;
        bytes["virtual"] = bytes_virtual;
        bytes["stored"] = bytes_stored;
        bytes["saved_by_compression"] = bytes_virtual - bytes_stored;
        out["bytes"] = std::move(bytes);

        // --- 30-day upload timeseries (day, files, virtual bytes) ---
        auto series = co_await sgrn::datastore::core::execSqlCoroVec(db,
            "SELECT to_char(date_trunc('day', f.created_at), 'YYYY-MM-DD') AS day, COUNT(*) AS files, "
            "COALESCE(SUM(o.original_size),0) AS bytes_virtual "
            "FROM storage.files f JOIN storage.objects o ON o.id = f.object_id "
            "WHERE f.created_at >= now() - interval '30 days' "
            "GROUP BY 1 ORDER BY 1",
            {});
        Json::Value timeseries = Json::arrayValue;
        for (const auto& row : series) {
            Json::Value e;
            e["day"] = row["day"].as<std::string>();
            e["files"] = fieldInt(row["files"]);
            e["bytes_virtual"] = fieldInt(row["bytes_virtual"]);
            timeseries.append(std::move(e));
        }
        out["timeseries_30d"] = std::move(timeseries);

        // --- top extensions by file count ---
        auto exts = co_await sgrn::datastore::core::execSqlCoroVec(db,
            "SELECT COALESCE(f.extension, '(none)') AS ext, COUNT(*) AS files, COALESCE(SUM(o.original_size),0) AS bytes_virtual "
            "FROM storage.files f JOIN storage.objects o ON o.id = f.object_id "
            "GROUP BY 1 ORDER BY 2 DESC LIMIT 10",
            {});
        Json::Value top_ext = Json::arrayValue;
        for (const auto& row : exts) {
            Json::Value e;
            e["extension"] = row["ext"].as<std::string>();
            e["files"] = fieldInt(row["files"]);
            e["bytes_virtual"] = fieldInt(row["bytes_virtual"]);
            top_ext.append(std::move(e));
        }
        out["top_extensions"] = std::move(top_ext);

        // --- top uploaders (users + services in one list) ---
        auto uploaders = co_await sgrn::datastore::core::execSqlCoroVec(db,
            "SELECT 'user:' || u.email AS actor, COUNT(*) AS files, COALESCE(SUM(o.original_size),0) AS bytes_virtual "
            "FROM storage.files f JOIN storage.objects o ON o.id = f.object_id JOIN core.users u ON u.id = f.user_id "
            "GROUP BY 1 "
            "UNION ALL "
            "SELECT 'service:' || s.name AS actor, COUNT(*) AS files, COALESCE(SUM(o.original_size),0) AS bytes_virtual "
            "FROM storage.files f JOIN storage.objects o ON o.id = f.object_id JOIN core.automated_services s "
            "ON s.id = f.automated_service_id "
            "GROUP BY 1 ORDER BY 2 DESC LIMIT 10",
            {});
        Json::Value top_actors = Json::arrayValue;
        for (const auto& row : uploaders) {
            Json::Value e;
            e["actor"] = row["actor"].as<std::string>();
            e["files"] = fieldInt(row["files"]);
            e["bytes_virtual"] = fieldInt(row["bytes_virtual"]);
            top_actors.append(std::move(e));
        }
        out["top_uploaders"] = std::move(top_actors);

        // --- most recent files ---
        auto recent = co_await sgrn::datastore::core::execSqlCoroVec(db,
            "SELECT f.name, COALESCE(f.extension, '') AS ext, o.original_size, "
            "COALESCE('user:' || u.email, 'service:' || s.name, '(unknown)') AS actor, "
            "to_char(f.created_at, 'YYYY-MM-DD HH24:MI:SS') AS at "
            "FROM storage.files f JOIN storage.objects o ON o.id = f.object_id "
            "LEFT JOIN core.users u ON u.id = f.user_id LEFT JOIN core.automated_services s ON s.id = f.automated_service_id "
            "ORDER BY f.id DESC LIMIT 10",
            {});
        Json::Value recent_json = Json::arrayValue;
        for (const auto& row : recent) {
            Json::Value e;
            e["name"] = row["name"].as<std::string>();
            e["extension"] = row["ext"].as<std::string>();
            e["bytes_virtual"] = fieldInt(row["original_size"]);
            e["actor"] = row["actor"].as<std::string>();
            e["at"] = row["at"].as<std::string>();
            recent_json.append(std::move(e));
        }
        out["recent_files"] = std::move(recent_json);

        co_return sgrn::createJsonResponse(out, k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createErrorResponse(e.what(), k500InternalServerError, "StorageAdmin");
    }
}

Task<HttpResponsePtr> StorageAdminHandler::handleAnalyticsBreakdown(HttpRequestPtr tsp_req) {
    try {
        const std::string kind = queryParam(tsp_req, "kind", "domain");
        if (kind != "domain" && kind != "organisation" && kind != "user" && kind != "service") {
            co_return sgrn::createErrorResponse("kind must be domain, organisation, user or service", k400BadRequest, "StorageAdmin");
        }
        const std::string search = queryParam(tsp_req, "search", "");
        const std::size_t limit = clampParam(queryParam(tsp_req, "limit", ""), 100, 1000);

        auto db_res = sgrn::datastore::core::getDbClient();
        if (db_res.hasError()) {
            co_return sgrn::createJsonResponse(db_res);
        }
        auto db = db_res.value();

        std::string sql;
        if (kind == "domain") {
            sql = "SELECT COALESCE(f.domain, '(none)') AS slice, COUNT(*) AS files, COALESCE(SUM(o.original_size),0) AS bytes_virtual, "
                  "COUNT(DISTINCT COALESCE('u' || f.user_id::text, 's' || f.automated_service_id::text)) AS actors "
                  "FROM storage.files f JOIN storage.objects o ON o.id = f.object_id "
                  "WHERE COALESCE(f.domain, '(none)') ILIKE '%' || $1 || '%' ESCAPE '\\' "
                  "GROUP BY 1 ORDER BY 2 DESC LIMIT " +
                  std::to_string(limit);
        } else if (kind == "organisation") {
            sql = "SELECT g.organisation AS slice, COUNT(*) AS files, COALESCE(SUM(o.original_size),0) AS bytes_virtual, "
                  "COUNT(DISTINCT COALESCE('u' || f.user_id::text, 's' || f.automated_service_id::text)) AS actors "
                  "FROM storage.files f JOIN storage.objects o ON o.id = f.object_id "
                  "LEFT JOIN core.users u ON u.id = f.user_id LEFT JOIN core.automated_services s ON s.id = f.automated_service_id "
                  "LEFT JOIN LATERAL (SELECT COALESCE(u.organisation, s.organisation, '(none)') AS organisation) g ON true "
                  "WHERE g.organisation ILIKE '%' || $1 || '%' ESCAPE '\\' "
                  "GROUP BY 1 ORDER BY 2 DESC LIMIT " +
                  std::to_string(limit);
        } else if (kind == "user") {
            sql = "SELECT u.email AS slice, COUNT(*) AS files, COALESCE(SUM(o.original_size),0) AS bytes_virtual, "
                  "COALESCE(u.storage_limit, -1) AS storage_cap, COALESCE(u.entry_count_limit, -1) AS entry_cap "
                  "FROM storage.files f JOIN storage.objects o ON o.id = f.object_id JOIN core.users u ON u.id = f.user_id "
                  "WHERE u.deleted_at IS NULL AND u.email ILIKE '%' || $1 || '%' ESCAPE '\\' "
                  "GROUP BY u.email, u.storage_limit, u.entry_count_limit ORDER BY 2 DESC LIMIT " +
                  std::to_string(limit);
        } else {
            sql = "SELECT s.name AS slice, COUNT(*) AS files, COALESCE(SUM(o.original_size),0) AS bytes_virtual, "
                  "COALESCE(s.storage_limit, -1) AS storage_cap, COALESCE(s.entry_count_limit, -1) AS entry_cap "
                  "FROM storage.files f JOIN storage.objects o ON o.id = f.object_id JOIN core.automated_services s "
                  "ON s.id = f.automated_service_id "
                  "WHERE s.deleted_at IS NULL AND s.name ILIKE '%' || $1 || '%' ESCAPE '\\' "
                  "GROUP BY s.name, s.storage_limit, s.entry_count_limit ORDER BY 2 DESC LIMIT " +
                  std::to_string(limit);
        }
        auto rows = co_await sgrn::datastore::core::execSqlCoroVec(db, sql, {escapeLike(search)});

        Json::Value list = Json::arrayValue;
        for (const auto& row : rows) {
            Json::Value e;
            e["slice"] = row["slice"].as<std::string>();
            e["files"] = fieldInt(row["files"]);
            e["bytes_virtual"] = fieldInt(row["bytes_virtual"]);
            if (kind == "domain" || kind == "organisation") {
                e["actors"] = fieldInt(row["actors"]);
            } else {
                const int64_t storage_cap = fieldInt(row["storage_cap"]);
                const int64_t entry_cap = fieldInt(row["entry_cap"]);
                e["storage_limit_bytes"] = storage_cap < 0 ? Json::Value::null : Json::Value(storage_cap);
                e["entry_count_limit"] = entry_cap < 0 ? Json::Value::null : Json::Value(entry_cap);
            }
            list.append(std::move(e));
        }
        Json::Value out;
        out["kind"] = kind;
        out["rows"] = std::move(list);
        out["count"] = static_cast<Json::Int64>(rows.size());
        co_return sgrn::createJsonResponse(out, k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createErrorResponse(e.what(), k500InternalServerError, "StorageAdmin");
    }
}

} // namespace sgrn::datastore::handlers::storage_admin
