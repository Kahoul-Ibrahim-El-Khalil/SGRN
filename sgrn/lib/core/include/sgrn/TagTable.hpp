#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/types/Value.hpp>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sgrn
{

/**
 * @brief Representation of a single symbolic tag with its address information.
 */
struct Tag {
    std::string name;
    std::string type_name;
    size_t size{0};
    Json::Value address_context; // Protocol-specific address (e.g., DB10.DBX0.0)
};

/**
 * @brief Manages a flat list of tags and their association with a MemoryProvider.
 */
class TagTable {
public:
    TagTable() = default;

    void addTag(Tag t_tag) {
        std::string t_name = t_tag.name;
        tags_[t_name] = std::move(t_tag);
    }

    const Tag* findTag(const std::string& t_name) const {
        auto it = tags_.find(t_name);
        return it != tags_.end() ? &it->second : nullptr;
    }

    std::vector<std::string> tagNames() const {
        std::vector<std::string> names;
        for (const auto& [t_name, _] : tags_)
            names.push_back(t_name);
        return names;
    }

    size_t tagCount() const {
        return tags_.size();
    }

    void clear() {
        tags_.clear();
    }

    // JSON persistence
    Json::Value toJson() const;
    void loadFromJson(const Json::Value& t_root);

private:
    std::map<std::string, Tag> tags_;
};

} // namespace sgrn
