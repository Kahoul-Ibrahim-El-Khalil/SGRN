#pragma once

#include <cctype>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sgrn::common
{

/**
 * @brief Canonical addressing type for hierarchical paths.
 *
 * Provides a unified way to address nodes (e.g. Data Blocks, Structs, fields, arrays)
 * supporting round-tripping between dotted (`a.b.c`) and slashed (`a/b/c`) representations.
 */
class TreePath {
public:
    TreePath() = default;

    /// Construct from a pre-parsed list of segments.
    explicit TreePath(std::vector<std::string> t_segments)
        : segments_(std::move(t_segments)) {
    }

    /// Parse from a dotted string (e.g. "Mixer.Pump.Motor")
    static TreePath fromDotted(const std::string& t_path) {
        std::vector<std::string> segs;
        std::string current;
        for (char c : t_path) {
            if (c == '.') {
                if (!current.empty()) {
                    segs.push_back(current);
                    current.clear();
                }
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty()) {
            segs.push_back(current);
        }
        return TreePath(std::move(segs));
    }

    /// Parse from a slashed string (e.g. "Mixer/Pump/Motor")
    static TreePath fromSlashed(const std::string& t_path) {
        std::vector<std::string> segs;
        std::string current;
        for (char c : t_path) {
            if (c == '/') {
                if (!current.empty()) {
                    segs.push_back(current);
                    current.clear();
                }
            } else {
                current.push_back(c);
            }
        }
        if (!current.empty()) {
            segs.push_back(current);
        }
        return TreePath(std::move(segs));
    }

    /// Get the parent path, or std::nullopt if this is the root (or empty).
    std::optional<TreePath> parent() const {
        if (segments_.size() <= 1) {
            return std::nullopt;
        }
        std::vector<std::string> parent_segs(segments_.begin(), segments_.end() - 1);
        return TreePath(std::move(parent_segs));
    }

    /// Returns true if this path is a prefix of (or equal to) the other path.
    bool isPrefixOf(const TreePath& t_other) const {
        if (segments_.size() > t_other.segments_.size()) {
            return false;
        }
        for (size_t i = 0; i < segments_.size(); ++i) {
            const auto& a = segments_[i];
            const auto& b = t_other.segments_[i];
            if (a.size() != b.size())
                return false;
            for (size_t j = 0; j < a.size(); ++j) {
                if (std::toupper(static_cast<unsigned char>(a[j])) != std::toupper(static_cast<unsigned char>(b[j]))) {
                    return false;
                }
            }
        }
        return true;
    }

    /// Format as a dotted string.
    std::string toDotted() const {
        std::string out;
        for (size_t i = 0; i < segments_.size(); ++i) {
            if (i > 0)
                out.push_back('.');
            out.append(segments_[i]);
        }
        return out;
    }

    /// Format as a slashed string.
    std::string toSlashed() const {
        std::string out;
        for (size_t i = 0; i < segments_.size(); ++i) {
            if (i > 0)
                out.push_back('/');
            out.append(segments_[i]);
        }
        return out;
    }

    /// Number of segments
    size_t size() const noexcept {
        return segments_.size();
    }

    /// Returns true if empty
    bool empty() const noexcept {
        return segments_.empty();
    }

    /// Access segments
    const std::vector<std::string>& t_segments() const noexcept {
        return segments_;
    }

    /// Case-insensitive equality
    bool operator==(const TreePath& t_other) const {
        if (segments_.size() != t_other.segments_.size())
            return false;

        for (size_t i = 0; i < segments_.size(); ++i) {
            const auto& a = segments_[i];
            const auto& b = t_other.segments_[i];
            if (a.size() != b.size())
                return false;
            for (size_t j = 0; j < a.size(); ++j) {
                if (std::toupper(static_cast<unsigned char>(a[j])) != std::toupper(static_cast<unsigned char>(b[j]))) {
                    return false;
                }
            }
        }
        return true;
    }

    bool operator!=(const TreePath& t_other) const {
        return !(*this == t_other);
    }

private:
    std::vector<std::string> segments_;
};

struct TreePathHash {
    size_t operator()(const TreePath& t_path) const {
        size_t h = 0;
        for (const auto& seg : t_path.t_segments()) {
            for (char c : seg) {
                h = h * 31 + static_cast<size_t>(std::toupper(static_cast<unsigned char>(c)));
            }
            h = h * 31 + '/';
        }
        return h;
    }
};

struct TreePathEqual {
    bool operator()(const TreePath& t_a, const TreePath& t_b) const {
        return t_a == t_b;
    }
};

} // namespace sgrn::common
