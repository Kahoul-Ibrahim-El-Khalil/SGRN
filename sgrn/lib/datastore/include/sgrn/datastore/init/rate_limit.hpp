#pragma once

#include <sgrn/datastore/utils/rate_limit.hpp>
#include <sgrn/datastore/utils/respond.hpp>

#include <drogon/HttpAppFramework.h>
#include <sgrn/debug.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

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

// In-process sliding-window state: per-key sorted list of timestamps (ms).
// Entries older than the window are evicted lazily on each check.
// Uses shared_mutex — reads (ZCARD equivalent) allow concurrency while
// writes (ZADD + eviction) take exclusive ownership.
struct InProcWindowStore {
    mutable std::shared_mutex mutex;
    // key → deque of timestamps (ascending, milliseconds since epoch)
    std::unordered_map<std::string, std::deque<uint64_t>> windows;

    // Returns Decision. Evicts stale entries, checks count, inserts if allowed.
    Decision check(const std::string& t_key, uint32_t t_limit, uint64_t t_window_ms) {
        const uint64_t now_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        const uint64_t cutoff = now_ms > t_window_ms ? now_ms - t_window_ms : 0;

        std::unique_lock lock(mutex);
        auto& dq = windows[t_key];

        // Evict expired entries
        while (!dq.empty() && dq.front() <= cutoff) {
            dq.pop_front();
        }

        const uint64_t count = dq.size();
        if (count >= t_limit) {
            // Retry after oldest entry exits the window
            uint64_t retry_after_ms = dq.empty() ? t_window_ms : (dq.front() + t_window_ms - now_ms);
            if (static_cast<int64_t>(retry_after_ms) < 0)
                retry_after_ms = 0;
            return Decision{false, retry_after_ms, 0};
        }

        dq.push_back(now_ms);
        return Decision{true, 0, t_limit - count - 1};
    }
};

inline InProcWindowStore& windowStore() {
    static InProcWindowStore store;
    return store;
}

// In-process sliding-window rate limiting enforced centrally in a pre-routing advice.
// Every request (API, embedded UI assets, even would-be 404s) is classified by path
// and checked against a per-key in-memory sliding window.
// No Redis required; no route table changes needed — generated CRUD views are
// covered automatically. Decisions are made in <1µs from the shared_mutex fast path.
inline void initRateLimiting() {
    RateLimitConfig cfg = RateLimitConfig::fromJson(drogon::app().getCustomConfig());
    if (!cfg.enabled) {
        SGRN_INFO("SGRN-Datastore", "Rate limiting disabled by configuration");
        return;
    }
    auto holder = rateLimitHolder();
    holder->enabled_at_boot = true;
    holder->publish(cfg);
    drogon::app().registerPreRoutingAdvice(
        [holder](const drogon::HttpRequestPtr& tsp_req, drogon::AdviceCallback&& t_respond, drogon::AdviceChainCallback&& t_proceed) {
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

            Decision decision = windowStore().check(key, limit, window);
            if (decision.allowed) {
                t_proceed();
                return;
            }

            const uint64_t retry_s = (decision.retry_after_ms + 999) / 1000;
            auto resp = sgrn::createErrorResponse("Rate limit exceeded, retry later", drogon::k429TooManyRequests, "RateLimit");
            resp->addHeader("Retry-After", std::to_string(retry_s < 1 ? 1 : retry_s));
            t_respond(resp);
        });
    SGRN_INFO("SGRN-Datastore", "Rate limiting enabled (auth {}/{}s, storage {}/{}s, general {}/{}s, page {}/{}s, upload {}/{}s)",
        cfg.auth.max_requests, cfg.auth.window_ms / 1000, cfg.storage.max_requests, cfg.storage.window_ms / 1000, cfg.general.max_requests,
        cfg.general.window_ms / 1000, cfg.page.max_requests, cfg.page.window_ms / 1000, cfg.upload.max_requests,
        cfg.upload.window_ms / 1000);
}

} // namespace sgrn::datastore::ratelimit
