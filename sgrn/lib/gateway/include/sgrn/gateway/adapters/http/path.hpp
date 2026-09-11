#pragma once
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <optional>
#include <rapidjson/document.h>
#include <string>
#include <vector>

namespace sgrn::gateway::adapters
{
struct Resolution {
    const ::sgrn::scl::DbSchema* schema{nullptr};
    std::string field_path;
    std::optional<size_t> array_index;
};

bool isAllDigits(const std::string& t_s);
Resolution resolveSemanticPath(const std::vector<std::string>& t_segs, const ::sgrn::scl::PlcSchemaStore& t_registry);
void collectLeaves(const rapidjson::Value& t_node, const std::string& t_prefix, std::vector<std::pair<std::string, std::string>>& t_out);
} // namespace sgrn::gateway::adapters
