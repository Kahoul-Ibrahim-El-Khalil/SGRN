#include <sgrn/datastore/utils/rate_limit.hpp>

#include <drogon/utils/Utilities.h>

#include <algorithm>
#include <cctype>

namespace sgrn::datastore::ratelimit
{

namespace
{

uint32_t jsonUint(const Json::Value& t_obj, const char* t_key, uint32_t t_dflt) {
    if (!t_obj.isMember(t_key)) {
        return t_dflt;
    }
    const Json::Value& v = t_obj[t_key];
    // Explicit zero is honored (a zero budget blocks the class — fail
    // closed, loudly); only absent/wrong-typed/negative values fall back.
    if (v.isUInt()) {
        return v.asUInt();
    }
    if (v.isInt() && v.asInt() >= 0) {
        return static_cast<uint32_t>(v.asInt());
    }
    return t_dflt;
}

uint64_t jsonWindowMs(const Json::Value& t_obj, const char* t_key, uint64_t t_dflt_ms) {
    if (!t_obj.isMember(t_key)) {
        return t_dflt_ms;
    }
    const Json::Value& v = t_obj[t_key];
    uint64_t seconds = 0;
    if (v.isUInt()) {
        seconds = v.asUInt64();
    } else if (v.isInt() && v.asInt() > 0) {
        seconds = static_cast<uint64_t>(v.asInt());
    } else {
        return t_dflt_ms;
    }
    return seconds == 0 ? t_dflt_ms : seconds * 1000;
}

bool startsWith(std::string_view t_in, std::string_view t_prefix) {
    return t_in.size() >= t_prefix.size() && t_in.substr(0, t_prefix.size()) == t_prefix;
}

std::string className(RateClass t_class) {
    switch (t_class) {
        case RateClass::Auth:
            return "auth";
        case RateClass::Storage:
            return "storage";
        case RateClass::Page:
            return "page";
        case RateClass::General:
            return "general";
        case RateClass::Upload:
            return "upload";
    }
    return "general";
}

std::string hexDigest(std::string_view t_in) {
    // drogon::utils::getSha256 returns lowercase hex.
    return drogon::utils::getSha256(t_in.data(), t_in.size());
}

std::string jsonStringField(const Json::Value& t_obj, const char* t_key) {
    if (!t_obj.isObject() || !t_obj.isMember(t_key)) {
        return {};
    }
    const Json::Value& v = t_obj[t_key];
    return v.isString() ? v.asString() : std::string{};
}

} // namespace

RateLimitConfig RateLimitConfig::fromJson(const Json::Value& t_custom_config) {
    RateLimitConfig cfg;
    if (!t_custom_config.isObject() || !t_custom_config.isMember("rate_limiting")) {
        return cfg;
    }
    const Json::Value& rl = t_custom_config["rate_limiting"];
    if (!rl.isObject()) {
        return cfg;
    }
    if (rl.isMember("enabled") && rl["enabled"].isBool()) {
        cfg.enabled = rl["enabled"].asBool();
    }
    cfg.auth = RateLimit{
        jsonUint(rl, "auth_endpoint_limit", cfg.auth.max_requests), jsonWindowMs(rl, "auth_endpoint_window_s", cfg.auth.window_ms)};
    cfg.storage = RateLimit{jsonUint(rl, "storage_endpoint_limit", cfg.storage.max_requests),
        jsonWindowMs(rl, "storage_endpoint_window_s", cfg.storage.window_ms)};
    cfg.general = RateLimit{jsonUint(rl, "general_endpoint_limit", cfg.general.max_requests),
        jsonWindowMs(rl, "general_endpoint_window_s", cfg.general.window_ms)};
    cfg.page = RateLimit{
        jsonUint(rl, "page_endpoint_limit", cfg.page.max_requests), jsonWindowMs(rl, "page_endpoint_window_s", cfg.page.window_ms)};
    cfg.upload = RateLimit{
        jsonUint(rl, "upload_endpoint_limit", cfg.upload.max_requests), jsonWindowMs(rl, "upload_endpoint_window_s", cfg.upload.window_ms)};
    cfg.burst_allowance = jsonUint(rl, "burst_allowance", cfg.burst_allowance);
    return cfg;
}

RateClass classifyPath(std::string_view t_path) {
    // Strip any query string defensively (drogon paths normally exclude it).
    const std::string_view path = t_path.substr(0, t_path.find('?'));
    if (startsWith(path, "/api/v1/auth/")) {
        return RateClass::Auth;
    }
    // Resumable upload chunk submissions are placed in a dedicated high-capacity Upload class
    // (a single 1.2 GB file at 5 MB chunks requires 240+ requests).
    if (startsWith(path, "/api/v1/storage/upload/")) {
        return RateClass::Upload;
    }
    if (startsWith(path, "/api/v1/storage/")) {
        return RateClass::Storage;
    }
    if (path == "/" || path == "/index.html" || startsWith(path, "/assets/")) {
        return RateClass::Page;
    }
    return RateClass::General;
}

uint32_t effectiveLimit(const RateLimitConfig& t_cfg, RateClass t_class) {
    switch (t_class) {
        case RateClass::Auth:
            return t_cfg.auth.max_requests;
        case RateClass::Storage:
            return t_cfg.storage.max_requests + t_cfg.burst_allowance;
        case RateClass::Page:
            return t_cfg.page.max_requests + t_cfg.burst_allowance;
        case RateClass::General:
            return t_cfg.general.max_requests + t_cfg.burst_allowance;
        case RateClass::Upload:
            return t_cfg.upload.max_requests + t_cfg.burst_allowance;
    }
    return t_cfg.general.max_requests + t_cfg.burst_allowance;
}

uint64_t windowMs(const RateLimitConfig& t_cfg, RateClass t_class) {
    switch (t_class) {
        case RateClass::Auth:
            return t_cfg.auth.window_ms;
        case RateClass::Storage:
            return t_cfg.storage.window_ms;
        case RateClass::Page:
            return t_cfg.page.window_ms;
        case RateClass::General:
            return t_cfg.general.window_ms;
        case RateClass::Upload:
            return t_cfg.upload.window_ms;
    }
    return t_cfg.general.window_ms;
}

std::string buildKey(RateClass t_class, std::string_view t_ip, std::string_view t_account) {
    std::string raw;
    raw.reserve(t_ip.size() + t_account.size() + 1);
    raw.append(t_ip.data(), t_ip.size());
    raw += '|';
    raw.append(t_account.data(), t_account.size());
    return "sgrn:rl:" + className(t_class) + ":" + hexDigest(raw);
}

std::string authAccountId(const drogon::HttpRequestPtr& t_req) {
    if (!t_req) {
        return {};
    }
    Json::Value body;
    try {
        auto json = t_req->getJsonObject();
        if (!json) {
            return {};
        }
        body = *json;
    } catch (const std::exception&) {
        return {};
    }
    return authAccountFromJson(t_req->path(), body);
}

std::string authAccountFromJson(std::string_view t_path, const Json::Value& t_body) {
    if (classifyPath(t_path) != RateClass::Auth) {
        return {};
    }
    // User signin carries the account name in the clear; automated-service
    // signin carries a secret, so only its digest may leave the request.
    const std::string email = jsonStringField(t_body, "email");
    if (!email.empty()) {
        std::string lower = email;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
        return "email:" + lower;
    }
    const std::string token = jsonStringField(t_body, "token");
    if (!token.empty()) {
        return "token:" + hexDigest(token);
    }
    const std::string secret = jsonStringField(t_body, "secret");
    if (!secret.empty()) {
        return "secret:" + hexDigest(secret);
    }
    return {};
}

} // namespace sgrn::datastore::ratelimit
