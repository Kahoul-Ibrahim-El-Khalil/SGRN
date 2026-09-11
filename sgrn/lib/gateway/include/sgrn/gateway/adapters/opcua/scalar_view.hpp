#pragma once

#include <cstdint>
#include <optional>

#include <s7codec/types.hpp>

namespace sgrn::gateway::twin
{
struct PlcNode;
} // namespace sgrn::gateway::twin

namespace sgrn::gateway::adapters
{

/// Shallow, allocation-free projection of the scalar-layout fields of
/// twin::PlcNode (see twin/PlcState.hpp). Copying a full PlcNode per array
/// element deep-copies name_/full_path_/children_ (a recursive vector) plus an
/// atomic version counter — measurable waste inside the struct-array
/// encode/decode loops. The scalar codec paths touch ONLY the fields below,
/// so hot loops pass this POD around instead.
struct PlcScalarView {
    s7codec::Type type{s7codec::Type::Byte};
    s7codec::Endian endian{s7codec::Endian::Big};
    uint32_t size{0};
    uint32_t count{1};
    uint8_t bit_index{0};
    uint32_t string_capacity{0};
    std::optional<double> min_val{std::nullopt};
    std::optional<double> max_val{std::nullopt};
};

/// POD-only projection of t_node's scalar layout (no strings, no children_,
/// no atomic copy). Defined in node_registration.cpp next to
/// NodeContext::resolveSymbol().
PlcScalarView makeScalarView(const twin::PlcNode& t_node);

} // namespace sgrn::gateway::adapters
