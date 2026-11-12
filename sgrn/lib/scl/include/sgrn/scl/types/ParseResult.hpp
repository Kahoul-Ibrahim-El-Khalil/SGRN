#pragma once
#include <fmt/core.h>
#include <sgrn/scl/types/DbSchema.hpp>
#include <sgrn/scl/types/PlcTag.hpp>
#include <sgrn/scl/types/UdtDefinition.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace sgrn::scl
{
struct ParseResult {
    std::vector<DbSchema> dbs;
    std::vector<UdtDefinition> udts;
    /// TIA-style tag-table rows (#TAG_TABLE blocks): name + type + address.
    std::vector<PlcTag> tags;
    std::vector<std::string> warnings;
    /// File-level declared dimensions (#DIMENSIONS(...)). Empty = undeclared:
    /// any #DIMENSION value is accepted without validation.
    std::vector<std::string> dimensions;
};

} // namespace sgrn::scl

template <>
struct fmt::formatter<sgrn::scl::ParseResult> : formatter<std::string_view> {
    auto format(const sgrn::scl::ParseResult& t_result, format_context& t_ctx) const {
        return formatter<std::string_view>::format(
            fmt::format("ParseResult{{dbs={}, udts={}, tags={}}}", t_result.dbs.size(), t_result.udts.size(), t_result.tags.size()), t_ctx);
    }
};
