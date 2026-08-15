#pragma once

#include <sgrn/structures/ArenaTree.hpp> // FIX-1
#include <sgrn/types/UniversalType.hpp>
#include <sgrn/utils/endianess.hpp>
#include <sgrn/utils/time.hpp>
#include <json/json.h>
#include <memory>
#include <stdexcept>
#include <string>

namespace sgrn
{

/**
 * @brief Binds a semantic Symbol to a physical memory Node in the ArenaTree.
 */
class ReflectedPoint {
public:
    ReflectedPoint() = delete;

    ReflectedPoint(ArenaTree& t_tree, const std::string& t_path)
        : tree_(&t_tree)
        , leaf_(t_tree.getNode(t_path))
        , full_path_(t_path) { // FIX-6: Store full path
    }

    /**
     * @brief Construct a point by numeric ID.
     */
    ReflectedPoint(ArenaTree& t_tree, uint16_t t_id)
        : tree_(&t_tree)
        , leaf_(t_tree.getNode(t_id)) {
        // FIX-6: Reconstruct full path
        if (leaf_ && leaf_->node) {
            full_path_ = reconstructPath(leaf_->node);
        }
    }

    bool isValid() const {
        return tree_ != nullptr && leaf_ != nullptr;
    }

    explicit operator bool() const {
        return isValid();
    }

    template <typename T>
    std::optional<T> get() const {
        if (!isValid())
            return std::nullopt;
        std::shared_lock<std::shared_mutex> lk(leaf_->node->mutex); // FIX-3: Use TreeNode mutex
        if (leaf_->size < sizeof(T)) {
            return std::nullopt;
        }
        const uint8_t* p_base = tree_->data();
        return sgrn::utils::fromBigEndian<T>(p_base + leaf_->offset);
    }

    template <typename T>
    std::optional<T> getLE() const {
        if (!isValid())
            return std::nullopt;
        std::shared_lock<std::shared_mutex> lk(leaf_->node->mutex); // FIX-3
        if (leaf_->size < sizeof(T)) {
            return std::nullopt;
        }
        const uint8_t* p_base = tree_->data();
        return sgrn::utils::fromLittleEndian<T>(p_base + leaf_->offset);
    }

    template <typename T>
    bool set(T t_value) {
        return setBE<T>(t_value);
    }

    template <typename T>
    bool setBE(T t_value) {
        if (!isValid())
            return false;
        {
            std::unique_lock<std::shared_mutex> lk(leaf_->node->mutex); // FIX-3
            if (leaf_->size < sizeof(T)) {
                return false;
            }

            uint8_t* p_base = const_cast<uint8_t*>(tree_->data());
            sgrn::utils::toBigEndian<T>(t_value, p_base + leaf_->offset);

            leaf_->last_update_ts.store(sgrn::utils::time::nowMilliseconds(), std::memory_order_relaxed);
        }

        leaf_->addDirtyRange(leaf_->offset, sizeof(T)); // FIX-4: Use absolute offset
        return true;
    }

    template <typename T>
    bool setLE(T t_value) {
        if (!isValid())
            return false;
        {
            std::unique_lock<std::shared_mutex> lk(leaf_->node->mutex); // FIX-3
            if (leaf_->size < sizeof(T)) {
                return false;
            }

            uint8_t* p_base = const_cast<uint8_t*>(tree_->data());
            sgrn::utils::toLittleEndian<T>(t_value, p_base + leaf_->offset);

            leaf_->last_update_ts.store(sgrn::utils::time::nowMilliseconds(), std::memory_order_relaxed);
        }

        leaf_->addDirtyRange(leaf_->offset, sizeof(T)); // FIX-4: Use absolute offset
        return true;
    }

    std::optional<bool> getBool() const {
        if (!isValid())
            return std::nullopt;
        std::shared_lock<std::shared_mutex> lk(leaf_->node->mutex); // FIX-3
        if (leaf_->size < 1)
            return std::nullopt;
        return tree_->data()[leaf_->offset] != 0;
    }

    bool setBool(bool t_value) {
        if (!isValid())
            return false;
        {
            std::unique_lock<std::shared_mutex> lk(leaf_->node->mutex); // FIX-3
            if (leaf_->size < 1)
                return false;
            uint8_t* p_base = const_cast<uint8_t*>(tree_->data());
            p_base[leaf_->offset] = t_value ? 1 : 0;
            leaf_->last_update_ts.store(sgrn::utils::time::nowMilliseconds(), std::memory_order_relaxed);
        }
        leaf_->addDirtyRange(leaf_->offset, 1); // FIX-4: Use absolute offset
        return true;
    }

    Json::Value getJson(UniversalType t_type) const {
        if (!isValid())
            return Json::Value::null;
        std::shared_lock<std::shared_mutex> lk(leaf_->node->mutex); // FIX-3
        if (leaf_->size == 0)
            return Json::Value::null;

        const uint8_t* p_ptr = tree_->data() + leaf_->offset;
        switch (t_type) {
            case UniversalType::Bool:
                return Json::Value(p_ptr[0] != 0);
            case UniversalType::Int:
                return Json::Value(static_cast<Json::Int64>(sgrn::utils::fromBigEndian<int32_t>(p_ptr)));
            case UniversalType::UInt:
                return Json::Value(static_cast<Json::UInt64>(sgrn::utils::fromBigEndian<uint32_t>(p_ptr)));
            case UniversalType::Float:
                return Json::Value(static_cast<double>(sgrn::utils::fromBigEndian<float>(p_ptr)));
            case UniversalType::String:
                return Json::Value(std::string(reinterpret_cast<const char*>(p_ptr), leaf_->size));
            default:
                return Json::Value::null;
        }
    }

    bool setJson(UniversalType t_type, const Json::Value& t_value) {
        if (!isValid())
            return false;
        {
            std::unique_lock<std::shared_mutex> lk(leaf_->node->mutex); // FIX-3
            uint8_t* p_ptr = const_cast<uint8_t*>(tree_->data()) + leaf_->offset;

            switch (t_type) {
                case UniversalType::Bool:
                    if (t_value.isBool())
                        p_ptr[0] = t_value.asBool() ? 1 : 0;
                    break;
                case UniversalType::Int:
                    if (t_value.isInt64())
                        sgrn::utils::toBigEndian<int32_t>(static_cast<int32_t>(t_value.asInt64()), p_ptr);
                    break;
                case UniversalType::UInt:
                    if (t_value.isUInt64())
                        sgrn::utils::toBigEndian<uint32_t>(static_cast<uint32_t>(t_value.asUInt64()), p_ptr);
                    break;
                case UniversalType::Float:
                    if (t_value.isNumeric())
                        sgrn::utils::toBigEndian<float>(static_cast<float>(t_value.asDouble()), p_ptr);
                    break;
                case UniversalType::String:
                    if (t_value.isString()) {
                        std::string s = t_value.asString();
                        std::memcpy(p_ptr, s.c_str(), std::min(s.length(), leaf_->size));
                    }
                    break;
                default:
                    return false;
            }
            leaf_->last_update_ts.store(sgrn::utils::time::nowMilliseconds(), std::memory_order_relaxed);
        }
        leaf_->addDirtyRange(leaf_->offset, leaf_->size); // FIX-4: Use absolute offset
        return true;
    }

    const LeafDescriptor* node_() const { // FIX-3: returns LeafDescriptor*
        return leaf_;
    }
    const std::string& t_path() const {
        return full_path_; // FIX-6: Return full path
    }
    uint16_t id_() const {
        return leaf_->id;
    }

private:
    static std::string reconstructPath(TreeNode* tp_n) { // FIX-6
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

    ArenaTree* tree_{nullptr};
    LeafDescriptor* leaf_{nullptr}; // FIX-3: Renamed from node_
    std::string full_path_;         // FIX-6
};

} // namespace sgrn
