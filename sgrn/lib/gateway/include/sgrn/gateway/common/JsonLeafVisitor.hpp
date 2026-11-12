#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace sgrn::gateway::common
{

/// Controls whether JSON arrays are emitted as leaves or ignored. HTTP writes
/// historically treat an array as one serialized value; nested RuntimeSync
/// deltas intentionally skip arrays because the runtime write path is scalar.
enum class JsonArrayPolicy { TreatAsLeaf, Skip };

/// Visit scalar/terminal JSON nodes without building an intermediate vector.
/// Supports Glaze JSON DOM Value and Glaze generic JSON nodes through their native
/// object-iteration APIs. The callback consumes the mutable path synchronously.
template <class JsonNode, class Visitor>
void visitJsonLeaves(const JsonNode& t_node, std::string& t_path, char t_separator, JsonArrayPolicy t_array_policy, Visitor&& t_visitor) {
    // Prefer the DOM compatibility view when both APIs are available. Its
    // MemberIterator yields dom::Value child views, which keeps callbacks that
    // consume the gateway DOM type well-formed. Raw Glaze generic nodes expose
    // only the lowercase API and are handled by the second branch.
    if constexpr (requires {
                      t_node.is_object();
                      t_node.memberBegin();
                      t_node.is_array();
                  }) {
        if (t_node.is_object()) {
            for (auto it = t_node.memberBegin(); it != t_node.memberEnd(); ++it) {
                const size_t previous_size = t_path.size();
                if (!t_path.empty())
                    t_path.push_back(t_separator);
                t_path.append(it->name.getString(), it->name.getStringLength());
                visitJsonLeaves(it->value, t_path, t_separator, t_array_policy, t_visitor);
                t_path.resize(previous_size);
            }
            return;
        }
        if (t_array_policy == JsonArrayPolicy::Skip && t_node.is_array())
            return;
    } else if constexpr (requires {
                             t_node.is_object();
                             t_node.get_object();
                         }) {
        if (t_node.is_object()) {
            for (const auto& [key, value] : t_node.get_object()) {
                const size_t previous_size = t_path.size();
                if (!t_path.empty())
                    t_path.push_back(t_separator);
                t_path.append(std::string_view{key.data(), key.size()});
                visitJsonLeaves(value, t_path, t_separator, t_array_policy, t_visitor);
                t_path.resize(previous_size);
            }
            return;
        }
        if (t_array_policy == JsonArrayPolicy::Skip && t_node.is_array())
            return;
    } else {
        static_assert(sizeof(JsonNode) == 0, "visitJsonLeaves requires a Glaze JSON DOM or Glaze JSON node");
    }

    std::invoke(t_visitor, t_path, t_node);
}

} // namespace sgrn::gateway::common
