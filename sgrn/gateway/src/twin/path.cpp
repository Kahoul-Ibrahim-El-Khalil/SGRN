#include <sgrn/gateway/twin/path.hpp>
#include <sgrn/utils/endianess.hpp>
#include <algorithm>
#include <string_view>

using ::sgrn::scl::DataBlockRegistry;
using ::sgrn::scl::DataType;

namespace sgrn::gateway::twin
{
int fieldSpanSize(const DbField& t_field) {
    if (t_field.type == DataType::Struct)
        return std::max(1, t_field.struct_size) * std::max(1, t_field.count);
    int base = s7codec::typeSpanBytes(t_field.type, t_field.count);
    return t_field.is_dynamic ? base + 4 : base;
}

int symbolFieldSpanBytes(const DbField& t_field) {
    auto prim = [&](DataType t_t) -> int {
        if (t_t == DataType::String)
            return 2 + std::max(0, t_field.count);
        if (t_t == DataType::WString)
            return 4 + (std::max(0, t_field.count) * 2);
        if (t_t == DataType::XString)
            return 8 + std::max(0, t_field.count);
        if (t_t == DataType::XWString)
            return 8 + (std::max(0, t_field.count) * 2);
        if (t_t == DataType::Struct)
            return 0;
        return s7codec::primitiveSize(t_t);
    };
    int base = 0;
    if (t_field.type == DataType::Struct)
        base = std::max(1, t_field.struct_size) * std::max(1, t_field.count);
    else if (t_field.count > 1 && t_field.type != DataType::String && t_field.type != DataType::WString &&
             t_field.type != DataType::XString && t_field.type != DataType::XWString)
        base = prim(t_field.type) * t_field.count;
    else
        base = prim(t_field.type);

    return t_field.is_dynamic ? base + 4 : base;
}

const DbField* findFieldByName(const DataBlockRegistry& t_reg, const std::string& t_name) {
    std::vector<DbField>::const_iterator it =
        std::find_if(t_reg.fields.begin(), t_reg.fields.end(), [&](const DbField& t_f) { return t_f.name == t_name; });
    return it == t_reg.fields.end() ? nullptr : &*it;
}

std::optional<LocatedField> findFieldByPath(const std::vector<DbField>& t_fields, const std::string& t_path, int t_base_offset) {
    // Use string_view to avoid substr allocations at each recursion level
    std::string_view sv(t_path);
    return [&](const std::vector<DbField>& t_flds, std::string_view t_p, int t_off) -> std::optional<LocatedField> {
        size_t sep = t_p.find_first_of("./");
        std::string_view current_name = (sep == std::string_view::npos) ? t_p : t_p.substr(0, sep);
        std::string_view remaining = (sep == std::string_view::npos) ? std::string_view{} : t_p.substr(sep + 1);

        auto it = std::find_if(t_flds.begin(), t_flds.end(), [&](const auto& t_f) { return t_f.name == current_name; });
        if (it == t_flds.end())
            return std::nullopt;

        int abs_offset = t_off + it->offset;
        if (remaining.empty())
            return LocatedField{&*it, abs_offset};

        // Recurse with remaining path — call ourselves via the lambda
        auto self = [&](const std::vector<DbField>& t_inner_flds, std::string_view t_inner_p, int t_inner_off,
                        auto& t_self_ref) -> std::optional<LocatedField> {
            size_t s = t_inner_p.find_first_of("./");
            std::string_view cur = (s == std::string_view::npos) ? t_inner_p : t_inner_p.substr(0, s);
            std::string_view rem = (s == std::string_view::npos) ? std::string_view{} : t_inner_p.substr(s + 1);

            auto inner_it = std::find_if(t_inner_flds.begin(), t_inner_flds.end(), [&](const auto& t_f) { return t_f.name == cur; });
            if (inner_it == t_inner_flds.end())
                return std::nullopt;

            int inner_abs = t_inner_off + inner_it->offset;
            if (rem.empty())
                return LocatedField{&*inner_it, inner_abs};
            return t_self_ref(inner_it->children, rem, inner_abs, t_self_ref);
        };
        return self(it->children, remaining, abs_offset, self);
    }(t_fields, sv, t_base_offset);
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
    if (t_node.type_ == s7codec::Type::Struct || !t_node.children_.empty()) {
        t_f.struct_size = static_cast<int>((t_node.count_ > 1) ? (t_node.size_ / t_node.count_) : t_node.size_);
        for (const auto& child : t_node.children_) {
            DbField cf = plcNodeToDbField(child);
            cf.offset = static_cast<int>(child.offset_);
            t_f.children.push_back(std::move(cf));
        }
    }
    return t_f;
}
} // namespace sgrn::gateway::twin
