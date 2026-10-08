#pragma once
// TagAccess — discrete (TIA-style) tag projection hooks for OpcUaAdapter.
//
// The twin is DB-only, so tag bytes live wherever a PlcRuntime exists.
// Owners with a runtime (s7shell OpcUaServer binding) install these via
// OpcUaAdapter::setTagAccess(); the adapter then serves a "Tags" folder.
// Null (default) = DB-only address space, unchanged behavior.
//
// Raw bytes are canonical S7 layout with one normalization: Bool tags arrive
// as a single byte holding 0/1 at bit 0 (the shared scalar decode path
// hardcodes bit 0).
#include <sgrn/Result.hpp>
#include <sgrn/scl/types.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sgrn::gateway::adapters
{

/// Backing store callbacks for one tag projection. Spans address tag bytes
/// relative to the tag start ([0, span)); bit_index >= 0 normalizes a single
/// bit into/out of bit 0 of a one-byte buffer (the shared scalar decode
/// hardcodes bit 0).
struct TagBacking {
    std::function<sgrn::Result<std::vector<uint8_t>, std::string>(
        const std::string& t_tag_name, size_t t_byte_offset, size_t t_size, int t_bit_index)>
        read_bytes;
    std::function<sgrn::Result<void, std::string>(
        const std::string& t_tag_name, size_t t_byte_offset, const std::vector<uint8_t>& t_bytes, int t_bit_index)>
        write_bytes;
};

/// One tag row for address-space registration (mirrors RuntimeTag describe).
struct TagDescriptor {
    std::string name;
    std::string table;
    std::string type_str;
    std::string udt_name; ///< non-empty for UDT-typed tags (projected as member nodes)
    ::sgrn::scl::DataType type{::sgrn::scl::DataType::Bool};
    int span_bytes{1};
    int bit_index{-1}; ///< bit position for Bool tags, -1 otherwise
};

/// Tag enumeration + backing, installed via OpcUaAdapter::setTagAccess().
struct TagAccess {
    std::function<std::vector<TagDescriptor>()> list;
    std::shared_ptr<const TagBacking> backing;
};

} // namespace sgrn::gateway::adapters
