#pragma once

#include <cstdint>
#include <string>

namespace sgrn::gateway::config
{

struct OpcUaConfig {
    std::string ip{"0.0.0.0"};
    uint16_t port{4840};
};

} // namespace sgrn::gateway::config
