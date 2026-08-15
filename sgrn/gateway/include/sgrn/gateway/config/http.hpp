#pragma once

#include <cstdint>
#include <string>

namespace sgrn::gateway::config
{

struct HttpConfig {
    std::string ip{"0.0.0.0"};
    uint16_t port{8080};
};

} // namespace sgrn::gateway::config
