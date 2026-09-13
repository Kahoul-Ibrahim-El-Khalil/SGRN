#pragma once

#include <cstdint>
#include <string>

namespace sgrn::gateway::config
{

struct HttpConfig {
    std::string ip{"0.0.0.0"};
    uint16_t port{8080};
    // Fixed-window per-IP budget enforced by RateLimitMiddleware on every
    // route (OPTIONS preflights and WS handshakes excluded). Zero disables.
    uint32_t rate_limit_max_requests{600};
    uint64_t rate_limit_window_s{60};
};

} // namespace sgrn::gateway::config
