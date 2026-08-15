#pragma once

#include <sgrn/structures/ArenaTree.hpp> // FIX-1
#include <sgrn/structures/ReflectedPoint.hpp>
#include <sgrn/types/UniversalType.hpp>
#include <sgrn/utils/endianess.hpp>
#include <sgrn/utils/time.hpp>
#include <cstdint>
#include <json/json.h>
#include <memory>
#include <optional>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace sgrn
{

/**
 * @brief Represents a symbolic industrial variable with its physical properties.
 */
struct Symbol {
    std::string name_;
    UniversalType type_{UniversalType::Unknown};
    size_t size_{0};
    uint16_t id_{0};
    Json::Value context_; // Protocol-specific info
    std::vector<Symbol> children_;

    // Cache to avoid runtime path lookups
    mutable LeafDescriptor* cached_node_{nullptr}; // FIX-3: Use LeafDescriptor*

    // Segmented Caching (for top-level symbols/DBs)
    mutable std::string cached_json_;
    mutable int64_t last_json_ts_{0};
    mutable std::shared_mutex cache_mutex_; // FIX-5: Plain embedded mutex

    Symbol() = default; // FIX-5: No make_unique needed

    // Custom move/copy to handle the shared_mutex
    Symbol(const Symbol& t_other)
        : name_(t_other.name_)
        , type_(t_other.type_)
        , size_(t_other.size_)
        , id_(t_other.id_)
        , context_(t_other.context_)
        , children_(t_other.children_)
        , cached_node_(t_other.cached_node_)
        , cached_json_(t_other.cached_json_)
        , last_json_ts_(t_other.last_json_ts_) {
        // FIX-5: Mutex is default-initialized
    }

    Symbol(Symbol&& t_other) noexcept
        : name_(std::move(t_other.name_))
        , type_(t_other.type_)
        , size_(t_other.size_)
        , id_(t_other.id_)
        , context_(std::move(t_other.context_))
        , children_(std::move(t_other.children_))
        , cached_node_(t_other.cached_node_)
        , cached_json_(std::move(t_other.cached_json_))
        , last_json_ts_(t_other.last_json_ts_) {
        // FIX-5: Mutex is default-initialized
    }

    Symbol& operator=(Symbol&& t_other) noexcept {
        if (this != &t_other) {
            name_ = std::move(t_other.name_);
            type_ = t_other.type_;
            size_ = t_other.size_;
            id_ = t_other.id_;
            context_ = std::move(t_other.context_);
            children_ = std::move(t_other.children_);
            cached_node_ = t_other.cached_node_;
            cached_json_ = std::move(t_other.cached_json_);
            last_json_ts_ = t_other.last_json_ts_;
            // FIX-5: Don't move the mutex
        }
        return *this;
    }

    Symbol& operator=(const Symbol& t_other) {
        if (this != &t_other) {
            name_ = t_other.name_;
            type_ = t_other.type_;
            size_ = t_other.size_;
            id_ = t_other.id_;
            context_ = t_other.context_;
            children_ = t_other.children_;
            cached_node_ = t_other.cached_node_;
            cached_json_ = t_other.cached_json_;
            last_json_ts_ = t_other.last_json_ts_;
            // FIX-5: Don't copy the mutex
        }
        return *this;
    }

    Json::Value toJson() const {
        Json::Value t_node;
        t_node["name"] = name_;
        t_node["type"] = std::string(typeToString(type_));
        t_node["size"] = static_cast<Json::UInt64>(size_);
        t_node["id"] = id_;
        t_node["context"] = context_;
        if (!children_.empty()) {
            Json::Value child_array(Json::arrayValue);
            for (const auto& child : children_)
                child_array.append(child.toJson());
            t_node["children"] = child_array;
        }
        return t_node;
    }

    static Symbol fromJson(const Json::Value& t_node) {
        Symbol t_s;
        t_s.name_ = t_node.get("name", "").asString();
        t_s.type_ = stringToType(t_node.get("type", "Unkown").asString());
        t_s.size_ = t_node.get("size", 0).asLargestUInt();
        t_s.id_ = static_cast<uint16_t>(t_node.get("id", 0).asUInt());
        t_s.context_ = t_node.get("context", Json::Value(Json::objectValue));
        if (t_node.isMember("children") && t_node["children"].isArray()) {
            for (const auto& childNode : t_node["children"]) {
                t_s.children_.push_back(fromJson(childNode));
            }
        }
        return t_s;
    }

    /**
     * @brief Checks if this symbol's memory range overlaps with any dirty ranges in the tree.
     */
    bool isDirty() const { // FIX-1: tree parameter was unused
        if (!cached_node_)
            return false;
        std::lock_guard<std::mutex> lk(cached_node_->dirty_mutex);
        return !cached_node_->dirty_ranges.empty();
    }

    /**
     * @brief Recursive serialization using RapidJSON Writer.
     */
    template <typename Writer>
    void serialize(
        Writer& t_writer, const ArenaTree& t_tree, const std::vector<std::pair<size_t, size_t>>& t_ranges = {}, int t_depth = 0) const {
        if (children_.empty()) {
            if (!cached_node_) {
                t_writer.Null();
                return;
            }

            const uint8_t* p_ptr = t_tree.data() + cached_node_->offset;
            size_t val_size = cached_node_->size;

            switch (type_) {
                case UniversalType::Bool:
                    t_writer.Bool(p_ptr[0] != 0);
                    break;
                case UniversalType::Int:
                    if (val_size == 1)
                        t_writer.Int(static_cast<int8_t>(p_ptr[0]));
                    else if (val_size == 2)
                        t_writer.Int(sgrn::utils::fromBigEndian<int16_t>(p_ptr));
                    else if (val_size == 4)
                        t_writer.Int(sgrn::utils::fromBigEndian<int32_t>(p_ptr));
                    else
                        t_writer.Int64(sgrn::utils::fromBigEndian<int64_t>(p_ptr));
                    break;
                case UniversalType::UInt:
                    if (val_size == 1)
                        t_writer.Uint(p_ptr[0]);
                    else if (val_size == 2)
                        t_writer.Uint(sgrn::utils::fromBigEndian<uint16_t>(p_ptr));
                    else if (val_size == 4)
                        t_writer.Uint(sgrn::utils::fromBigEndian<uint32_t>(p_ptr));
                    else
                        t_writer.Uint64(sgrn::utils::fromBigEndian<uint64_t>(p_ptr));
                    break;
                case UniversalType::Float:
                    if (val_size == 4)
                        t_writer.Double(static_cast<double>(sgrn::utils::fromBigEndian<float>(p_ptr)));
                    else
                        t_writer.Double(sgrn::utils::fromBigEndian<double>(p_ptr));
                    break;
                case UniversalType::String:
                    t_writer.String(reinterpret_cast<const char*>(p_ptr), static_cast<rapidjson::SizeType>(val_size));
                    break;
                case UniversalType::DateTime: {
                    int64_t ms = sgrn::utils::fromBigEndian<int64_t>(p_ptr);
                    std::string ts = sgrn::utils::time::iso8601Timestamp(ms);
                    t_writer.String(ts.c_str(), static_cast<rapidjson::SizeType>(ts.length()));
                } break;
                default:
                    t_writer.Null();
                    break;
            }
        } else {
            t_writer.StartObject();
            for (const auto& child : children_) {
                if (!t_ranges.empty()) {
                    bool overlaps = false;

                    if (child.cached_node_) {
                        size_t start = child.cached_node_->offset;
                        size_t end = start + child.cached_node_->size;
                        for (const auto& r : t_ranges) {
                            if (start < r.second && end > r.first) {
                                overlaps = true;
                                break;
                            }
                        }
                    } else {
                        overlaps = true;
                    }

                    if (!overlaps)
                        continue;
                }

                t_writer.Key(child.name_.c_str(), static_cast<rapidjson::SizeType>(child.name_.length()));
                child.serialize(t_writer, t_tree, t_ranges, t_depth + 1);
            }
            t_writer.EndObject();
        }
    }
};

/**
 * @brief Unified Digital Twin Registry.
 */
class Registry {
public:
    void add(Symbol t_s, const std::string& t_parent_path = "", std::optional<size_t> t_fixed_offset = std::nullopt) {
        std::string full_path = t_parent_path.empty() ? t_s.name_ : t_parent_path + "." + t_s.name_;

        if (t_fixed_offset.has_value()) {
            tree_.registerView(t_s.id_, full_path, t_s.size_, *t_fixed_offset);
            t_s.cached_node_ = tree_.getNode(full_path);
        } else if (t_s.children_.empty() || t_s.size_ > 0) {
            tree_.registerLeaf(t_s.id_, full_path, t_s.size_);
            t_s.cached_node_ = tree_.getNode(full_path);
        }

        bool is_top_level = t_parent_path.empty();

        for (auto& child : t_s.children_) {
            std::optional<size_t> child_abs_offset = std::nullopt;
            if (t_s.cached_node_) {
                auto child_rel_off = child.context_["offset"];
                if (!child_rel_off.isNull()) {
                    child_abs_offset = t_s.cached_node_->offset + child_rel_off.asUInt();
                }
            }

            add(child, full_path, child_abs_offset);
            child = symbols_[full_path + "." + child.name_];
        }

        auto& stored = symbols_[full_path];
        stored = std::move(t_s);

        if (is_top_level) {
            top_level_names_.push_back(full_path);
        }
    }

    std::optional<ReflectedPoint> getPoint(const std::string& t_path) {
        auto it = symbols_.find(t_path);
        if (it == symbols_.end())
            return std::nullopt;

        try {
            return ReflectedPoint(tree_, t_path);
        } catch (...) {
            return std::nullopt;
        }
    }

    Json::Value getJson(const std::string& t_path) const {
        const auto* p_sym = find(t_path);
        if (!p_sym)
            return Json::Value::null;

        if (p_sym->children_.empty()) {
            auto point = const_cast<Registry*>(this)->getPoint(t_path);
            return point ? point->getJson(p_sym->type_) : Json::Value::null;
        }

        Json::Value t_root(Json::objectValue);
        for (const auto& child : p_sym->children_) {
            t_root[child.name_] = getJson(t_path + "." + child.name_);
        }
        return t_root;
    }

    void setCacheEnabled(bool t_enabled) {
        cache_enabled_ = t_enabled;
    }
    bool isCacheEnabled() const {
        return cache_enabled_;
    }

    const std::vector<std::string>& getTopLevelNames() const {
        return top_level_names_;
    }
    ArenaTree& tree() { // FIX-1
        return tree_;
    }
    const ArenaTree& tree() const { // FIX-1
        return tree_;
    }

    const Symbol* find(const std::string& t_name) const {
        auto it = symbols_.find(t_name);
        if (it != symbols_.end())
            return &it->second;
        return nullptr;
    }

    std::vector<std::string> names() const {
        std::vector<std::string> result;
        result.reserve(symbols_.size());
        for (const auto& [name_, _] : symbols_)
            result.push_back(name_);
        return result;
    }

    Json::Value toJson() const {
        Json::Value t_root(Json::arrayValue);
        for (const auto& name_ : top_level_names_) {
            t_root.append(symbols_.at(name_).toJson());
        }
        return t_root;
    }

    void loadFromJson(const Json::Value& t_root) {
        if (!t_root.isArray())
            return;
        for (const auto& t_node : t_root)
            add(Symbol::fromJson(t_node));
    }

    void clear() {
        symbols_.clear();
        top_level_names_.clear();
        tree_.clear();
    }

    std::string getFullSnapshot() const {
        auto lk = const_cast<ArenaTree&>(tree_).lockShared(top_level_names_); // FIX-1

        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);
        t_writer.StartObject();
        for (const auto& name_ : top_level_names_) {
            const auto& p_sym = symbols_.at(name_);
            t_writer.Key(p_sym.name_.c_str(), static_cast<rapidjson::SizeType>(p_sym.name_.length()));
            p_sym.serialize(t_writer, tree_);
        }
        t_writer.EndObject();
        return sb.GetString();
    }

    std::string getDeltaSnapshot(const std::vector<std::string>& t_filter = {}) {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);
        t_writer.StartObject();

        auto dirty_nodes = tree_.getDirtyNodes();
        if (dirty_nodes.empty()) {
            t_writer.EndObject();
            return sb.GetString();
        }

        std::vector<std::string> target_paths;
        for (auto* t_node : dirty_nodes) {
            if (!t_filter.empty()) {
                // node->node->name is the leaf name, but we need the path.
                // ReflectedPoint handles path reconstruction, but here we have the LeafDescriptor.
                // We'll use a helper to get the path.
                std::string t_path = reconstructPath(t_node->node);
                if (std::find(t_filter.begin(), t_filter.end(), t_path) == t_filter.end())
                    continue;
                target_paths.push_back(t_path);
            } else {
                target_paths.push_back(reconstructPath(t_node->node));
            }
        }

        if (target_paths.empty()) {
            t_writer.EndObject();
            return sb.GetString();
        }

        auto lk = tree_.lockShared(target_paths);

        for (const auto& t_path : target_paths) {
            auto it = symbols_.find(t_path);
            if (it == symbols_.end())
                continue;

            const auto& p_sym = it->second;
            auto t_ranges = tree_.getNode(t_path)->getAndClearDirty();

            t_writer.Key(p_sym.name_.c_str(), static_cast<rapidjson::SizeType>(p_sym.name_.length()));
            p_sym.serialize(t_writer, tree_, t_ranges);
        }

        t_writer.EndObject();
        return sb.GetString();
    }

protected:
    static std::string reconstructPath(TreeNode* tp_n) {
        if (!tp_n || !tp_n->parent)
            return "";
        std::string p = tp_n->name;
        TreeNode* p_curr = tp_n->parent;
        while (p_curr && p_curr->parent) {
            p = p_curr->name + "." + p;
            p_curr = p_curr->parent;
        }
        return p;
    }

    ArenaTree tree_;
    std::unordered_map<std::string, Symbol> symbols_;
    std::vector<std::string> top_level_names_;
    bool cache_enabled_{true};
};

} // namespace sgrn
