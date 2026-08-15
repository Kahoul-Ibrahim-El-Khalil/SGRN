#include <fmt/core.h>
#include <sgrn/datastore/plugins/redis/RedisMiddleware.hpp>
#include <sgrn/debug.hpp>
#include <sstream>

namespace sgrn::datastore::plugins
{

// Internal formatting (prefixed with sgrn:service: via prefix())
static constexpr std::string_view kSessionFmt = "session:{}";
static constexpr std::string_view kUserCacheFmt = "user:cache:{}";
static constexpr std::string_view kUploadKeyFmt = "upload:key:{}";
static constexpr std::string_view kDownloadKeyFmt = "download:key:{}";
static constexpr std::string_view kPresignedUrlKeyFmt = "presigned_url:{}";

void RedisMiddleware::initAndStart(const Json::Value& t_config) {
    SGRN_INFO("RedisMiddleware", "RedisMiddleware plugin started successfully.");
}

void RedisMiddleware::shutdown() {
    SGRN_INFO("RedisMiddleware", "RedisMiddleware plugin shutting down.");
}

static std::string serializeJson(const Json::Value& t_val) {
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    return Json::writeString(builder, t_val);
}

static std::optional<Json::Value> deserializeJson(const std::string& t_data) {
    Json::CharReaderBuilder builder;
    Json::Value t_val;
    std::string errs;
    std::istringstream ss(t_data);
    if (!Json::parseFromStream(builder, ss, &t_val, &errs)) {
        return std::nullopt;
    }
    return t_val;
}

drogon::Task<void> RedisMiddleware::set(const std::string& t_key, const std::string& t_val, int t_ttl) {
    auto redis = drogon::app().getRedisClient();
    if (redis == nullptr)
        co_return;

    auto prefixed = prefix(t_key);
    if (t_ttl > 0) {
        co_await redis->execCommandCoro("SETEX %s %d %s", prefixed.c_str(), t_ttl, t_val.c_str());
    } else {
        co_await redis->execCommandCoro("SET %s %s", prefixed.c_str(), t_val.c_str());
    }
}

drogon::Task<void> RedisMiddleware::setJson(const std::string& t_key, const Json::Value& t_val, int t_ttl) {
    co_await set(t_key, serializeJson(t_val), t_ttl);
}

drogon::Task<BackendResult<std::string>> RedisMiddleware::get(const std::string& t_key) {
    auto redis = drogon::app().getRedisClient();
    if (redis == nullptr)
        co_return BackendResult<std::string>::Error(scope_redis, "Redis client not available");

    auto prefixed = prefix(t_key);
    try {
        auto res = co_await redis->execCommandCoro("GET %s", prefixed.c_str());
        if (res.isNil())
            co_return BackendResult<std::string>::Error(scope_redis, "key not found");
        co_return res.asString();
    } catch (const std::exception& e) {
        SGRN_ERROR_LOG("RedisMiddleware", "GET error: " + std::string(e.what()));
        co_return BackendResult<std::string>::Error(scope_redis, std::string("GET error: ") + e.what());
    }
}

drogon::Task<BackendResult<Json::Value>> RedisMiddleware::getJson(const std::string& t_key) {
    auto res = co_await get(t_key);
    if (res.hasError())
        co_return res.error();

    auto json_val = deserializeJson(res.value());
    if (!json_val.has_value())
        co_return BackendResult<Json::Value>::Error(scope_redis, "Invalid JSON data");
    co_return json_val.value();
}

BackendResult<std::string> RedisMiddleware::getSync(const std::string& t_key) {
    auto redis = drogon::app().getRedisClient();
    if (redis == nullptr)
        return BackendResult<std::string>::Error(scope_redis, "Redis client not available");

    auto prefixed = prefix(t_key);
    try {
        auto res = redis->execCommandSync(
            [](const drogon::nosql::RedisResult& t_r) -> std::optional<std::string> {
                if (t_r.isNil())
                    return std::nullopt;
                return t_r.asString();
            },
            "GET %s", prefixed.c_str());
        if (!res.has_value())
            return BackendResult<std::string>::Error(scope_redis, "key not found");
        return res.value();
    } catch (const std::exception& e) {
        SGRN_ERROR_LOG("RedisMiddleware", "getSync error: " + std::string(e.what()));
        return BackendResult<std::string>::Error(scope_redis, std::string("getSync error: ") + e.what());
    } catch (...) {
        SGRN_ERROR_LOG("RedisMiddleware", "getSync unknown error");
        return BackendResult<std::string>::Error(scope_redis, "getSync unknown error");
    }
}

BackendResult<Json::Value> RedisMiddleware::getJsonSync(const std::string& t_key) {
    auto res = getSync(t_key);
    if (res.hasError())
        return res.error();

    auto json_val = deserializeJson(res.value());
    if (!json_val.has_value())
        return BackendResult<Json::Value>::Error(scope_redis, "Invalid JSON data");
    return json_val.value();
}

drogon::Task<void> RedisMiddleware::del(const std::string& t_key) {
    auto redis = drogon::app().getRedisClient();
    if (redis == nullptr)
        co_return;

    auto prefixed = prefix(t_key);
    co_await redis->execCommandCoro("DEL %s", prefixed.c_str());
}

drogon::Task<bool> RedisMiddleware::exists(const std::string& t_key) {
    auto redis = drogon::app().getRedisClient();
    if (redis == nullptr)
        co_return false;

    auto prefixed = prefix(t_key);
    auto res = co_await redis->execCommandCoro("EXISTS %s", prefixed.c_str());
    co_return res.asInteger() > 0;
}

drogon::Task<void> RedisMiddleware::expire(const std::string& t_key, int t_ttl) {
    auto redis = drogon::app().getRedisClient();
    if (redis == nullptr)
        co_return;

    auto prefixed = prefix(t_key);
    co_await redis->execCommandCoro("EXPIRE %s %d", prefixed.c_str(), t_ttl);
}

// --- Authentication default_componentent ---

drogon::Task<BackendResult<void>> RedisMiddleware::storeSession(
    const std::string& t_token, const Json::Value& t_session_data, int t_ttl_seconds) {
    try {
        std::string session_key = fmt::format(kSessionFmt, t_token);
        co_await setJson(session_key, t_session_data, t_ttl_seconds);
        if (t_session_data.isMember("upload_key")) {
            std::string upload_redis_key = fmt::format(kUploadKeyFmt, t_session_data["upload_key"].asString());
            co_await set(upload_redis_key, "1", t_ttl_seconds);
        }
        co_return {};
    } catch (const std::exception& e) {
        SGRN_ERROR_LOG("RedisMiddleware", "storeSession error: " + std::string(e.what()));
        co_return BackendResult<void>::Error(makeBackendError(scope_redis, "storeSession error: {}", e.what()));
    }
}

drogon::Task<BackendResult<Json::Value>> RedisMiddleware::getSession(const std::string& t_token) {
    std::string t_key = fmt::format(kSessionFmt, t_token);
    co_return co_await getJson(t_key);
}

BackendResult<Json::Value> RedisMiddleware::getSessionSync(const std::string& t_token) {
    std::string t_key = fmt::format(kSessionFmt, t_token);
    return getJsonSync(t_key);
}

drogon::Task<BackendResult<void>> RedisMiddleware::deleteSession(const std::string& t_token) {
    try {
        std::string session_key = fmt::format(kSessionFmt, t_token);
        auto session_opt = co_await getJson(session_key);
        if (!session_opt.hasError() && session_opt.value().isMember("upload_key")) {
            std::string upload_redis_key = fmt::format(kUploadKeyFmt, session_opt.value()["upload_key"].asString());
            co_await del(upload_redis_key);
        }
        co_await del(session_key);
        co_return {};
    } catch (const std::exception& e) {
        SGRN_ERROR_LOG("RedisMiddleware", "deleteSession error: " + std::string(e.what()));
        co_return BackendResult<void>::Error(makeBackendError(scope_redis, "deleteSession error: {}", e.what()));
    }
}

drogon::Task<BackendResult<void>> RedisMiddleware::storeUserCache(
    const std::string& t_email, const std::string& t_token, int t_ttl_seconds) {
    try {
        std::string t_key = fmt::format(kUserCacheFmt, t_email);
        Json::Value cache;
        cache["token"] = t_token;
        co_await setJson(t_key, cache, t_ttl_seconds);
        co_return {};
    } catch (const std::exception& e) {
        SGRN_ERROR_LOG("RedisMiddleware", "storeUserCache error: " + std::string(e.what()));
        co_return BackendResult<void>::Error(makeBackendError(scope_redis, "storeUserCache error: {}", e.what()));
    }
}

drogon::Task<BackendResult<std::string>> RedisMiddleware::getTokenFromUserCache(const std::string& t_email) {
    std::string t_key = fmt::format(kUserCacheFmt, t_email);
    auto json = co_await getJson(t_key);
    if (json.hasError())
        co_return json.error();
    if (json.value().isMember("token")) {
        co_return json.value()["token"].asString();
    }
    co_return BackendResult<std::string>::Error(scope_redis, "token not found in cache");
}

drogon::Task<BackendResult<void>> RedisMiddleware::deleteUserCache(const std::string& t_email) {
    try {
        std::string t_key = fmt::format(kUserCacheFmt, t_email);
        co_await del(t_key);
        co_return {};
    } catch (const std::exception& e) {
        SGRN_ERROR_LOG("RedisMiddleware", "deleteUserCache error: " + std::string(e.what()));
        co_return BackendResult<void>::Error(makeBackendError(scope_redis, "deleteUserCache error: {}", e.what()));
    }
}

drogon::Task<void> RedisMiddleware::refreshSession(const std::string& t_token, int t_ttl_seconds) {
    std::string session_key = fmt::format(kSessionFmt, t_token);
    co_await expire(session_key, t_ttl_seconds);
}

drogon::Task<int64_t> RedisMiddleware::getSessionTtl(const std::string& t_token) {
    auto redis = drogon::app().getRedisClient();
    if (redis == nullptr)
        co_return -2; // -2 usually means key does not exist in Redis TTL terms

    std::string t_key = fmt::format(kSessionFmt, t_token);
    auto prefixed = prefix(t_key);
    auto res = co_await redis->execCommandCoro("TTL %s", prefixed.c_str());
    co_return res.asInteger();
}

// --- Download Management ---

drogon::Task<void> RedisMiddleware::storeDownloadToken(const std::string& t_token, const std::string& t_file_path, int t_ttl_seconds) {
    std::string t_key = fmt::format(kDownloadKeyFmt, t_token);
    co_await set(t_key, t_file_path, t_ttl_seconds);
}

drogon::Task<BackendResult<std::string>> RedisMiddleware::getFilePathFromToken(const std::string& t_token) {
    std::string t_key = fmt::format(kDownloadKeyFmt, t_token);
    co_return co_await get(t_key);
}

drogon::Task<void> RedisMiddleware::removeDownloadToken(const std::string& t_token) {
    std::string t_key = fmt::format(kDownloadKeyFmt, t_token);
    co_await del(t_key);
}

drogon::Task<BackendResult<std::string>> RedisMiddleware::getPresignedUrl(const std::string& t_object_key) {
    std::string t_key = fmt::format(kPresignedUrlKeyFmt, t_object_key);
    co_return co_await get(t_key);
}

drogon::Task<void> RedisMiddleware::cachePresignedUrl(const std::string& t_object_key, const std::string& t_url, int t_ttl_seconds) {
    std::string t_key = fmt::format(kPresignedUrlKeyFmt, t_object_key);
    co_await set(t_key, t_url, t_ttl_seconds);
}

drogon::Task<void> RedisMiddleware::invalidatePresignedUrl(const std::string& t_object_key) {
    std::string t_key = fmt::format(kPresignedUrlKeyFmt, t_object_key);
    co_await del(t_key);
}

} // namespace sgrn::datastore::plugins
