#pragma once

#include <sgrn/datastore/utils/rate_limit.hpp>
#include <sgrn/datastore/utils/respond.hpp>

#include <drogon/HttpAppFramework.h>
#include <drogon/nosql/RedisClient.h>
#include <sgrn/debug.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

namespace sgrn::datastore::ratelimit
{

// Live rate-limit config. initRateLimiting() seeds the holder at boot and the
// pre-routing advice reads a copy per request; the admin system-config
// endpoint republishes after a validated save, so limit/window edits apply
// without a restart. Copies are tiny (a few ints); a plain mutex is enough.
//
// One deliberate exception: flipping `enabled` needs a restart. The advice
// itself is only registered when enabled at boot, so enabling at runtime
// would silently do nothing — the PUT handler reports that path as
// restart-required instead of pretending it applied.
struct RateLimitHolder {
    mutable std::mutex mutex;
    std::shared_ptr<const RateLimitConfig> config = std::make_shared<const RateLimitConfig>();
    bool enabled_at_boot = false;

    RateLimitConfig snapshot() const {
        std::lock_guard lock(mutex);
        return *config;
    }
    void publish(RateLimitConfig t_cfg) {
        std::lock_guard lock(mutex);
        config = std::make_shared<const RateLimitConfig>(std::move(t_cfg));
    }
};

inline std::shared_ptr<RateLimitHolder>& rateLimitHolder() {
    static auto holder = std::make_shared<RateLimitHolder>();
    return holder;
}

inline void publishRateLimitConfig(RateLimitConfig t_cfg) {
    rateLimitHolder()->publish(std::move(t_cfg));
}

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
    auto holder = rateLimitHolder();
    holder->enabled_at_boot = true;
    holder->publish(cfg);
    drogon::app().registerPreRoutingAdvice([holder](const drogon::HttpRequestPtr& tsp_req, drogon::AdviceCallback&& t_respond,
                                               drogon::AdviceChainCallback&& t_proceed) {
        if (tsp_req->method() == drogon::Options) {
            t_proceed(); // CORS preflights are never limited
            return;
        }
        const RateLimitConfig live = holder->snapshot();
        const RateClass cls = classifyPath(tsp_req->path());
        const std::string ip = tsp_req->getPeerAddr().toIp();
        const std::string key = buildKey(cls, ip, authAccountId(tsp_req));
        const uint32_t limit = effectiveLimit(live, cls);
        const uint64_t window = windowMs(live, cls);

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
