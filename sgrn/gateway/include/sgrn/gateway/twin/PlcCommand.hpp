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
    std::vector<uint8_t> value_binary;

    // Populated by adapters (e.g. OPC UA write_handler) so PlcCommandProcessor
    // can coalesce without a second symbol lookup.
    uint16_t db_number{0};
    size_t offset{0};
    size_t size{0};

    std::vector<DbMemorySpan> batch_spans;

    uint64_t timestamp;
};

} // namespace sgrn::gateway::twin
