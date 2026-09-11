#pragma once

#include <cstdint>
#include <string>

namespace sgrn::gateway::config
{

struct WebSocketConfig {
    std::string ip{"0.0.0.0"};
    uint16_t port{8081};
};

} // namespace sgrn::gateway::config
