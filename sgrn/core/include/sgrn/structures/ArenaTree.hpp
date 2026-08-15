/*sgrn/core/ArenaTree*/
#pragma once

#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace sgrn
{

struct TreeNode;

/**
 * @brief FIX 3 · Holds leaf-specific metadata, dirty tracking, and timestamps.
 */
struct LeafDescriptor {
    uint16_t id = 0;
    size_t offset = 0;
    size_t size = 0;
    mutable std::atomic<int64_t> last_update_ts{0}; // FIX-3
    mutable std::mutex dirty_mutex;
    std::vector<std::pair<size_t, size_t>> dirty_ranges;
    TreeNode* node = nullptr; // FIX-3: Back-pointer for hierarchical locking

    void markDirty(size_t t_start, size_t t_len) {
        std::lock_guard<std::mutex> lk(dirty_mutex);
        dirty_ranges.push_back({t_start, t_start + t_len});
    }

    void addDirtyRange(size_t t_start, size_t t_len) {
        markDirty(t_start, t_len);
    }

    std::vector<std::pair<size_t, size_t>> getAndClearDirty() {
        std::lock_guard<std::mutex> lk(dirty_mutex);
        auto out = std::move(dirty_ranges);
        dirty_ranges.clear();
        return out;
    }
};

struct TreeNode {
    std::string name;
    mutable std::shared_mutex mutex; // FIX-3: for hierarchical locking only
    TreeNode* parent = nullptr;
    std::vector<std::unique_ptr<TreeNode>> children;
    LeafDescriptor* leaf = nullptr; // FIX-3: Link to data if this is a leaf

    TreeNode(std::string t_n, TreeNode* tp_p = nullptr)
        : name(std::move(t_n))
        , parent(tp_p) {
    }
};

/**
 * @brief THE SINGLE SOURCE OF TRUTH for the Digital Twin state.
 * Uses a single flat arena for all segments to ensure cache locality and O(1) physical access.
 */
class ArenaTree {
public:
    struct Segment {
        std::string name;
        uint32_t id{0};
        size_t offset{0};
        size_t size{0};
        mutable std::shared_mutex mutex_;
        mutable std::atomic<int64_t> last_write_ms{0};
        mutable std::atomic<bool> is_dirty_{false};
        void markDirty() {
            is_dirty_.store(true, std::memory_order_release);
        }

        bool getAndClearDirty() {
            return is_dirty_.exchange(false, std::memory_order_acq_rel);
        }
    };

    enum class LockMode { Shared, Exclusive };

    class SegmentLock {
    public:
        SegmentLock() = default;
        SegmentLock(std::vector<Segment*> t_segments, LockMode t_mode)
            : segments_(std::move(t_segments))
            , mode_(t_mode) {
            for (auto* s : segments_) {
                if (mode_ == LockMode::Shared)
                    s->mutex_.lock_shared();
                else
                    s->mutex_.lock();
            }
        }
        ~SegmentLock() {
            unlock();
        }

        SegmentLock(SegmentLock&& t_other) noexcept
            : segments_(std::move(t_other.segments_))
            , mode_(t_other.mode_) {
            t_other.segments_.clear();
        }
        SegmentLock& operator=(SegmentLock&& t_other) noexcept {
            if (this != &t_other) {
                unlock();
                segments_ = std::move(t_other.segments_);
                mode_ = t_other.mode_;
                t_other.segments_.clear();
            }
            return *this;
        }

        void unlock() {
            for (int i = static_cast<int>(segments_.size()) - 1; i >= 0; --i) {
                if (mode_ == LockMode::Shared)
                    segments_[i]->mutex_.unlock_shared();
                else
                    segments_[i]->mutex_.unlock();
            }
            segments_.clear();
        }

    private:
        std::vector<Segment*> segments_;
        LockMode mode_{LockMode::Shared};
    };

    SegmentLock lockSegment(const std::string& t_name, LockMode t_mode) {
        auto it = segments_.find(t_name);
        if (it != segments_.end())
            return SegmentLock({it->second.get()}, t_mode);
        return {};
    }

    SegmentLock lockSegment(uint32_t t_id, LockMode t_mode) {
        auto it = segments_by_id_.find(t_id);
        if (it != segments_by_id_.end())
            return SegmentLock({it->second}, t_mode);
        return {};
    }

    SegmentLock lockSegments(std::vector<uint32_t> t_ids, LockMode t_mode) {
        std::sort(t_ids.begin(), t_ids.end());
        t_ids.erase(std::unique(t_ids.begin(), t_ids.end()), t_ids.end());
        std::vector<Segment*> targets;
        for (auto id : t_ids) {
            auto it = segments_by_id_.find(id);
            if (it != segments_by_id_.end())
                targets.push_back(it->second);
        }
        return SegmentLock(std::move(targets), t_mode);
    }

    /**
     * @brief RAII Guard for hierarchical locking.
     */
    class Guard {
    public:
        enum class Mode { Shared, Exclusive };
        Guard() = default;
        Guard(std::vector<std::vector<TreeNode*>> t_paths, Mode t_mode)
            : paths_(std::move(t_paths))
            , mode_(t_mode) {
        }
        ~Guard() {
            unlock();
        }

        Guard(Guard&& t_other) noexcept
            : paths_(std::move(t_other.paths_))
            , mode_(t_other.mode_) {
            t_other.paths_.clear();
        }
        Guard& operator=(Guard&& t_other) noexcept {
            if (this != &t_other) {
                unlock();
                paths_ = std::move(t_other.paths_);
                mode_ = t_other.mode_;
                t_other.paths_.clear();
            }
            return *this;
        }

        void unlock() {
            for (int tp_p = static_cast<int>(paths_.size()) - 1; tp_p >= 0; --tp_p) {
                auto& nodes = paths_[tp_p];
                if (nodes.empty())
                    continue;
                if (mode_ == Mode::Exclusive)
                    nodes.back()->mutex.unlock();
                else
                    nodes.back()->mutex.unlock_shared();
                for (int i = static_cast<int>(nodes.size()) - 2; i >= 0; --i)
                    nodes[i]->mutex.unlock_shared();
            }
            paths_.clear();
        }

    private:
        std::vector<std::vector<TreeNode*>> paths_;
        Mode mode_{Mode::Shared};
    };

    explicit ArenaTree(size_t t_initial_capacity = 1024 * 1024)
        : root_(std::make_unique<TreeNode>("root")) {
        arena_.reserve(t_initial_capacity);
        registerSegment("default", 0, 0);
    }

    void reserve(size_t t_capacity) {
        arena_.reserve(t_capacity);
    }

    void registerSegment(const std::string& t_name, uint32_t t_id, size_t t_size, size_t t_alignment = 8) {
        auto it = segments_.find(t_name);
        if (it != segments_.end() && t_name == "default" && it->second->size == 0) {
            it->second->id = t_id;
            segments_by_id_.erase(0);
            segments_by_id_[t_id] = it->second.get();
        }

        size_t padding = (t_alignment - (arena_.size() % t_alignment)) % t_alignment;
        if (padding > 0)
            arena_.resize(arena_.size() + padding, 0);

        size_t t_offset = arena_.size();
        arena_.resize(t_offset + t_size, 0);

        if (it == segments_.end() || t_name != "default") {
            auto p_seg = std::make_unique<Segment>();
            p_seg->name = t_name;
            p_seg->id = t_id;
            p_seg->offset = t_offset;
            p_seg->size = t_size;
            segments_[t_name] = std::move(p_seg);
            segments_by_id_[t_id] = segments_[t_name].get();
        } else {
            it->second->offset = t_offset;
            it->second->size = t_size;
        }
    }

    size_t registerLeaf(uint16_t t_id, const std::string& t_path, size_t t_size, size_t t_alignment = 8) {
        if (segments_.empty())
            registerSegment("default", 0, 0);
        auto* p_seg = segments_by_id_.begin()->second;

        size_t padding = (t_alignment - (arena_.size() % t_alignment)) % t_alignment;
        if (padding > 0)
            arena_.resize(arena_.size() + padding, 0);
        size_t t_offset = arena_.size();
        arena_.resize(t_offset + t_size, 0);

        if (p_seg->name == "default")
            p_seg->size = arena_.size() - p_seg->offset;
        return registerView(t_id, t_path, t_size, t_offset);
    }

    size_t registerView(uint16_t t_id, const std::string& t_path, size_t t_size, size_t t_offset) {
        TreeNode* p_current = root_.get();
        for (const auto& part : splitPath(t_path)) {
            TreeNode* found = nullptr;
            for (const auto& child : p_current->children)
                if (child->name == part) {
                    found = child.get();
                    break;
                }
            if (!found) {
                auto next = std::make_unique<TreeNode>(part, p_current);
                found = next.get();
                p_current->children.push_back(std::move(next));
            }
            p_current = found;
        }

        auto leaf = std::make_unique<LeafDescriptor>();
        leaf->id = t_id;
        leaf->offset = t_offset;
        leaf->size = t_size;
        leaf->node = p_current;
        p_current->leaf = leaf.get();

        id_map_[t_id] = leaf.get();
        path_map_[t_path] = leaf.get();
        leaf_storage_.push_back(std::move(leaf));

        return t_offset;
    }

    size_t registerLeaf(uint32_t t_segment_id, const std::string& t_path, size_t t_rel_offset, size_t t_size) {
        auto it = segments_by_id_.find(t_segment_id);
        if (it == segments_by_id_.end())
            return 0;
        return registerView(static_cast<uint16_t>(t_segment_id), t_path, t_size, it->second->offset + t_rel_offset);
    }

    // Locking API
    Guard lockShared(const std::string& t_path) {
        return lock({t_path}, Guard::Mode::Shared);
    }
    Guard lockExclusive(const std::string& t_path) {
        return lock({t_path}, Guard::Mode::Exclusive);
    }
    Guard lockShared(std::vector<std::string> t_paths) {
        return lock(std::move(t_paths), Guard::Mode::Shared);
    }
    Guard lockExclusive(std::vector<std::string> t_paths) {
        return lock(std::move(t_paths), Guard::Mode::Exclusive);
    }

    uint8_t* data() {
        return arena_.data();
    }
    const uint8_t* data() const {
        return arena_.data();
    }
    size_t size() const {
        return arena_.size();
    }

    uint8_t* data(const std::string& t_segment_name) {
        auto it = segments_.find(t_segment_name);
        return it != segments_.end() ? arena_.data() + it->second->offset : nullptr;
    }
    const uint8_t* data(const std::string& t_segment_name) const {
        auto it = segments_.find(t_segment_name);
        return it != segments_.end() ? arena_.data() + it->second->offset : nullptr;
    }

    uint8_t* data(uint32_t t_segment_id) {
        auto it = segments_by_id_.find(t_segment_id);
        return it != segments_by_id_.end() ? arena_.data() + it->second->offset : nullptr;
    }
    const uint8_t* data(uint32_t t_segment_id) const {
        auto it = segments_by_id_.find(t_segment_id);
        return it != segments_by_id_.end() ? arena_.data() + it->second->offset : nullptr;
    }

    LeafDescriptor* getNode(uint16_t t_id) {
        auto it = id_map_.find(t_id);
        return it != id_map_.end() ? it->second : nullptr;
    }
    const LeafDescriptor* getNode(uint16_t t_id) const {
        auto it = id_map_.find(t_id);
        return it != id_map_.end() ? it->second : nullptr;
    }

    LeafDescriptor* getNode(const std::string& t_path) {
        auto it = path_map_.find(t_path);
        return it != path_map_.end() ? it->second : nullptr;
    }
    const LeafDescriptor* getNode(const std::string& t_path) const {
        auto it = path_map_.find(t_path);
        return it != path_map_.end() ? it->second : nullptr;
    }

    std::vector<LeafDescriptor*> getDirtyNodes() {
        std::vector<LeafDescriptor*> out;
        for (auto& leaf : leaf_storage_) {
            std::lock_guard<std::mutex> lk(leaf->dirty_mutex);
            if (!leaf->dirty_ranges.empty())
                out.push_back(leaf.get());
        }
        return out;
    }

    void clear() {
        arena_.clear();
        segments_.clear();
        segments_by_id_.clear();
        path_map_.clear();
        id_map_.clear();
        leaf_storage_.clear();
        root_ = std::make_unique<TreeNode>("root");
        registerSegment("default", 0, 0);
    }

    const std::map<std::string, std::unique_ptr<Segment>>& segments() const {
        return segments_;
    }
    const std::map<uint32_t, Segment*>& segments_by_id() const {
        return segments_by_id_;
    }

protected:
    std::vector<uint8_t> arena_;

private:
    std::map<std::string, std::unique_ptr<Segment>> segments_;
    std::map<uint32_t, Segment*> segments_by_id_;
    std::unique_ptr<TreeNode> root_;
    std::unordered_map<uint16_t, LeafDescriptor*> id_map_;
    std::unordered_map<std::string, LeafDescriptor*> path_map_;
    std::vector<std::unique_ptr<LeafDescriptor>> leaf_storage_;

    Guard lock(std::vector<std::string> t_paths, Guard::Mode t_mode) {
        std::sort(t_paths.begin(), t_paths.end());
        t_paths.erase(std::unique(t_paths.begin(), t_paths.end()), t_paths.end());

        // 1. Collect all nodes involved in the paths
        std::vector<TreeNode*> unique_nodes;
        std::vector<std::vector<TreeNode*>> all_paths;

        for (const auto& tp_p : t_paths) {
            std::vector<TreeNode*> nodes;
            TreeNode* p_curr = root_.get();
            nodes.push_back(p_curr);
            unique_nodes.push_back(p_curr);

            for (const auto& part : splitPath(tp_p)) {
                TreeNode* found = nullptr;
                for (const auto& c : p_curr->children) {
                    if (c->name == part) {
                        found = c.get();
                        break;
                    }
                }
                if (found) {
                    p_curr = found;
                    nodes.push_back(p_curr);
                    unique_nodes.push_back(p_curr);
                }
            }
            all_paths.push_back(std::move(nodes));
        }

        // 2. Deduplicate unique_nodes to prevent double-locking parents
        std::sort(unique_nodes.begin(), unique_nodes.end());
        unique_nodes.erase(std::unique(unique_nodes.begin(), unique_nodes.end()), unique_nodes.end());

        // 3. Lock each node exactly once.
        // We lock all intermediate and leaf nodes in the requested mode.
        // NOTE: In a strictly hierarchical lock, we'd lock parents in Shared mode,
        // but locking unique nodes in the requested mode is safer for multi-path atomic ops.
        for (auto* tp_n : unique_nodes) {
            if (t_mode == Guard::Mode::Exclusive)
                tp_n->mutex.lock();
            else
                tp_n->mutex.lock_shared();
        }

        return Guard(std::move(all_paths), t_mode);
    }

    static std::string reconstructPath(TreeNode* tp_n) { // FIX-6 helper
        if (!tp_n || !tp_n->parent)
            return "";
        std::string tp_p = tp_n->name;
        TreeNode* p_curr = tp_n->parent;
        while (p_curr && p_curr->parent) {
            tp_p = p_curr->name + "." + tp_p;
            p_curr = p_curr->parent;
        }
        return tp_p;
    }

    static std::vector<std::string> splitPath(const std::string& t_p) {
        std::vector<std::string> r;
        std::stringstream ss(t_p);
        std::string i;
        while (std::getline(ss, i, '.'))
            if (!i.empty())
                r.push_back(i);
        return r;
    }
};

} // namespace sgrn
