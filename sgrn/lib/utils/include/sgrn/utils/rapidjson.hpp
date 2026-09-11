#pragma once

#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/pointer.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <string>
#include <vector>

namespace sgrn::utils::rapidjson
{

/**
 * @brief Extracts a specific subtree from a JSON string using a path.
 * @param json_str The source JSON string.
 * @param path The path to extract (e.g. "Mixer/Subsystem" or "Inlet.Pressure").
 * @param separator The character used to separate path segments.
 * @return The extracted JSON string, or std::nullopt if the path doesn't exist.
 */
inline std::optional<std::string> extractSubtree(const std::string& t_json_str, const std::string& t_path, char t_separator = '/') {
    if (t_json_str.empty())
        return std::nullopt;
    if (t_path.empty())
        return t_json_str;

    ::rapidjson::Document doc;
    doc.Parse(t_json_str.c_str());
    if (doc.HasParseError())
        return std::nullopt;

    // Convert our custom path (A/B/C) to a RapidJSON Pointer (/A/B/C)
    std::string rj_pointer = "/";
    for (char c : t_path) {
        if (c == t_separator)
            rj_pointer += '/';
        else
            rj_pointer += c;
    }

    ::rapidjson::Pointer ptr(rj_pointer.c_str());
    if (const ::rapidjson::Value* p_val = ptr.Get(doc)) {
        ::rapidjson::StringBuffer sb;
        ::rapidjson::Writer<::rapidjson::StringBuffer> writer(sb);
        p_val->Accept(writer);
        return sb.GetString();
    }

    return std::nullopt;
}

} // namespace sgrn::utils::rapidjson
