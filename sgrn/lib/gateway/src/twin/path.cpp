#include <sgrn/gateway/twin/path.hpp>
#include <sgrn/utils/endianess.hpp>

namespace sgrn::gateway::twin
{
int fieldSpanSize(const DbField& t_field) {
    return static_cast<int>(::sgrn::scl::fieldSpanSize(t_field));
}

std::optional<LocatedField> findFieldByPath(const std::vector<DbField>& t_fields, const std::string& t_path, int t_base_offset) {
    // scl's version is the superset: same "./" separators plus `name[i]`
    // array-index resolution on the path component.
    return ::sgrn::scl::findFieldByPath(t_fields, t_path, t_base_offset);
}

DbField plcNodeToDbField(const sgrn::gateway::twin::PlcNode& t_node) {
    DbField t_f;
    t_f.name = t_node.name_;
    t_f.type = t_node.type_;
    t_f.count = static_cast<int>(t_node.count_);
    t_f.bit_index = static_cast<int>(t_node.bit_index_);
    t_f.endianness = t_node.endian_;
    // offset=0 because callers pass ptr already positioned at this node;
    // children carry their own relative offsets as stored in PlcNode.
    t_f.offset = 0;
    if (t_node.type_ == s7codec::Type::String || t_node.type_ == s7codec::Type::WString || t_node.type_ == s7codec::Type::XString ||
        t_node.type_ == s7codec::Type::XWString) {
        t_f.string_capacity = static_cast<int>(t_node.string_capacity_);
    }
    if (t_node.type_ == s7codec::Type::Struct || !t_node.children_.empty()) {
        // struct_size is the PER-ELEMENT byte span (DbField convention, and
        // what PlcNode::serialize assumes for its stride). PlcNode::size_ is
        // already per-element — do NOT divide by count (that yields a bogus
        // stride, e.g. 28/3=9 for a 3x28 UDT array, failing every struct-array
        // encode at the first child past the truncated span).
        t_f.struct_size = static_cast<int>(t_node.size_);
        for (const auto& child : t_node.children_) {
            DbField cf = plcNodeToDbField(child);
            cf.offset = static_cast<int>(child.offset_);
            t_f.children.push_back(std::move(cf));
        }
    }
    return t_f;
}
} // namespace sgrn::gateway::twin
