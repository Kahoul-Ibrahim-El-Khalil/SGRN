#pragma once

#include <drogon/HttpRequest.h>

#include <cstdint>
#include <json/json.h>
#include <string>
#include <string_view>

namespace sgrn::datastore::ratelimit
{

// Request classes, each with its own sliding-window budget. Auth is the
// brute-force surface (strict, no burst); storage guards expensive object
// paths; page covers the embedded UI assets (generous — a page load is a
// handful of requests, a flood is not); general is everything else.
enum class RateClass { Auth, Storage, Page, General };

struct RateLimit {
    uint32_t max_requests = 100;
    uint64_t window_ms = 60000;
};

struct RateLimitConfig {
    bool enabled = true;
    RateLimit auth{5, 60000};
    RateLimit storage{30, 60000};
    RateLimit general{100, 60000};
    RateLimit page{120, 60000};
    // Extra headroom added to storage/general/page caps (absorbs legitimate
    // bursts). Deliberately NOT applied to auth: brute-force budgets stay
    // exact.
    uint32_t burst_allowance = 10;

    // Reads custom_config["rate_limiting"]; missing keys keep the defaults
    // above so older configs keep working unchanged.
    static RateLimitConfig fromJson(const Json::Value& t_custom_config);
};

// Pure path classifier (no I/O — unit-tested). In-app paths: nginx strips
// the /datastore prefix before proxying, so the UI arrives as /, /index.html
// and /assets/*. API paths keep their /api/v1 prefix.
RateClass classifyPath(std::string_view t_path);

// Effective request cap for a class (limit + burst, except auth).
uint32_t effectiveLimit(const RateLimitConfig& t_cfg, RateClass t_class);

// Window for a class, milliseconds.
uint64_t windowMs(const RateLimitConfig& t_cfg, RateClass t_class);

// Redis key for a caller. The account part is always a hex digest, never the
// raw identifier: tokens must not land in the Redis keyspace (or slowlogs),
// and emails are PII. Format: sgrn:rl:<class>:<hex>.
std::string buildKey(RateClass t_class, std::string_view t_ip, std::string_view t_account);

// Account identifier for auth paths: the signin email, or the hex digest of
// an automated-service token (the token itself must never leave the request).
// Empty when the path is not an auth signin or no identifier is present —
// callers then fall back to IP-only keys.
std::string authAccountId(const drogon::HttpRequestPtr& t_req);

// Pure body-parsing half of authAccountId (unit-tested directly).
std::string authAccountFromJson(std::string_view t_path, const Json::Value& t_body);

struct Decision {
    bool allowed = true;
    uint64_t retry_after_ms = 0;
    uint64_t remaining = 0;
};

// Atomic sliding-window check (single EVAL round trip):
//   ZREMRANGEBYSCORE key 0 <now-window>
//   ZCARD -> count; deny when count >= limit (reporting when the oldest
//   entry ages out as retry_after_ms), else ZADD now+unique member,
//   PEXPIRE window, allow with remaining budget.
// Returns {allowed, retry_after_ms} on the wire; the C++ side derives
// `remaining` for allowed replies.
extern const char* kLuaSlidingWindow;

} // namespace sgrn::datastore::ratelimit
