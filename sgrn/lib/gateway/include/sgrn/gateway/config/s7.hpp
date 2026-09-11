#pragma once

#include <cstdint>
#include <string>

namespace sgrn::gateway::config
{

struct S7Config {
    std::string ip{"0.0.0.0"};
    uint16_t port{102};
    uint32_t max_clients{8};
    uint32_t pdu_size{960};
    bool little_endian{true}; // default when S7 is absent (generic device)
};

} // namespace sgrn::gateway::config
