#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace sgrn::gateway::twin
{

/**
 * @brief Canonical addressing type for the Twin hierarchy.
 *
 * Provides a unified way to address nodes (e.g. Data Blocks, Structs, fields, arrays)
 * supporting round-tripping between dotted (`a.b.c`) and slashed (`a/b/c`) representations.
 */
class TreePath {
public:
    TreePath() = default;

    /// Construct from a pre-parsed list of segments.
    explicit TreePath(std::vector<std::string> t_segments);

    /// Parse from a dotted string (e.g. "Mixer.Pump.Motor")
    static TreePath fromDotted(const std::string& t_path);

    /// Parse from a slashed string (e.g. "Mixer/Pump/Motor")
    static TreePath fromSlashed(const std::string& t_path);

    /// Get the parent path, or std::nullopt if this is the root (or empty).
    std::optional<TreePath> parent() const;

    /// Returns true if this path is a prefix of (or equal to) the other path.
    bool isPrefixOf(const TreePath& t_other) const;

    /// Format as a dotted string.
    std::string toDotted() const;

    /// Format as a slashed string.
    std::string toSlashed() const;

    /// Number of segments
    size_t size() const {
        return segments_.size();
    }

    /// Returns true if empty
    bool empty() const {
        return segments_.empty();
    }

    /// Access segments
    const std::vector<std::string>& t_segments() const {
        return segments_;
    }

    /// Case-insensitive equality
    bool operator==(const TreePath& t_other) const;
    bool operator!=(const TreePath& t_other) const {
        return !(*this == t_other);
    }

private:
    std::vector<std::string> segments_;
};

struct TreePathHash {
    size_t operator()(const TreePath& t_path) const;
};

struct TreePathEqual {
    bool operator()(const TreePath& t_a, const TreePath& t_b) const {
        return t_a == t_b;
    }
};

} // namespace sgrn::gateway::twin
