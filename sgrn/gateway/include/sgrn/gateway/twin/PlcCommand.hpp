#pragma once
#include "sgrn/gateway/twin/DbMemorySpan.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace sgrn::gateway::twin
{

struct PlcCommand {
    enum Type { WriteField, WriteBinary, WriteBatch };
    Type type;
    std::string path;
    std::string value_json;
    uint64_t timestamp;
};

} // namespace sgrn::gateway::twin
