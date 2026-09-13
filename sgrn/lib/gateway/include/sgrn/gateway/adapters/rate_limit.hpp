#pragma once

#include <crow.h>

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace sgrn::gateway::adapters
{

/// Per-client-IP fixed-window rate limiter as a Crow middleware.
///
/// Attach per route with `.CROW_MIDDLEWARES(app, RateLimitMiddleware)`
/// (Crow only runs middleware — global or local — for routes carrying
/// explicit middleware indices, so every route needs the annotation; the
/// dynamic asset rules and the SPA catchall enforce the shared checkRequest()
/// core manually for the same reason). Skips CORS preflights (OPTIONS) and
/// WebSocket handshakes (Upgrade) — the former must stay cheap, the latter
/// is a single request per connection while frames never re-enter
/// middleware.
///
/// State is in-process memory (mutex-guarded): correct for the shipped
/// single-instance gateway. Multi-instance deployments must either pin
/// clients (sticky sessions) or replace this with the shared Redis limiter
/// the datastore uses. Budgets come from HttpConfig (defaults below) via
/// configure(), called once from the application startup path.
struct RateLimitMiddleware : public crow::ILocalMiddleware {
    struct context {};

    static void configure(uint32_t t_max_requests_per_window, uint64_t t_window_seconds) noexcept {
        s_max_requests = t_max_requests_per_window == 0 ? kDefaultMax : t_max_requests_per_window;
        s_window = std::chrono::seconds(t_window_seconds == 0 ? kDefaultWindowS : t_window_seconds);
    }

    void before_handle(crow::request& t_req, crow::response& t_res, context&) {
        checkRequest(t_req, t_res);
    }

    void after_handle(crow::request&, crow::response&, context&) {
        // Nothing to do post-handling.
    }

    // Shared core behind before_handle. Returns true when the request may
    // proceed; on false, t_res already carries the 429 with Retry-After.
    static bool checkRequest(const crow::request& t_req, crow::response& t_res) {
        if (t_req.method == crow::HTTPMethod::Options) {
            return true;
        }
        const auto upgrade = t_req.get_header_value("Upgrade");
        if (!upgrade.empty()) {
            return true; // WebSocket handshake: limit nothing past this point
        }
        const std::string ip = clientIp(t_req);
        const auto now = std::chrono::steady_clock::now();
        uint64_t retry_s = 0;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            if (s_windows.size() > kMaxTrackedIps) {
                pruneExpired(now);
                if (s_windows.size() > kMaxTrackedIps) {
                    return true; // fail open rather than grow without bound
                }
            }
            Window& w = s_windows[ip];
            if (now - w.start >= s_window) {
                w.start = now;
                w.count = 0;
            }
            if (++w.count > s_max_requests) {
                retry_s = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(w.start + s_window - now).count()) + 1;
            }
        }
        if (retry_s == 0) {
            return true;
        }
        t_res.code = 429;
        t_res.set_header("Content-Type", "application/json");
        t_res.set_header("Retry-After", std::to_string(retry_s));
        t_res.write(R"({"error":"Rate limit exceeded, retry later","scope":"RateLimit"})");
        t_res.end();
        return false;
    }

private:
    static constexpr uint32_t kDefaultMax = 600; // requests per window per IP
    static constexpr uint64_t kDefaultWindowS = 60;
    static constexpr std::size_t kMaxTrackedIps = 100000;

    struct Window {
        std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
        uint64_t count{0};
    };

    static std::string clientIp(const crow::request& t_req) {
        // Prefer proxy headers (the shipped nginx sets X-Real-IP); fall back
        // to the direct peer. X-Forwarded-For may chain — first entry wins.
        const std::string real = t_req.get_header_value("X-Real-IP");
        if (!real.empty()) {
            return real;
        }
        const std::string fwd = t_req.get_header_value("X-Forwarded-For");
        const std::string::size_type comma = fwd.find(',');
        if (comma != std::string::npos) {
            return fwd.substr(0, comma);
        }
        if (!fwd.empty()) {
            return fwd;
        }
        return t_req.remote_ip_address;
    }

    static void pruneExpired(const std::chrono::steady_clock::time_point& t_now) {
        for (auto it = s_windows.begin(); it != s_windows.end();) {
            if (t_now - it->second.start >= s_window) {
                it = s_windows.erase(it);
            } else {
                ++it;
            }
        }
    }

    static std::mutex s_mutex;
    static std::unordered_map<std::string, Window> s_windows;
    static uint32_t s_max_requests;
    static std::chrono::seconds s_window;
};

inline std::mutex RateLimitMiddleware::s_mutex;
inline std::unordered_map<std::string, RateLimitMiddleware::Window> RateLimitMiddleware::s_windows;
inline uint32_t RateLimitMiddleware::s_max_requests{RateLimitMiddleware::kDefaultMax};
inline std::chrono::seconds RateLimitMiddleware::s_window{RateLimitMiddleware::kDefaultWindowS};

/// Crow application type for every gateway listener: identical to
/// crow::SimpleApp plus the global rate-limit middleware.
using GatewayApp = crow::App<RateLimitMiddleware>;

} // namespace sgrn::gateway::adapters
