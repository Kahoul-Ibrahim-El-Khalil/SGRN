#include <sgrn/scl/types/ParseResult.hpp>

#include <sgrn/Result.hpp>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string>

using sgrn::Result;
using sgrn::scl::ParseResult;
// -----------------------------------------------------------------------------
// global namespace — functional JSON form
// -----------------------------------------------------------------------------

std::string toJsonString(const sgrn::scl::ParseResult& t_parse_result) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);
    sgrn::scl::serializeToWriter(t_writer, t_parse_result);
    return sb.GetString();
}

rapidjson::Document toJson(const sgrn::scl::ParseResult& t_parse_result) {
    rapidjson::Document doc;
    doc.Parse(toJsonString(t_parse_result).c_str());
    return doc;
}

// -----------------------------------------------------------------------------
// from-side — per-type namespace
// -----------------------------------------------------------------------------

namespace sgrn::scl::parsing
{

Result<ParseResult, std::string> fromJson(const rapidjson::Value& t_node) {
    sgrn::scl::ParseResult t_result;
    if (!t_node.IsObject()) {
        return sgrn::Error("Object is Empty");
    }

    if (t_node.HasMember("dbs") && t_node["dbs"].IsArray()) {
        for (const auto& db_node : t_node["dbs"].GetArray())
            t_result.dbs.push_back(sgrn::scl::db::fromJson(db_node));
    }
    if (t_node.HasMember("udts") && t_node["udts"].IsArray()) {
        for (const auto& udt_node : t_node["udts"].GetArray())
            t_result.udts.push_back(sgrn::scl::udt::fromJson(udt_node));
    }
    if (t_node.HasMember("warnings") && t_node["warnings"].IsArray()) {
        for (const auto& warn : t_node["warnings"].GetArray())
            if (warn.IsString())
                t_result.warnings.push_back(warn.GetString());
    }

    return t_result;
}

Result<ParseResult, std::string> fromJsonString(const std::string& t_value) {
    rapidjson::Document doc;
    doc.Parse(t_value.c_str());
    return fromJson(doc);
}

} // namespace sgrn::scl::parsing
