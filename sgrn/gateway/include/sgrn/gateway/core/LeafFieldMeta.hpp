#pragma once

#include <cstdint>
#include <memory>
#include <s7codec/types.hpp>
#include <string>
#include <vector>

namespace sgrn::gateway::core
{

/// Metadata for a typed leaf field update (avoids JSON round-trip for OPC UA push).
struct LeafFieldMeta {
    s7codec::Type type{s7codec::Type::Byte};
    uint32_t field_size{0};
    uint32_t array_length{0};
    int elem_ua_type_index{-1};
    std::string udt_name;
    bool valid{false};
};

struct TypedLeafPayload {
    LeafFieldMeta meta;
    std::shared_ptr<std::vector<uint8_t>> bytes;
};

} // namespace sgrn::gateway::core
