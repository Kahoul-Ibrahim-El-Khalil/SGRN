// Rate limiter unit tests (pure logic only — no Redis, no app singleton).
//
// Covers the parts of the pre-routing limiter that don't need I/O:
// path classification, config defaults/overrides, effective caps (burst
// applies everywhere except auth), key construction (deterministic,
// account-separating, never embedding raw secrets), and auth account
// extraction from signin bodies. The Redis Lua window itself is exercised
// against live stacks, not here.

#include <sgrn/datastore/utils/rate_limit.hpp>

#include <drogon/HttpRequest.h>
#include <json/json.h>

#include <cstdio>
#include <string>

namespace
{

int g_failures = 0;

#define CHECK(cond)                                                                                                                        \
    do {                                                                                                                                   \
        if (!(cond)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                                    \
        }                                                                                                                                  \
    } while (0)

using sgrn::datastore::ratelimit::authAccountFromJson;
using sgrn::datastore::ratelimit::authAccountId;
using sgrn::datastore::ratelimit::buildKey;
using sgrn::datastore::ratelimit::classifyPath;
using sgrn::datastore::ratelimit::effectiveLimit;
using sgrn::datastore::ratelimit::RateClass;
using sgrn::datastore::ratelimit::RateLimitConfig;
using sgrn::datastore::ratelimit::windowMs;

Json::Value parseJson(const std::string& t_raw) {
    Json::Value v;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    reader->parse(t_raw.data(), t_raw.data() + t_raw.size(), &v, &errors);
    return v;
}

} // namespace

int main() {
    // --- path classification ------------------------------------------------
    CHECK(classifyPath("/api/v1/auth/user/signin") == RateClass::Auth);
    CHECK(classifyPath("/api/v1/auth/automated-service/signin") == RateClass::Auth);
    CHECK(classifyPath("/api/v1/auth/user/password") == RateClass::Auth);
    CHECK(classifyPath("/api/v1/storage/files") == RateClass::Storage);
    CHECK(classifyPath("/api/v1/storage/drive/list") == RateClass::Storage);
    // Resumable upload endpoints — dedicated high-capacity Upload rate class
    CHECK(classifyPath("/api/v1/storage/upload/chunk") == RateClass::Upload);
    CHECK(classifyPath("/api/v1/storage/upload/init") == RateClass::Upload);
    CHECK(classifyPath("/api/v1/storage/upload/complete") == RateClass::Upload);
    CHECK(classifyPath("/api/v1/storage/upload/status") == RateClass::Upload);
    CHECK(classifyPath("/api/v1/storage/upload/abort") == RateClass::Upload);
    CHECK(classifyPath("/") == RateClass::Page);
    CHECK(classifyPath("/index.html") == RateClass::Page);
    CHECK(classifyPath("/assets/app.abc123.js") == RateClass::Page);
    CHECK(classifyPath("/api/v1/domains") == RateClass::General);
    CHECK(classifyPath("/api/v1/admin/users") == RateClass::General);
    CHECK(classifyPath("/api/v1/admin/storage/overview") == RateClass::General);
    CHECK(classifyPath("/nonexistent") == RateClass::General);
    CHECK(classifyPath("") == RateClass::General);
    CHECK(classifyPath("/api/v1/auth/user/signin?next=/drive") == RateClass::Auth); // query stripped
    CHECK(classifyPath("/api/v1/auth") == RateClass::General);                      // prefix itself is not a route
    CHECK(classifyPath("/api/v1/authority") == RateClass::General);                 // no partial-prefix match

    // --- config defaults ----------------------------------------------------
    {
        const RateLimitConfig cfg = RateLimitConfig::fromJson(Json::Value(Json::objectValue));
        CHECK(cfg.enabled);
        CHECK(effectiveLimit(cfg, RateClass::Auth) == 5);
        CHECK(effectiveLimit(cfg, RateClass::Storage) == 40); // 30 + burst 10
        CHECK(effectiveLimit(cfg, RateClass::General) == 110);
        CHECK(effectiveLimit(cfg, RateClass::Page) == 130);
        CHECK(effectiveLimit(cfg, RateClass::Upload) == 10010); // 10000 + burst 10
        CHECK(windowMs(cfg, RateClass::Auth) == 60000);
    }
    // --- config overrides + invalid values fall back -------------------------
    {
        const Json::Value custom = parseJson(
            R"({"rate_limiting":{"enabled":false,"auth_endpoint_limit":2,"auth_endpoint_window_s":30,
               "general_endpoint_limit":"lots","storage_endpoint_limit":0,"burst_allowance":3}})");
        const RateLimitConfig cfg = RateLimitConfig::fromJson(custom);
        CHECK(!cfg.enabled);
        CHECK(effectiveLimit(cfg, RateClass::Auth) == 2); // strict: no burst added
        CHECK(windowMs(cfg, RateClass::Auth) == 30000);
        CHECK(effectiveLimit(cfg, RateClass::General) == 103); // bad string -> default 100 + burst 3
        CHECK(effectiveLimit(cfg, RateClass::Storage) == 3);   // explicit 0 honored: 0 + burst 3
    }
    // --- key construction ----------------------------------------------------
    {
        const std::string k1 = buildKey(RateClass::Auth, "10.0.0.1", "email:a@b.c");
        const std::string k2 = buildKey(RateClass::Auth, "10.0.0.1", "email:a@b.c");
        CHECK(k1 == k2); // deterministic
        CHECK(k1.rfind("sgrn:rl:auth:", 0) == 0);
        CHECK(buildKey(RateClass::Auth, "10.0.0.1", "email:a@b.c") != buildKey(RateClass::Auth, "10.0.0.1", "email:x@y.z"));
        CHECK(buildKey(RateClass::Auth, "10.0.0.1", "") != buildKey(RateClass::Auth, "10.0.0.2", ""));
        CHECK(buildKey(RateClass::Auth, "10.0.0.1", "") != buildKey(RateClass::General, "10.0.0.1", ""));
        CHECK(k1.find("a@b.c") == std::string::npos); // raw PII never in the key
        const std::string secret = "super-secret-token";
        const std::string ks = buildKey(RateClass::Auth, "10.0.0.1", secret);
        CHECK(ks.find(secret) == std::string::npos); // raw secrets never in the key
    }
    // --- auth account extraction ---------------------------------------------
    {
        CHECK(authAccountFromJson("/api/v1/auth/user/signin", parseJson("{\"email\":\"Admin@Local.com\"}")) == "email:admin@local.com");
        const std::string tok = authAccountFromJson("/api/v1/auth/automated-service/signin", parseJson("{\"token\":\"s3cr3t\"}"));
        CHECK(tok.rfind("token:", 0) == 0 && tok.size() > 16 && tok.find("s3cr3t") == std::string::npos);
        const std::string sec = authAccountFromJson("/api/v1/auth/automated-service/signin", parseJson("{\"secret\":\"s3cr3t\"}"));
        CHECK(sec.rfind("secret:", 0) == 0 && sec.find("s3cr3t") == std::string::npos);
        CHECK(authAccountFromJson("/api/v1/auth/user/signin", parseJson("{}")).empty());
        CHECK(authAccountFromJson("/api/v1/auth/user/signin", parseJson("{\"email\":42}")).empty());
        CHECK(authAccountFromJson("/api/v1/domains", parseJson("{\"email\":\"a@b.c\"}")).empty()); // non-auth path
        auto req = drogon::HttpRequest::newHttpRequest();                                          // standalone request: path "/", no body
        CHECK(authAccountId(req).empty());
        CHECK(authAccountId(nullptr).empty());
    }

    if (g_failures == 0) {
        std::printf("rate_limit_test: ALL CHECKS PASSED\n");
        return 0;
    }
    std::printf("rate_limit_test: %d FAILURES\n", g_failures);
    return 1;
}
