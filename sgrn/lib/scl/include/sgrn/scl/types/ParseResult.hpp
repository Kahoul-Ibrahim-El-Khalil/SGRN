#pragma once
#include <fmt/core.h>
#include <sgrn/scl/types/DbSchema.hpp>
#include <sgrn/scl/types/UdtDefinition.hpp>
#include <rapidjson/document.h>
#include <string>
#include <vector>

namespace sgrn::scl
{
struct ParseResult {
    std::vector<DbSchema> dbs;
    std::vector<UdtDefinition> udts;
    std::vector<std::string> warnings;
};

/// Writer-agnostic JSON "form" for ParseResult (dbs + udts + warnings).
template <typename Writer>
inline void serializeToWriter(Writer& t_writer, const sgrn::scl::ParseResult& t_result) {
    t_writer.StartObject();
    t_writer.Key("dbs");
    t_writer.StartArray();
    for (const auto& t_db : t_result.dbs) {
        sgrn::scl::db::serializeToWriter(t_writer, t_db);
    }
    t_writer.EndArray();
    t_writer.Key("udts");
    t_writer.StartArray();
    for (const auto& t_udt : t_result.udts) {
        sgrn::scl::udt::serializeToWriter(t_writer, t_udt);
    }
    t_writer.EndArray();
    if (!t_result.warnings.empty()) {
        t_writer.Key("warnings");
        t_writer.StartArray();
        for (const auto& warn : t_result.warnings) {
            t_writer.String(warn.c_str());
        }
        t_writer.EndArray();
    }
    t_writer.EndObject();
}
/// from-side lives in its own namespace (C++ cannot overload on return type globally).
sgrn::scl::ParseResult fromJson(const rapidjson::Value& t_value);
sgrn::scl::ParseResult fromJsonString(const std::string& t_value);

} // namespace sgrn::scl

// global namespace — functional JSON form
std::string toJsonString(const sgrn::scl::ParseResult& t_parse_result);
rapidjson::Document toJson(const sgrn::scl::ParseResult& t_parse_result);

template <>
struct fmt::formatter<sgrn::scl::ParseResult> : formatter<std::string_view> {
    auto format(const sgrn::scl::ParseResult& t_result, format_context& t_ctx) const {
        return formatter<std::string_view>::format(
            fmt::format("ParseResult{{dbs={}, udts={}}}", t_result.dbs.size(), t_result.udts.size()), t_ctx);
    }
};
