#pragma once

#include <sgrn/scl/types.hpp>

#include <functional>
#include <string>
#include <vector>

namespace sgrn::gateway::twin
{

using ::sgrn::scl::DbField;
using ::sgrn::scl::DbSchema;

struct DbFieldVisitInfo {
    const DbField* field{nullptr};
    std::string path;
    int absolute_offset{0};
    bool is_leaf{false};
};

template <typename Fn>
void visitDbFields(const std::vector<DbField>& t_fields, Fn&& t_fn, std::string t_prefix = "", int t_base_offset = 0) {
    for (const auto& field : t_fields) {
        const std::string path = t_prefix.empty() ? field.name : t_prefix + "." + field.name;
        const int absolute_offset = t_base_offset + field.offset;
        const bool is_leaf = field.children.empty();
        t_fn(DbFieldVisitInfo{&field, path, absolute_offset, is_leaf});
        if (!field.children.empty()) {
            visitDbFields(field.children, t_fn, path, absolute_offset);
        }
    }
}

inline std::vector<std::string> collectDbLeafPaths(const DbSchema& t_registry) {
    std::vector<std::string> paths;
    visitDbFields(t_registry.fields, [&](const DbFieldVisitInfo& t_info) {
        if (t_info.is_leaf) {
            paths.push_back(t_info.path);
        }
    });
    return paths;
}

} // namespace sgrn::gateway::twin
