#pragma once

#include <sgrn/types/UniversalType.hpp>
#include <json/json.h>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace sgrn
{

/**
 * @brief Definition of a single field within a structure or memory block.
 */
struct FieldDefinition {
    std::string name;
    UniversalType type{UniversalType::Unknown};
    size_t offset{0};
    size_t size{0};
    int bit_index{-1};
    size_t count{1}; // For arrays

    std::string template_name; // Reference to a SchemaTemplate
    std::vector<FieldDefinition> children;

    Json::Value context; // Protocol-specific attributes
};

/**
 * @brief A reusable schema template (e.g., a UDT or a specific structure).
 */
struct SchemaTemplate {
    std::string name;
    size_t size{0};
    std::vector<FieldDefinition> fields;

    Json::Value context;
};

/**
 * @brief Definition of a top-level memory block (e.g., an S7 DB or Modbus range).
 */
struct BlockDefinition {
    std::string name;
    uint32_t id{0}; // Numeric ID (e.g., DB number)
    size_t size{0};
    std::vector<FieldDefinition> fields;

    Json::Value context;
};

/**
 * @brief Protocol-agnostic schema directory manager.
 *
 * Stores templates and block definitions that can be used to populate a Registry.
 */
class SchemaStore {
public:
    void addTemplate(SchemaTemplate t_t) {
        templates_[t_t.name] = std::move(t_t);
    }

    const SchemaTemplate* findTemplate(const std::string& t_name) const {
        auto it = templates_.find(t_name);
        return it != templates_.end() ? &it->second : nullptr;
    }

    void addBlock(BlockDefinition t_b) {
        blocks_[t_b.name] = t_b;
        blocks_by_id_[t_b.id] = &blocks_[t_b.name];
    }

    const BlockDefinition* findBlock(const std::string& t_name) const {
        auto it = blocks_.find(t_name);
        return it != blocks_.end() ? &it->second : nullptr;
    }

    const BlockDefinition* findBlock(uint32_t t_id) const {
        auto it = blocks_by_id_.find(t_id);
        return it != blocks_by_id_.end() ? it->second : nullptr;
    }

    const std::map<std::string, SchemaTemplate>& templates() const {
        return templates_;
    }
    const std::map<std::string, BlockDefinition>& blocks() const {
        return blocks_;
    }

    void clear() {
        templates_.clear();
        blocks_.clear();
        blocks_by_id_.clear();
    }

    // JSON persistence
    Json::Value toJson() const;
    void loadFromJson(const Json::Value& t_root);

private:
    std::map<std::string, SchemaTemplate> templates_;
    std::map<std::string, BlockDefinition> blocks_;
    std::map<uint32_t, const BlockDefinition*> blocks_by_id_;
};

} // namespace sgrn
