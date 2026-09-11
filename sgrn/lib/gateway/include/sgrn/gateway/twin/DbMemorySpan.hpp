#pragma once
#include <cstddef>
#include <cstdint>

namespace sgrn::gateway::twin
{

struct DbMemorySpan {
    uint16_t db;
    size_t offset;
    size_t size;
    uint8_t* p_buffer;
};

} // namespace sgrn::gateway::twin
