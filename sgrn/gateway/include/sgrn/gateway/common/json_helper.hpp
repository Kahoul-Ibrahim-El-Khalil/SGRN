#pragma once

#include <sgrn/utils/strings.hpp>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <set>
#include <string>
#include <vector>

namespace sgrn::gateway::common
{

/**
 * @brief JSON utility functions for protocol adapters
 */
namespace json_helper
{

/**
 * @brief Parse JSON string to Document
 * @return Result with Document or error string
 */
inline sgrn::Result<rapidjson::Document, std::string> parse(const std::string& t_json) {
    rapidjson::Document doc;
    doc.Parse(t_json.c_str());
    if (doc.HasParseError()) {
        return "JSON parse error at offset " + std::to_string(doc.GetErrorOffset());
    }
    if (!doc.IsObject()) {
        return "JSON root must be an object";
    }
    return doc;
}

/**
 * @brief Extract a field value from JSON document as string
 * @example extractField(doc, "ReactorCore.speed") → "12.5"
 */
inline std::string extractField(const rapidjson::Document& t_doc, std::string_view t_path) {

    auto parts = sgrn::utils::strings::tokenize(t_path, '.');
    const rapidjson::Value* current = &t_doc;

    for (const auto& part : parts) {
        if (!current->IsObject() || !current->HasMember(part.c_str())) {
            return "null";
        }
        current = &(*current)[part.c_str()];
    }

    if (current->IsString()) {
        return current->GetString();
    } else if (current->IsNumber()) {
        if (current->IsInt())
            return std::to_string(current->GetInt());
        if (current->IsInt64())
            return std::to_string(current->GetInt64());
        if (current->IsUint())
            return std::to_string(current->GetUint());
        if (current->IsUint64())
            return std::to_string(current->GetUint64());
        if (current->IsDouble())
            return std::to_string(current->GetDouble());
        if (current->IsFloat())
            return std::to_string(current->GetFloat());
    } else if (current->IsBool()) {
        return current->GetBool() ? "true" : "false";
    } else if (current->IsNull()) {
        return "null";
    }

    return "null";
}

/**
 * @brief Recursively copy a specific path from a source JSON value into a
 *        destination JSON object, preserving the full nested structure.
 *
 * @param t_src   Source JSON value (must be an object)
 * @param t_dst   Destination JSON value (will have members added)
 * @param t_alloc Memory pool allocator from the destination document
 * @param t_tokens Path segments (e.g. ["ReactorCore", "sensors", "valve"])
 * @param t_depth Current index into tokens being processed
 *
 * Example: copyJsonPath(src={"a":{"b":{"c":1}}}, dst={}, tokens=["a","b","c"], 0)
 *   Result: dst={"a":{"b":{"c":1}}}
 */
inline void copyJsonPath(const rapidjson::Value& t_src, rapidjson::Value& t_dst, rapidjson::MemoryPoolAllocator<>& t_alloc,
    const std::vector<std::string>& t_tokens, size_t t_depth) {
    if (t_depth >= t_tokens.size() || !t_src.IsObject())
        return;

    const auto& token = t_tokens[t_depth];
    if (!t_src.HasMember(token.c_str()))
        return;

    if (t_depth == t_tokens.size() - 1) {
        // Last token — copy the value directly (any type: scalar, array, object, null)
        rapidjson::Value key(token.c_str(), t_alloc);
        rapidjson::Value val;
        val.CopyFrom(t_src[token.c_str()], t_alloc);
        t_dst.AddMember(key, val, t_alloc);
    } else {
        // Intermediate token — recurse into child object
        const auto& child = t_src[token.c_str()];
        if (child.IsObject()) {
            rapidjson::Value nested;
            nested.SetObject();
            copyJsonPath(child, nested, t_alloc, t_tokens, t_depth + 1);
            if (!nested.ObjectEmpty()) {
                rapidjson::Value key(token.c_str(), t_alloc);
                t_dst.AddMember(key, nested, t_alloc);
            }
        }
    }
}

/**
 * @brief Filter JSON document to only include specified fields
 * @example filterFields(doc, {"ReactorCore.speed", "Pump1.status"})
 *
 * Preserves the full nested structure of each requested path and copies
 * values of any JSON type (scalars, arrays, nested objects, null).
 */
inline std::string filterFields(const rapidjson::Document& t_doc, const std::set<std::string>& t_fields) {

    rapidjson::Document filtered;
    filtered.SetObject();
    auto& alloc = filtered.GetAllocator();

    for (const auto& field : t_fields) {
        auto parts = sgrn::utils::strings::tokenize(field, '.');
        if (parts.size() < 2)
            continue;

        const std::string& db_name = parts[0];
        if (!t_doc.HasMember(db_name.c_str()))
            continue;

        // Build nested object for this field, preserving intermediate levels
        rapidjson::Value db_obj;
        db_obj.SetObject();
        copyJsonPath(t_doc[db_name.c_str()], db_obj, alloc, parts, 1);

        if (db_obj.ObjectEmpty())
            continue;

        // Merge multiple fields from the same DB
        if (filtered.HasMember(db_name.c_str())) {
            for (auto it = db_obj.MemberBegin(); it != db_obj.MemberEnd(); ++it) {
                rapidjson::Value key(it->name.GetString(), alloc);
                rapidjson::Value val;
                val.CopyFrom(it->value, alloc);
                filtered[db_name.c_str()].AddMember(key, val, alloc);
            }
        } else {
            rapidjson::Value key(db_name.c_str(), alloc);
            filtered.AddMember(key, db_obj, alloc);
        }
    }

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    filtered.Accept(writer);
    return buffer.GetString();
}

/**
 * @brief Serialize an array of JSON objects to a compact JSON string.
 *
 * The caller provides a row builder callback that populates each object item.
 * This eliminates the repetitive "create Document → SetArray → AddMember →
 * serializeCompact" boilerplate in HTTP handlers while preserving native
 * JSON value types (numbers stay numbers, not stringified).
 *
 * @param t_row_count   Number of items in the array
 * @param t_row_builder Callable (rapidjson::Value& item, allocator& alloc, size_t index)
 */
template <typename RowBuilder>
inline std::string buildArrayResponse(size_t t_row_count, RowBuilder&& t_row_builder) {
    rapidjson::Document root;
    auto& alloc = root.GetAllocator();
    root.SetArray();

    for (size_t i = 0; i < t_row_count; ++i) {
        rapidjson::Value item(rapidjson::kObjectType);
        t_row_builder(item, alloc, i);
        root.PushBack(item, alloc);
    }

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    root.Accept(writer);
    return buffer.GetString();
}

/**
 * @brief Serialize a simple key-value pair to JSON
 */
inline std::string serializeValue(std::string_view t_key, std::string_view t_value) {

    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();

    doc.AddMember(rapidjson::Value(t_key.data(), alloc), rapidjson::Value(t_value.data(), alloc), alloc);

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    return buffer.GetString();
}

/**
 * @brief Serialize a JSON object from key-value pairs
 */
inline std::string serializeObject(const std::vector<std::pair<std::string, std::string>>& t_pairs) {

    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();

    for (const auto& [key, value] : t_pairs) {
        doc.AddMember(rapidjson::Value(key.c_str(), alloc), rapidjson::Value(value.c_str(), alloc), alloc);
    }

    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);
    return buffer.GetString();
}

} // namespace json_helper

} // namespace sgrn::gateway::common
