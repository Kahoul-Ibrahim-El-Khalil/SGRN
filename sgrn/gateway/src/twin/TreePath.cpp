#include <sgrn/Result.hpp>
#include <sgrn/gateway/twin/TreePath.hpp>
#include <cctype>
#include <sstream>
namespace sgrn::gateway::twin
{

TreePath::TreePath(std::vector<std::string> t_segments)
    : segments_(std::move(t_segments)) {
}

TreePath TreePath::fromDotted(const std::string& t_path) {
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

TreePath TreePath::fromSlashed(const std::string& t_path) {
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

std::optional<TreePath> TreePath::parent() const {
    if (segments_.size() <= 1) {
        return std::nullopt;
    }
    std::vector<std::string> parent_segs(segments_.begin(), segments_.end() - 1);
    return TreePath(std::move(parent_segs));
}

bool TreePath::isPrefixOf(const TreePath& t_other) const {
    if (segments_.size() > t_other.segments_.size()) {
        return false;
    }
    for (size_t i = 0; i < segments_.size(); ++i) {
        // Case-insensitive comparison
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

std::string TreePath::toDotted() const {
    std::string out;
    for (size_t i = 0; i < segments_.size(); ++i) {
        if (i > 0)
            out.push_back('.');
        out.append(segments_[i]);
    }
    return out;
}

std::string TreePath::toSlashed() const {
    std::string out;
    for (size_t i = 0; i < segments_.size(); ++i) {
        if (i > 0)
            out.push_back('/');
        out.append(segments_[i]);
    }
    return out;
}

bool TreePath::operator==(const TreePath& t_other) const {
    SGRN_RETURN_IF(segments_.size() != t_other.segments_.size(), false);

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

size_t TreePathHash::operator()(const TreePath& t_path) const {
    size_t h = 0;
    for (const auto& seg : t_path.t_segments()) {
        for (char c : seg) {
            h = h * 31 + static_cast<size_t>(std::toupper(static_cast<unsigned char>(c)));
        }
        h = h * 31 + '/'; // delimiter mix
    }
    return h;
}

} // namespace sgrn::gateway::twin
