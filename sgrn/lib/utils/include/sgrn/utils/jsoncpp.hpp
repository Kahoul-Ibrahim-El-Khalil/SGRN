#pragma once

// jsoncpp-only helpers for sgrn/datastore (and jwt). Edge targets use sgrn/utils/json.hpp (rapidjson).

#include <fmt/core.h>
#include <sgrn/Result.hpp>
#include <sgrn/utils/strings.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <json/json.h>
#include <json/value.h>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace sgrn::utils::json
{

namespace fs = std::filesystem;

/**
 * @brief Internal helper to load a JSON value from a file.
 */
inline sgrn::Result<Json::Value, std::string> deserializeFromFile(const fs::path& t_path) {
    std::ifstream ifs(t_path, std::ios::binary);
    if (!ifs.is_open()) {
        return fmt::format("Failed to open JSON file: {}", t_path.string());
    }
    Json::Value t_root;
    Json::CharReaderBuilder builder;
    std::string errs;
    if (!Json::parseFromStream(builder, ifs, &t_root, &errs)) {
        return fmt::format("Failed to parse JSON from {}: {}", t_path.string(), errs);
    }
    return t_root;
}

/**
 * @brief Serializes a Json::Value into a compact std::string (no whitespace).
 */
inline std::string serialize(const Json::Value& t_json_value, const std::string& t_indentation = "") {
    Json::StreamWriterBuilder t_w;
    t_w["indentation"] = t_indentation;
    return Json::writeString(t_w, t_json_value);
}

/**
 * @brief Serializes a Json::Value into a compact JSON string (no whitespace).
 */
inline std::string serializeCompact(const Json::Value& t_json_value) {
    return serialize(t_json_value, "");
}

/**
 * @brief Serializes a Json::Value into a pretty-printed JSON string.
 */
inline std::string serializePretty(const Json::Value& t_json_value) {
    return serialize(t_json_value, "  ");
}

/**
 * @brief Alias for serializePretty.
 */
inline std::string toString(const Json::Value& t_json_value) {
    return serializePretty(t_json_value);
}

inline bool serializeToFile(const fs::path& t_output_path, const Json::Value& t_json_value, const std::string& t_indentation = "") {
    std::ofstream file(t_output_path.string());
    if (!file.is_open())
        return false;

    Json::StreamWriterBuilder writer;
    writer["indentation"] = t_indentation;
    std::unique_ptr<Json::StreamWriter> const sw(writer.newStreamWriter());
    sw->write(t_json_value, &file);
    file << '\n';
    return file.good();
}

/**
 * @brief Converts a JSON string into a Json::Value object.
 */
inline sgrn::Result<Json::Value, std::string> deserialize(std::string_view t_json_str, bool t_collect_comments = false) {
    Json::CharReaderBuilder reader_builder;
    reader_builder["collectComments"] = t_collect_comments;

    Json::Value t_root;
    std::string errs;

    std::unique_ptr<Json::CharReader> const reader(reader_builder.newCharReader());
    const bool ok = reader->parse(t_json_str.data(), t_json_str.data() + t_json_str.size(), &t_root, &errs);

    if (!ok)
        return fmt::format("Failed to parse JSON: {}", errs);
    return t_root;
}

inline sgrn::Result<Json::Value, std::string> deserialize(const std::string& t_json_str, bool t_collect_comments = false) {
    return deserialize(std::string_view(t_json_str), t_collect_comments);
}

/**
 * @brief Filters keys in-place, keeping only what is allowed.
 */
template <size_t N>
inline sgrn::Result<void, std::string> filterJson(Json::Value& t_root_json, const std::array<std::string_view, N>& t_allowed_keys) {
    try {
        if (!t_root_json.isObject()) {
            return {};
        }

        auto member_names = t_root_json.getMemberNames();
        for (const auto& t_key : member_names) {
            auto it = std::find(t_allowed_keys.begin(), t_allowed_keys.end(), t_key);
            if (it == t_allowed_keys.end()) {
                t_root_json.removeMember(t_key);
            }
        }
        return {};
    } catch (const std::exception& e) {
        return fmt::format("filterJson failed: {}", e.what());
    }
}

namespace detail
{
inline void walkFlatten(
    const Json::Value& t_val, std::string& t_prefix, std::string_view t_sep, Json::Value& t_flat, Json::StreamWriterBuilder& t_w) {
    if (t_val.isObject()) {
        for (const auto& t_key : t_val.getMemberNames()) {
            const auto old_len = t_prefix.size();
            t_prefix.append(t_key);
            t_prefix.append(t_sep);
            walkFlatten(t_val[t_key], t_prefix, t_sep, t_flat, t_w);
            t_prefix.resize(old_len);
        }
    } else {
        std::string_view final_key(t_prefix.data(), t_prefix.size() - t_sep.size());
        if (t_val.isArray()) {
            t_flat[std::string(final_key)] = Json::writeString(t_w, t_val);
        } else {
            t_flat[std::string(final_key)] = t_val;
        }
    }
}

inline void walkFlattenToString(const Json::Value& t_val, std::string& t_prefix, std::string_view t_sep, char t_kv, std::string& t_out,
    Json::StreamWriterBuilder& t_w) {
    if (t_val.isObject()) {
        for (const auto& t_key : t_val.getMemberNames()) {
            const auto old_len = t_prefix.size();
            t_prefix.append(t_key);
            t_prefix.append(t_sep);
            walkFlattenToString(t_val[t_key], t_prefix, t_sep, t_kv, t_out, t_w);
            t_prefix.resize(old_len);
        }
    } else {
        t_out.append(t_prefix.data(), t_prefix.size() - t_sep.size());
        t_out.push_back(t_kv);
        if (t_val.isArray()) {
            t_out.append(Json::writeString(t_w, t_val));
        } else {
            t_out.append(t_val.asString());
        }
        t_out.push_back('\n');
    }
}
} // namespace detail

inline void flattenJsonInplace(Json::Value& t_json, std::string_view t_sep) {
    Json::Value t_flat(Json::objectValue);
    Json::StreamWriterBuilder t_w;
    t_w["indentation"] = "";

    std::string t_prefix;
    t_prefix.reserve(128);
    detail::walkFlatten(t_json, t_prefix, t_sep, t_flat, t_w);
    t_json.swap(t_flat);
}

inline void applyScope(Json::Value& t_json, std::string_view t_scope, std::string_view t_scope_sep) {
    if (t_scope_sep.empty() || !t_json.isObject())
        return;

    Json::Value scoped(Json::objectValue);
    for (const auto& t_key : t_json.getMemberNames()) {
        std::string new_key;
        new_key.reserve(t_scope.size() + t_scope_sep.size() + t_key.size());
        new_key.append(t_scope);
        new_key.append(t_scope_sep);
        new_key.append(t_key);
        scoped[new_key] = std::move(t_json[t_key]);
    }
    t_json.swap(scoped);
}

inline std::string flattenJsonToString(const Json::Value& t_json, std::string_view t_sep, char t_kv = '=') {
    std::string t_out;
    Json::StreamWriterBuilder t_w;
    t_w["indentation"] = "";

    std::string t_prefix;
    t_prefix.reserve(128);
    detail::walkFlattenToString(t_json, t_prefix, t_sep, t_kv, t_out, t_w);
    return t_out;
}

/**
 * @brief Computes the delta between two JSON objects.
 */
inline Json::Value buildDelta(const Json::Value& t_prev, const Json::Value& t_cur) {
    Json::Value d(Json::objectValue);
    if (t_cur.isObject()) {
        for (const auto& k : t_cur.getMemberNames()) {
            if (!t_prev.isMember(k)) {
                d[k] = t_cur[k];
            } else {
                Json::Value delta_res = buildDelta(t_prev[k], t_cur[k]);
                if (!delta_res.isNull()) {
                    d[k] = delta_res;
                }
            }
        }
    } else if (t_cur != t_prev) {
        return t_cur;
    }
    return d.empty() ? Json::Value() : d;
}

/**
 * @brief Applies a delta to a base JSON object.
 */
inline void applyDelta(Json::Value& t_base, const Json::Value& t_delta) {
    if (!t_delta.isObject()) {
        t_base = t_delta;
        return;
    }
    if (!t_base.isObject()) {
        t_base = t_delta;
        return;
    }
    for (const auto& k : t_delta.getMemberNames()) {
        applyDelta(t_base[k], t_delta[k]);
    }
}

/**
 * @brief Merges two JSON values while checking for structural compatibility.
 */
inline sgrn::Result<Json::Value, std::string> mergeWithSchemaCheck(
    const Json::Value& t_a, const Json::Value& t_b, const std::string& t_path = "root") {
    if (t_a.isNull())
        return t_b;
    if (t_b.isNull())
        return t_a;

    if (t_a.isObject() && t_b.isObject()) {
        Json::Value result = t_a;
        for (const auto& k : t_b.getMemberNames()) {
            if (t_a.isMember(k)) {
                auto res = mergeWithSchemaCheck(t_a[k], t_b[k], t_path + "." + k);
                if (res.hasError())
                    return res.error();
                result[k] = res.value();
            } else {
                result[k] = t_b[k];
            }
        }
        return result;
    }

    if (t_a.type() != t_b.type()) {
        return fmt::format("Schema conflict at {}: base is {}, incoming is {}", t_path, (int)t_a.type(), (int)t_b.type());
    }

    return t_b;
}

/**
 * @brief Merges a base tree with multiple deltas loaded from the filesystem.
 */
inline sgrn::Result<Json::Value, std::string> mergeJsonDeltas(const fs::path& t_base_tree_path, std::span<const fs::path> t_deltas_paths) {

    if (!fs::exists(t_base_tree_path)) {
        return fmt::format("Path to JSON base tree: '{}' does not exist", t_base_tree_path.string());
    }

    auto baseline_res = deserializeFromFile(t_base_tree_path);
    if (baseline_res.hasError())
        return baseline_res.error();

    Json::Value result = std::move(baseline_res.value());

    for (const auto& t_path : t_deltas_paths) {
        auto delta_res = deserializeFromFile(t_path);
        if (delta_res.hasError())
            return delta_res.error();
        applyDelta(result, delta_res.value());
    }

    return result;
}

/**
 * @brief Computes the resultant tree from a span of JSON values.
 */
inline Json::Value computeResultantTree(std::span<const Json::Value> t_steps) {
    if (t_steps.empty())
        return Json::Value(Json::objectValue);

    Json::Value result = t_steps[0];
    for (size_t i = 1; i < t_steps.size(); ++i) {
        applyDelta(result, t_steps[i]);
    }
    return result;
}

/**
 * @brief Computes the resultant tree from a span of file paths.
 */
inline sgrn::Result<Json::Value, std::string> computeResultantTree(std::span<const fs::path> t_paths) {
    if (t_paths.empty())
        return Json::Value(Json::objectValue);
    return mergeJsonDeltas(t_paths[0], t_paths.subspan(1));
}

/**
 * @brief Writes a Json::Value to a file on disk.
 */
inline sgrn::Result<void, std::string> serializeAndWriteToFile(const fs::path& t_path, const Json::Value& t_json) {
    if (serializeToFile(t_path, t_json, "  "))
        return {};
    return fmt::format("Failed to open file for writing: {}", t_path.string());
}

/**
 * @brief Loads a JSON file from disk into a Json::Value.
 */
inline sgrn::Result<Json::Value, std::string> loadJsonFile(const fs::path& t_path) {
    return deserializeFromFile(t_path);
}

inline std::optional<int64_t> toSignedInteger(const Json::Value& t_value) {
    if (t_value.isInt64())
        return t_value.asInt64();
    if (t_value.isInt())
        return static_cast<int64_t>(t_value.asInt());
    if (t_value.isUInt64()) {
        const uint64_t unsigned_value = t_value.asUInt64();
        if (unsigned_value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
            return static_cast<int64_t>(unsigned_value);
        return std::nullopt;
    }
    if (t_value.isUInt())
        return static_cast<int64_t>(t_value.asUInt());
    if (t_value.isDouble())
        return static_cast<int64_t>(t_value.asDouble());
    if (t_value.isString()) {
        if (std::optional<int64_t> parsed = sgrn::utils::strings::parseInt64(t_value.asString())) {
            return parsed.value();
        }
    }
    return std::nullopt;
}

inline std::optional<uint64_t> toUnsignedInteger(const Json::Value& t_value) {
    if (t_value.isUInt64())
        return t_value.asUInt64();
    if (t_value.isUInt())
        return static_cast<uint64_t>(t_value.asUInt());
    if (t_value.isInt64()) {
        const int64_t signed_value = t_value.asInt64();
        if (signed_value >= 0)
            return static_cast<uint64_t>(signed_value);
        return std::nullopt;
    }
    if (t_value.isInt()) {
        const int32_t signed_value = t_value.asInt();
        if (signed_value >= 0)
            return static_cast<uint64_t>(signed_value);
        return std::nullopt;
    }
    if (t_value.isDouble()) {
        const double numeric = t_value.asDouble();
        if (numeric >= 0.0)
            return static_cast<uint64_t>(numeric);
        return std::nullopt;
    }
    if (t_value.isString()) {
        if (std::optional<uint64_t> parsed = sgrn::utils::strings::parseUInt64(t_value.asString())) {
            return parsed.value();
        }
    }
    return std::nullopt;
}

inline std::optional<double> toFloatingPoint(const Json::Value& t_value) {
    if (t_value.isNumeric())
        return t_value.asDouble();
    if (t_value.isString()) {
        if (std::optional<double> parsed = sgrn::utils::strings::parseDouble(t_value.asString())) {
            return parsed.value();
        }
    }
    return std::nullopt;
}

/**
 * @brief Inserts a value into a JSON object using a dot-notated path (e.g., "a.b.c").
 * If intermediate objects do not exist, they are created.
 */
inline void insertNestedValue(Json::Value& t_root, const std::string& t_path, const Json::Value& t_value, char t_sep = '.') {
    std::size_t start = 0;
    Json::Value* p_cursor = &t_root;
    while (start < t_path.size()) {
        const std::size_t sep_pos = t_path.find(t_sep, start);
        const std::string segment = sep_pos == std::string::npos ? t_path.substr(start) : t_path.substr(start, sep_pos - start);
        if (segment.empty()) {
            return;
        }
        if (sep_pos == std::string::npos) {
            (*p_cursor)[segment] = t_value;
            return;
        }
        p_cursor = &((*p_cursor)[segment]);
        if (!p_cursor->isObject()) {
            *p_cursor = Json::Value(Json::objectValue);
        }
        start = sep_pos + 1;
    }
}

/**
 * @brief Coerces a string into a JSON value (boolean, number, or string).
 */
inline Json::Value fromString(const std::string& t_val) {
    if (t_val == "true")
        return true;
    if (t_val == "false")
        return false;
    if (t_val == "null")
        return Json::Value(Json::nullValue);

    // Try parsing as number
    try {
        size_t pos;
        double d = std::stod(t_val, &pos);
        if (pos == t_val.size()) {
            // Check if it's an integer to avoid .0 representation if possible
            if (t_val.find('.') == std::string::npos && t_val.find('e') == std::string::npos && t_val.find('E') == std::string::npos) {
                try {
                    return static_cast<Json::Int64>(std::stoll(t_val));
                } catch (...) {
                    // safe to discard: if stoll fails, we fallback to stod or string
                }
            }
            return d;
        }
    } catch (...) {
        // safe to discard: if stod fails, we fallback to string
    }

    return t_val;
}

/**
 * @brief Returns the member name for a given JSON type.
 */
inline std::string typeName(const Json::ValueType t_type) {
    switch (t_type) {
        case Json::nullValue:
            return "null";
        case Json::intValue:
        case Json::uintValue:
            return "integer";
        case Json::realValue:
            return "number";
        case Json::stringValue:
            return "string";
        case Json::booleanValue:
            return "boolean";
        case Json::arrayValue:
            return "array";
        case Json::objectValue:
            return "object";
    }
    return "unknown";
}

/**
 * @brief Ensures a member exists and has the correct type.
 */
inline sgrn::Result<const Json::Value*, std::string> requireMember(
    const Json::Value& t_parent, const std::string& t_key, Json::ValueType t_type) {
    if (!t_parent.isMember(t_key))
        return fmt::format("missing '{}'", t_key);
    const Json::Value* p_value = &t_parent[t_key];
    if (p_value->type() != t_type) {
        return fmt::format("'{}' must be {}", t_key, typeName(t_type));
    }
    return p_value;
}

inline sgrn::Result<std::string, std::string> requireString(const Json::Value& t_parent, const std::string& t_key) {
    auto res = requireMember(t_parent, t_key, Json::stringValue);
    if (res.hasError())
        return res.error();
    return res.value()->asString();
}

inline sgrn::Result<int, std::string> requireInt(const Json::Value& t_parent, const std::string& t_key) {
    auto res = requireMember(t_parent, t_key, Json::intValue);
    if (res.hasError()) {
        // Fallback for uint
        res = requireMember(t_parent, t_key, Json::uintValue);
        if (res.hasError())
            return res.error();
    }
    return res.value()->asInt();
}

inline sgrn::Result<bool, std::string> requireBool(const Json::Value& t_parent, const std::string& t_key) {
    auto res = requireMember(t_parent, t_key, Json::booleanValue);
    if (res.hasError())
        return res.error();
    return res.value()->asBool();
}

inline sgrn::Result<double, std::string> requireDouble(const Json::Value& t_parent, const std::string& t_key) {
    auto res = requireMember(t_parent, t_key, Json::realValue);
    if (res.hasError()) {
        // Fallback for int/uint
        if (t_parent.isMember(t_key) && t_parent[t_key].isNumeric())
            return t_parent[t_key].asDouble();
        return res.error();
    }
    return res.value()->asDouble();
}

/**
 * @brief Returns the member if it exists and is an object, or an empty object otherwise.
 */
inline Json::Value objectOrEmpty(const Json::Value& t_parent, const std::string& t_key) {
    if (!t_parent.isMember(t_key) || t_parent[t_key].isNull())
        return Json::Value(Json::objectValue);
    if (!t_parent[t_key].isObject())
        return Json::Value(Json::objectValue);
    return t_parent[t_key];
}

} // namespace sgrn::utils::json
