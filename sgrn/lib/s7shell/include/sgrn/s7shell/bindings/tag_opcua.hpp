#pragma once
// =============================================================================
// tag_opcua.hpp — hook a PlcRuntime's discrete tags into OpcUaAdapter.
//
// Serves a "Tags" folder with one live variable node per tag (scalars
// natively typed, UDTs as JSON strings). Reads/writes flow through the same
// shared backing as tags().get/put, the S7 server, HTTP /tags and the
// gateway deltas. Bool bytes are normalized (0/1 at bit 0) for the shared
// scalar decode path, which hardcodes bit 0.
// =============================================================================

#include <sgrn/gateway/adapters/opcua.hpp>
#include <sgrn/plcsim/runtime/PlcRuntime.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace sgrn::s7shell::bindings
{

inline void hookOpcuaTags(::sgrn::gateway::adapters::OpcUaAdapter* tp_adapter, const ::sgrn::plcsim::runtime::PlcRuntimeSPtr& t_rt) {
    if (!tp_adapter || !t_rt)
        return;
    using ::sgrn::gateway::adapters::TagAccess;
    using ::sgrn::gateway::adapters::TagBacking;
    using ::sgrn::gateway::adapters::TagDescriptor;

    auto backing = std::make_shared<TagBacking>();
    // Span core: tag-relative [offset, offset+size), bit >= 0 normalizes one
    // bit into/out of bit 0 (shared scalar codec hardcodes bit 0). Used by
    // top-level tags (their own span/bit) and UDT member nodes alike.
    backing->read_bytes = [t_rt](const std::string& t_name, size_t t_byte_offset, size_t t_size,
                              int t_bit) -> sgrn::Result<std::vector<uint8_t>, std::string> {
        auto d = t_rt->describeTag(t_name);
        if (d.hasError())
            return sgrn::Result<std::vector<uint8_t>, std::string>::Error("unknown tag '" + t_name + "'");
        const auto& tag = d.value();
        if (t_byte_offset + t_size > static_cast<size_t>(tag.span_bytes) || t_size == 0)
            return sgrn::Result<std::vector<uint8_t>, std::string>::Error("span out of tag range");
        std::vector<uint8_t> buf(t_size, 0);
        const size_t abs_off = static_cast<size_t>(tag.addr.byte_offset) + t_byte_offset;
        auto r = t_rt->readAreaMemory(tag.addr.area, abs_off, buf.size(), buf.data());
        if (r.hasError())
            return sgrn::Result<std::vector<uint8_t>, std::string>::Error(r.error());
        if (t_bit >= 0) {
            if (buf.size() != 1)
                return sgrn::Result<std::vector<uint8_t>, std::string>::Error("bit access needs one byte");
            buf[0] = static_cast<uint8_t>((buf[0] >> t_bit) & 1u);
        }
        return buf;
    };
    backing->write_bytes = [t_rt](const std::string& t_name, size_t t_byte_offset, const std::vector<uint8_t>& t_bytes,
                               int t_bit) -> sgrn::Result<void, std::string> {
        auto d = t_rt->describeTag(t_name);
        if (d.hasError())
            return sgrn::Result<void, std::string>::Error("unknown tag '" + t_name + "'");
        const auto& tag = d.value();
        if (t_byte_offset + t_bytes.size() > static_cast<size_t>(tag.span_bytes) || t_bytes.empty())
            return sgrn::Result<void, std::string>::Error("span out of tag range");
        const size_t abs_off = static_cast<size_t>(tag.addr.byte_offset) + t_byte_offset;
        if (t_bit >= 0) {
            if (t_bytes.size() != 1)
                return sgrn::Result<void, std::string>::Error("bit access needs one byte");
            auto r = t_rt->writeAreaBit(tag.addr.area, abs_off, t_bit, (t_bytes[0] & 1u) != 0);
            if (r.hasError())
                return r;
            return {};
        }
        auto r = t_rt->writeAreaMemory(tag.addr.area, abs_off, t_bytes.size(), t_bytes.data());
        if (r.hasError())
            return r;
        return {};
    };

    TagAccess access;
    access.list = [t_rt]() {
        std::vector<TagDescriptor> out;
        for (const auto& name : t_rt->tagNames()) {
            auto d = t_rt->describeTag(name);
            if (d.hasError())
                continue;
            const auto& tag = d.value();
            TagDescriptor desc;
            desc.name = tag.name;
            desc.table = tag.table;
            desc.type_str = tag.type_str;
            desc.udt_name = tag.udt_name;
            desc.type = tag.type;
            desc.span_bytes = tag.span_bytes;
            desc.bit_index = tag.addr.bit_index;
            out.push_back(std::move(desc));
        }
        return out;
    };
    access.backing = std::move(backing);
    tp_adapter->setTagAccess(std::move(access));
}

} // namespace sgrn::s7shell::bindings
