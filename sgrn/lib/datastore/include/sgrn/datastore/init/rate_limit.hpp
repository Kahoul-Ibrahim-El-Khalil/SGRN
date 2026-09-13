#pragma once

#include <sgrn/datastore/utils/rate_limit.hpp>
#include <sgrn/datastore/utils/respond.hpp>

#include <drogon/HttpAppFramework.h>
#include <drogon/nosql/RedisClient.h>
#include <sgrn/debug.hpp>

#include <atomic>
#include <chrono>
#include <memory>

namespace sgrn::datastore::ratelimit
{

// Sliding-window rate limiting enforced centrally in a pre-routing advice:
// every request (API, embedded UI assets, even would-be 404s) is classified
// by path and checked against a Redis Lua script (atomic, distributed-safe
// across instances). No route table changes needed — generated CRUD views
// are covered automatically.
//
// Keying: client IP, plus the account identifier on auth signin paths
// (email, or a digest of the service token — raw secrets never leave the
// request). Redis outage or misconfiguration fails OPEN (requests proceed):
// sessions already require Redis, so authed paths fail downstream anyway,
// while anonymous page serving stays up.
inline void initRateLimiting() {
    RateLimitConfig cfg = RateLimitConfig::fromJson(drogon::app().getCustomConfig());
    if (!cfg.enabled) {
        SGRN_INFO("SGRN-Datastore", "Rate limiting disabled by configuration");
        return;
    }
    auto shared = std::make_shared<RateLimitConfig>(cfg);
    drogon::app().registerPreRoutingAdvice([shared](const drogon::HttpRequestPtr& tsp_req, drogon::AdviceCallback&& t_respond,
                                               drogon::AdviceChainCallback&& t_proceed) {
        if (tsp_req->method() == drogon::Options) {
            t_proceed(); // CORS preflights are never limited
            return;
        }
        const RateClass cls = classifyPath(tsp_req->path());
        const std::string ip = tsp_req->getPeerAddr().toIp();
        const std::string key = buildKey(cls, ip, authAccountId(tsp_req));
        const uint32_t limit = effectiveLimit(*shared, cls);
        const uint64_t window = windowMs(*shared, cls);

        auto redis = drogon::app().getRedisClient();
        if (!redis) {
            t_proceed();
            return;
        }
        static std::atomic<uint64_t> member_seq{0};
        const uint64_t now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        const std::string member = std::to_string(now_ms) + ":" + std::to_string(member_seq.fetch_add(1, std::memory_order_relaxed));
        const std::string now_s = std::to_string(now_ms);
        const std::string window_s = std::to_string(window);
        const std::string limit_s = std::to_string(limit);
        redis->execCommandAsync(
            [t_respond = std::move(t_respond), t_proceed = std::move(t_proceed)](const drogon::nosql::RedisResult& r) mutable {
                try {
                    const auto parts = r.asArray();
                    const bool allowed = parts.size() >= 2 && parts[0].asInteger() == 1;
                    if (allowed) {
                        t_proceed();
                        return;
                    }
                    const uint64_t retry_ms = parts.size() >= 2 ? static_cast<uint64_t>(parts[1].asInteger()) : 0;
                    const uint64_t retry_s = (retry_ms + 999) / 1000 < 1 ? 1 : (retry_ms + 999) / 1000;
                    auto resp = sgrn::createErrorResponse("Rate limit exceeded, retry later", drogon::k429TooManyRequests, "RateLimit");
                    resp->addHeader("Retry-After", std::to_string(retry_s));
                    t_respond(resp);
                } catch (const std::exception&) {
                    t_proceed(); // malformed reply: fail open
                }
            },
            [t_proceed = std::move(t_proceed)](const std::exception&) mutable {
                t_proceed(); // Redis error/timeout: fail open
            },
            "EVAL %s 1 %s %s %s %s %s", kLuaSlidingWindow, key.c_str(), now_s.c_str(), window_s.c_str(), limit_s.c_str(), member.c_str());
    });
    SGRN_INFO("SGRN-Datastore", "Rate limiting enabled (auth {}/{}s, storage {}/{}s, general {}/{}s, page {}/{}s)", cfg.auth.max_requests,
        cfg.auth.window_ms / 1000, cfg.storage.max_requests, cfg.storage.window_ms / 1000, cfg.general.max_requests,
        cfg.general.window_ms / 1000, cfg.page.max_requests, cfg.page.window_ms / 1000);
}

} // namespace sgrn::datastore::ratelimit
