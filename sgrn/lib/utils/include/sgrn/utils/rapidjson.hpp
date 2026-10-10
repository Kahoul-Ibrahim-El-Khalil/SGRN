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
    if (doc.HasParseError()) {
        fprintf(stderr, "extractSubtree: Parse error: %d\n", doc.GetParseError());
        return std::nullopt;
    }

    std::vector<std::string> segments;
    size_t start = 0;
    while (start < t_path.size()) {
        size_t end = t_path.find(t_separator, start);
        if (end == std::string::npos) {
            segments.push_back(t_path.substr(start));
            break;
        }
        segments.push_back(t_path.substr(start, end - start));
        start = end + 1;
    }

    fprintf(stderr, "extractSubtree: path='%s', segments=%zu\n", t_path.c_str(), segments.size());

    const ::rapidjson::Value* current = &doc;
    for (const auto& segment : segments) {
        if (!current->IsObject()) {
            fprintf(stderr, "extractSubtree: not object at segment '%s'\n", segment.c_str());
            return std::nullopt;
        }

        const ::rapidjson::Value* found = nullptr;
        for (auto it = current->MemberBegin(); it != current->MemberEnd(); ++it) {
            if (strcasecmp(it->name.GetString(), segment.c_str()) == 0) {
                found = &it->value;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "extractSubtree: field not found: '%s', available: ", segment.c_str());
            for (auto it = current->MemberBegin(); it != current->MemberEnd(); ++it) {
                fprintf(stderr, "%s ", it->name.GetString());
            }
            fprintf(stderr, "\n");
            return std::nullopt;
        }
        current = found;
    }

    ::rapidjson::StringBuffer sb;
    ::rapidjson::Writer<::rapidjson::StringBuffer> writer(sb);
    current->Accept(writer);
    std::string result = sb.GetString();
    fprintf(stderr, "extractSubtree: result length=%zu\n", result.length());
    return result;
}

} // namespace sgrn::utils::rapidjson
