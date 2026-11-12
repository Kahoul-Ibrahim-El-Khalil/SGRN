#pragma once

#include <sgrn/utils/json-dom.hpp>

#include <fmt/core.h>
#include <sgrn/Result.hpp>
#include <sgrn/utils/strings.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sgrn::utils::json
{

namespace fs = std::filesystem;

using Document = ::sgrn::json::dom::Document;
using Value = ::sgrn::json::dom::Value;
using Allocator = ::sgrn::json::dom::Document::AllocatorType;

namespace detail
{

inline std::string parseErrorString(const ::sgrn::json::dom::Document& t_doc) {
    return t_doc.errorMessage();
}

inline bool valueIsInteger(const Value& t_value) {
    if (t_value.isInt() || t_value.isInt64() || t_value.isUint() || t_value.isUint64()) {
        return true;
    }
    if (t_value.isDouble()) {
        const double n = t_value.getDouble();
        return n == std::trunc(n);
    }
    return false;
}

inline bool valueIsNumeric(const Value& t_value) {
    return t_value.is_number();
}

inline void walkFlatten(const Value& t_val, std::string& t_prefix, std::string_view t_sep, Value& t_flat, Allocator& t_alloc) {
    if (t_val.is_object()) {
        for (auto it = t_val.memberBegin(); it != t_val.memberEnd(); ++it) {
            const auto old_len = t_prefix.size();
            t_prefix.append(it->name.getString(), it->name.getStringLength());
            t_prefix.append(t_sep);
            walkFlatten(it->value, t_prefix, t_sep, t_flat, t_alloc);
            t_prefix.resize(old_len);
        }
    } else {
        const std::string_view final_key(t_prefix.data(), t_prefix.size() - t_sep.size());
        ::sgrn::json::dom::Value tp_key(final_key.data(), static_cast<::sgrn::json::dom::SizeType>(final_key.size()), t_alloc);
        if (t_val.is_array()) {
            ::sgrn::json::dom::StringBuffer sb;
            ::sgrn::json::dom::Writer<::sgrn::json::dom::StringBuffer> writer(sb);
            t_val.accept(writer);
            t_flat.addMember(tp_key, ::sgrn::json::dom::Value(sb.getString(), sb.size(), t_alloc), t_alloc);
        } else {
            ::sgrn::json::dom::Value copy;
            copy.copyFrom(t_val, t_alloc);
            t_flat.addMember(tp_key, copy, t_alloc);
        }
    }
}

inline void walkFlattenToString(const Value& t_val, std::string& t_prefix, std::string_view t_sep, char t_kv, std::string& t_out) {
    if (t_val.is_object()) {
        for (auto it = t_val.memberBegin(); it != t_val.memberEnd(); ++it) {
            const auto old_len = t_prefix.size();
            t_prefix.append(it->name.getString(), it->name.getStringLength());
            t_prefix.append(t_sep);
            walkFlattenToString(it->value, t_prefix, t_sep, t_kv, t_out);
            t_prefix.resize(old_len);
        }
    } else {
        t_out.append(t_prefix.data(), t_prefix.size() - t_sep.size());
        t_out.push_back(t_kv);
        if (t_val.is_array()) {
            ::sgrn::json::dom::StringBuffer sb;
            ::sgrn::json::dom::Writer<::sgrn::json::dom::StringBuffer> writer(sb);
            t_val.accept(writer);
            t_out.append(sb.getString(), sb.size());
        } else if (t_val.is_string()) {
            t_out.append(t_val.getString(), t_val.getStringLength());
        } else if (t_val.is_boolean()) {
            t_out.append(t_val.getBool() ? "true" : "false");
        } else if (t_val.is_number()) {
            ::sgrn::json::dom::StringBuffer sb;
            ::sgrn::json::dom::Writer<::sgrn::json::dom::StringBuffer> writer(sb);
            t_val.accept(writer);
            t_out.append(sb.getString(), sb.size());
        } else if (t_val.is_null()) {
            t_out.append("null");
        }
        t_out.push_back('\n');
    }
}

inline ::sgrn::json::dom::Type expectedGlazeJsonType(const char* tp_type_name) {
    if (std::string_view(tp_type_name) == "null")
        return ::sgrn::json::dom::Type::Null;
    if (std::string_view(tp_type_name) == "boolean")
        return ::sgrn::json::dom::Type::True;
    if (std::string_view(tp_type_name) == "integer" || std::string_view(tp_type_name) == "number")
        return ::sgrn::json::dom::Type::Number;
    if (std::string_view(tp_type_name) == "string")
        return ::sgrn::json::dom::Type::String;
    if (std::string_view(tp_type_name) == "array")
        return ::sgrn::json::dom::Type::Array;
    if (std::string_view(tp_type_name) == "object")
        return ::sgrn::json::dom::Type::Object;
    return ::sgrn::json::dom::Type::Null;
}

inline bool matchesExpectedType(const Value& t_value, const char* tp_type_name) {
    const auto expected = expectedGlazeJsonType(tp_type_name);
    if (expected == ::sgrn::json::dom::Type::True) {
        return t_value.is_boolean();
    }
    if (expected == ::sgrn::json::dom::Type::Number) {
        if (std::string_view(tp_type_name) == "integer") {
            return valueIsInteger(t_value);
        }
        return valueIsNumeric(t_value);
    }
    if (t_value.type() == expected) {
        return true;
    }
    return false;
}

} // namespace detail

inline sgrn::Result<Document, std::string> deserialize(std::string_view t_json_str, bool /*t_collect_comments*/ = false) {
    Document t_doc;
    // Match jsoncpp CharReaderBuilder defaults: allow // and /* */ comments, trailing commas.
    constexpr unsigned parse_compat = ::sgrn::json::dom::kParseCommentsFlag | ::sgrn::json::dom::kParseTrailingCommasFlag;
    t_doc.parse<parse_compat>(t_json_str.data(), t_json_str.size());
    if (t_doc.hasParseError()) {
        return fmt::format("Failed to parse JSON: {} (offset {}", detail::parseErrorString(t_doc), t_doc.errorOffset());
    }
    return t_doc;
}

inline sgrn::Result<Document, std::string> deserialize(const std::string& t_json_str, bool t_collect_comments = false) {
    return deserialize(std::string_view(t_json_str), t_collect_comments);
}

inline sgrn::Result<Document, std::string> deserializeFromFile(const fs::path& t_path) {
    std::ifstream ifs(t_path, std::ios::binary);
    if (!ifs.is_open()) {
        return fmt::format("Failed to open JSON file: {}", t_path.string());
    }
    const std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return deserialize(content);
}

inline std::string serialize(const Value& t_json_value, bool t_pretty = false) {
    ::sgrn::json::dom::StringBuffer buffer;
    if (t_pretty) {
        ::sgrn::json::dom::PrettyWriter<::sgrn::json::dom::StringBuffer> writer(buffer);
        t_json_value.accept(writer);
    } else {
        ::sgrn::json::dom::Writer<::sgrn::json::dom::StringBuffer> writer(buffer);
        t_json_value.accept(writer);
    }
    return std::string(buffer.getString(), buffer.size());
}

inline std::string serializeCompact(const Value& t_json_value) {
    return serialize(t_json_value, false);
}

inline std::string serializePretty(const Value& t_json_value) {
    return serialize(t_json_value, true);
}

inline std::string toString(const Value& t_json_value) {
    return serializePretty(t_json_value);
}

inline bool serializeToFile(const fs::path& t_output_path, const Value& t_json_value, bool t_pretty = false) {
    std::ofstream file(t_output_path.string());
    if (!file.is_open()) {
        return false;
    }
    file << serialize(t_json_value, t_pretty) << '\n';
    return file.good();
}

template <size_t N>
inline sgrn::Result<void, std::string> filterJson(Value& t_root_json, const std::array<std::string_view, N>& t_allowed_keys) {
    if (!t_root_json.is_object()) {
        return {};
    }

    std::vector<std::string> to_remove;
    for (auto it = t_root_json.memberBegin(); it != t_root_json.memberEnd(); ++it) {
        const std::string_view tp_key(it->name.getString(), it->name.getStringLength());
        const auto found = std::find(t_allowed_keys.begin(), t_allowed_keys.end(), tp_key);
        if (found == t_allowed_keys.end()) {
            to_remove.emplace_back(it->name.getString(), it->name.getStringLength());
        }
    }

    for (const auto& tp_key : to_remove) {
        t_root_json.removeMember(tp_key.c_str());
    }
    return {};
}

inline void flattenJsonInplace(Value& t_json, std::string_view t_sep, Allocator& t_alloc) {
    Value t_flat(::sgrn::json::dom::Type::Object);
    std::string t_prefix;
    t_prefix.reserve(128);
    detail::walkFlatten(t_json, t_prefix, t_sep, t_flat, t_alloc);
    t_json.swap(t_flat);
}

inline void flattenJsonInplace(Document& t_json, std::string_view t_sep) {
    flattenJsonInplace(t_json, t_sep, t_json.allocator());
}

inline void applyScope(Value& t_json, std::string_view t_scope, std::string_view t_scope_sep, Allocator& t_alloc) {
    if (t_scope_sep.empty() || !t_json.is_object()) {
        return;
    }

    Value scoped(::sgrn::json::dom::Type::Object);
    for (auto it = t_json.memberBegin(); it != t_json.memberEnd(); ++it) {
        std::string new_key;
        new_key.reserve(t_scope.size() + t_scope_sep.size() + it->name.getStringLength());
        new_key.append(t_scope);
        new_key.append(t_scope_sep);
        new_key.append(it->name.getString(), it->name.getStringLength());

        ::sgrn::json::dom::Value tp_key(new_key.c_str(), static_cast<::sgrn::json::dom::SizeType>(new_key.size()), t_alloc);
        ::sgrn::json::dom::Value t_val;
        t_val.copyFrom(it->value, t_alloc);
        scoped.addMember(tp_key, t_val, t_alloc);
    }
    t_json.swap(scoped);
}

inline void applyScope(Document& t_json, std::string_view t_scope, std::string_view t_scope_sep) {
    applyScope(t_json, t_scope, t_scope_sep, t_json.allocator());
}

inline std::string flattenJsonToString(const Value& t_json, std::string_view t_sep, char t_kv = '=') {
    std::string t_out;
    std::string t_prefix;
    t_prefix.reserve(128);
    detail::walkFlattenToString(t_json, t_prefix, t_sep, t_kv, t_out);
    return t_out;
}

inline Value buildDelta(const Value& t_prev, const Value& t_cur, Allocator& t_alloc) {
    if (t_cur.is_object()) {
        Value t_delta(::sgrn::json::dom::Type::Object);
        for (auto it = t_cur.memberBegin(); it != t_cur.memberEnd(); ++it) {
            const char* p_ey = it->name.getString();
            if (!t_prev.is_object() || !t_prev.hasMember(p_ey)) {
                ::sgrn::json::dom::Value copy;
                copy.copyFrom(it->value, t_alloc);
                t_delta.addMember(::sgrn::json::dom::stringRef(p_ey), copy, t_alloc);
            } else {
                Value sub_delta = buildDelta(t_prev[p_ey], it->value, t_alloc);
                if (!sub_delta.is_null()) {
                    t_delta.addMember(::sgrn::json::dom::stringRef(p_ey), sub_delta, t_alloc);
                }
            }
        }
        if (t_delta.objectEmpty()) {
            Value empty;
            empty.setNull();
            return empty;
        }
        return t_delta;
    }
    if (!(t_cur == t_prev)) {
        Value changed;
        changed.copyFrom(t_cur, t_alloc);
        return changed;
    }
    Value empty;
    empty.setNull();
    return empty;
}

inline Document buildDelta(const Value& t_prev, const Value& t_cur) {
    Document t_delta;
    Value built = buildDelta(t_prev, t_cur, t_delta.allocator());
    t_delta.swap(built);
    return t_delta;
}

inline void applyDelta(Value& t_base, const Value& t_delta, Allocator& t_alloc) {
    if (!t_delta.is_object()) {
        t_base.copyFrom(t_delta, t_alloc);
        return;
    }
    if (!t_base.is_object()) {
        t_base.setObject();
    }
    for (auto it = t_delta.memberBegin(); it != t_delta.memberEnd(); ++it) {
        const char* p_ey = it->name.getString();
        if (!t_base.hasMember(p_ey)) {
            ::sgrn::json::dom::Value k(it->name, t_alloc);
            ::sgrn::json::dom::Value v;
            v.copyFrom(it->value, t_alloc);
            t_base.addMember(k, v, t_alloc);
        } else {
            auto child = t_base[p_ey];
            applyDelta(child, it->value, t_alloc);
        }
    }
}

inline void applyDelta(Document& t_base, const Value& t_delta) {
    applyDelta(t_base, t_delta, t_base.allocator());
}

inline sgrn::Result<Document, std::string> mergeWithSchemaCheck(
    const Value& t_a, const Value& t_b, Allocator& t_alloc, const std::string& t_path = "root") {
    if (t_a.is_null()) {
        Document t_out;
        t_out.copyFrom(t_b, t_alloc);
        return t_out;
    }
    if (t_b.is_null()) {
        Document t_out;
        t_out.copyFrom(t_a, t_alloc);
        return t_out;
    }

    if (t_a.is_object() && t_b.is_object()) {
        Document result;
        result.copyFrom(t_a, t_alloc);
        for (auto it = t_b.memberBegin(); it != t_b.memberEnd(); ++it) {
            const char* p_ey = it->name.getString();
            if (result.hasMember(p_ey)) {
                auto merged = mergeWithSchemaCheck(result[p_ey], it->value, t_alloc, t_path + "." + p_ey);
                PROPAGATE_ERROR_IF(merged);
                result[p_ey].copyFrom(merged.value(), t_alloc);
            } else {
                ::sgrn::json::dom::Value k(it->name, t_alloc);
                ::sgrn::json::dom::Value v;
                v.copyFrom(it->value, t_alloc);
                result.addMember(k, v, t_alloc);
            }
        }
        return result;
    }

    if (t_a.type() != t_b.type() && !(t_a.is_number() && t_b.is_number())) {
        return fmt::format(
            "Schema conflict at {}: base is {}, incoming is {}", t_path, static_cast<int>(t_a.type()), static_cast<int>(t_b.type()));
    }

    Document t_out;
    t_out.copyFrom(t_b, t_alloc);
    return t_out;
}

inline sgrn::Result<Document, std::string> mergeJsonDeltas(const fs::path& t_base_tree_path, std::span<const fs::path> t_deltas_paths) {
    if (!fs::exists(t_base_tree_path)) {
        return fmt::format("Path to JSON base tree: '{}' does not exist", t_base_tree_path.string());
    }

    auto baseline_res = deserializeFromFile(t_base_tree_path);
    PROPAGATE_ERROR_IF(baseline_res);

    Document result = std::move(baseline_res.value());

    for (const auto& t_path : t_deltas_paths) {
        auto delta_res = deserializeFromFile(t_path);
        PROPAGATE_ERROR_IF(delta_res);
        applyDelta(result, delta_res.value());
    }

    return result;
}

inline Document computeResultantTree(std::span<const Value> t_steps, Allocator& t_alloc) {
    if (t_steps.empty()) {
        Document empty;
        empty.setObject();
        return empty;
    }

    Document result;
    result.copyFrom(t_steps[0], t_alloc);
    for (size_t i = 1; i < t_steps.size(); ++i) {
        applyDelta(result, t_steps[i]);
    }
    return result;
}

inline Document computeResultantTree(std::span<const Document> t_steps) {
    if (t_steps.empty()) {
        Document empty;
        empty.setObject();
        return empty;
    }

    Document result;
    result.copyFrom(t_steps[0], result.allocator());
    for (size_t i = 1; i < t_steps.size(); ++i) {
        applyDelta(result, t_steps[i]);
    }
    return result;
}

inline sgrn::Result<Document, std::string> computeResultantTree(std::span<const fs::path> t_paths) {
    if (t_paths.empty()) {
        Document empty;
        empty.setObject();
        return empty;
    }
    return mergeJsonDeltas(t_paths[0], t_paths.subspan(1));
}

inline sgrn::Result<void, std::string> serializeAndWriteToFile(const fs::path& t_path, const Value& t_json) {
    if (serializeToFile(t_path, t_json, true)) {
        return {};
    }
    return fmt::format("Failed to open file for writing: {}", t_path.string());
}

inline sgrn::Result<Document, std::string> loadJsonFile(const fs::path& t_path) {
    return deserializeFromFile(t_path);
}

inline std::optional<int64_t> toSignedInteger(const Value& t_value) {
    if (t_value.isInt64()) {
        return t_value.getInt64();
    }
    if (t_value.isInt()) {
        return static_cast<int64_t>(t_value.getInt());
    }
    if (t_value.isUint64()) {
        const uint64_t unsigned_value = t_value.getUint64();
        if (unsigned_value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return static_cast<int64_t>(unsigned_value);
        }
        return std::nullopt;
    }
    if (t_value.isUint()) {
        return static_cast<int64_t>(t_value.getUint());
    }
    if (t_value.isDouble()) {
        return static_cast<int64_t>(t_value.getDouble());
    }
    if (t_value.is_string()) {
        if (std::optional<int64_t> parsed = sgrn::utils::strings::parseInt64(t_value.getString())) {
            return parsed.value();
        }
    }
    return std::nullopt;
}

inline std::optional<uint64_t> toUnsignedInteger(const Value& t_value) {
    if (t_value.isUint64()) {
        return t_value.getUint64();
    }
    if (t_value.isUint()) {
        return static_cast<uint64_t>(t_value.getUint());
    }
    if (t_value.isInt64()) {
        const int64_t signed_value = t_value.getInt64();
        if (signed_value >= 0) {
            return static_cast<uint64_t>(signed_value);
        }
        return std::nullopt;
    }
    if (t_value.isInt()) {
        const int32_t signed_value = t_value.getInt();
        if (signed_value >= 0) {
            return static_cast<uint64_t>(signed_value);
        }
        return std::nullopt;
    }
    if (t_value.isDouble()) {
        const double numeric = t_value.getDouble();
        if (numeric >= 0.0) {
            return static_cast<uint64_t>(numeric);
        }
        return std::nullopt;
    }
    if (t_value.is_string()) {
        if (std::optional<uint64_t> parsed = sgrn::utils::strings::parseUInt64(t_value.getString())) {
            return parsed.value();
        }
    }
    return std::nullopt;
}

inline std::optional<double> toFloatingPoint(const Value& t_value) {
    if (t_value.is_number()) {
        return t_value.getDouble();
    }
    if (t_value.is_string()) {
        if (std::optional<double> parsed = sgrn::utils::strings::parseDouble(t_value.getString())) {
            return parsed.value();
        }
    }
    return std::nullopt;
}

inline void insertNestedValue(Value& t_root, const std::string& t_path, const Value& t_value, Allocator& t_alloc, char t_sep = '.') {
    std::size_t start = 0;
    Value* p_cursor = &t_root;
    Value cursor_storage;
    while (start < t_path.size()) {
        const std::size_t sep_pos = t_path.find(t_sep, start);
        const std::string segment = sep_pos == std::string::npos ? t_path.substr(start) : t_path.substr(start, sep_pos - start);
        if (segment.empty()) {
            return;
        }
        if (sep_pos == std::string::npos) {
            if (!p_cursor->is_object()) {
                p_cursor->setObject();
            }
            ::sgrn::json::dom::Value tp_key(segment.c_str(), static_cast<::sgrn::json::dom::SizeType>(segment.size()), t_alloc);
            ::sgrn::json::dom::Value t_val;
            t_val.copyFrom(t_value, t_alloc);
            if (p_cursor->hasMember(segment.c_str())) {
                (*p_cursor)[segment.c_str()].copyFrom(t_value, t_alloc);
            } else {
                p_cursor->addMember(tp_key, t_val, t_alloc);
            }
            return;
        }
        if (!p_cursor->is_object()) {
            p_cursor->setObject();
        }
        if (!p_cursor->hasMember(segment.c_str())) {
            ::sgrn::json::dom::Value tp_key(segment.c_str(), static_cast<::sgrn::json::dom::SizeType>(segment.size()), t_alloc);
            ::sgrn::json::dom::Value obj(::sgrn::json::dom::Type::Object);
            p_cursor->addMember(tp_key, obj, t_alloc);
        }
        auto child = (*p_cursor)[segment.c_str()];
        cursor_storage.rebindTo(child);
        p_cursor = &cursor_storage;
        start = sep_pos + 1;
    }
}

inline void insertNestedValue(Document& t_root, const std::string& t_path, const Value& t_value, char t_sep = '.') {
    insertNestedValue(t_root, t_path, t_value, t_root.allocator(), t_sep);
}

inline Value fromString(const std::string& t_val, Allocator& t_alloc) {
    if (t_val == "true") {
        Value v(true);
        return v;
    }
    if (t_val == "false") {
        Value v(false);
        return v;
    }
    if (t_val == "null") {
        Value v;
        v.setNull();
        return v;
    }

    try {
        size_t pos = 0;
        const double d = std::stod(t_val, &pos);
        if (pos == t_val.size()) {
            if (t_val.find('.') == std::string::npos && t_val.find('e') == std::string::npos && t_val.find('E') == std::string::npos) {
                try {
                    Value v;
                    v.setInt64(std::stoll(t_val));
                    return v;
                } catch (...) {
                    // safe to discard: if stoll fails, we fallback to stod or string
                }
            }
            Value v;
            v.setDouble(d);
            return v;
        }
    } catch (...) {
        // safe to discard: if stod fails, we fallback to string
    }

    return Value(t_val.c_str(), static_cast<::sgrn::json::dom::SizeType>(t_val.size()), t_alloc);
}

inline std::string typeName(::sgrn::json::dom::Type t_type) {
    switch (t_type) {
        case ::sgrn::json::dom::Type::Null:
            return "null";
        case ::sgrn::json::dom::Type::False:
        case ::sgrn::json::dom::Type::True:
            return "boolean";
        case ::sgrn::json::dom::Type::Number:
            return "number";
        case ::sgrn::json::dom::Type::String:
            return "string";
        case ::sgrn::json::dom::Type::Array:
            return "array";
        case ::sgrn::json::dom::Type::Object:
            return "object";
    }
    return "unknown";
}

inline sgrn::Result<Value, std::string> requireMember(const Value& t_parent, const char* tp_key, const char* tp_type_name) {
    if (!t_parent.is_object() || !t_parent.hasMember(tp_key)) {
        return sgrn::Result<Value, std::string>::Error(fmt::format("missing '{}'", tp_key));
    }
    Value value = Value::view(t_parent.rawValue().get_object().at(tp_key));
    if (!detail::matchesExpectedType(value, tp_type_name)) {
        return sgrn::Result<Value, std::string>::Error(fmt::format("'{}' must be {}", tp_key, tp_type_name));
    }
    return value;
}

inline sgrn::Result<Value, std::string> requireMember(const Value& t_parent, const std::string& t_key, const char* tp_type_name) {
    return requireMember(t_parent, t_key.c_str(), tp_type_name);
}

inline sgrn::Result<std::string, std::string> requireString(const Value& t_parent, const std::string& t_key) {
    auto res = requireMember(t_parent, t_key, "string");
    PROPAGATE_ERROR_IF(res);
    return std::string(res.value().getString(), res.value().getStringLength());
}

inline sgrn::Result<int, std::string> requireInt(const Value& t_parent, const std::string& t_key) {
    auto res = requireMember(t_parent, t_key, "integer");
    PROPAGATE_ERROR_IF(res);
    if (res.value().isInt()) {
        return res.value().getInt();
    }
    if (res.value().isInt64()) {
        return static_cast<int>(res.value().getInt64());
    }
    if (res.value().isUint()) {
        return static_cast<int>(res.value().getUint());
    }
    if (res.value().isUint64()) {
        return static_cast<int>(res.value().getUint64());
    }
    if (res.value().isDouble()) {
        return static_cast<int>(res.value().getDouble());
    }
    return fmt::format("'{}' must be integer", t_key);
}

inline sgrn::Result<bool, std::string> requireBool(const Value& t_parent, const std::string& t_key) {
    auto res = requireMember(t_parent, t_key, "boolean");
    PROPAGATE_ERROR_IF(res);
    return res.value().getBool();
}

inline sgrn::Result<double, std::string> requireDouble(const Value& t_parent, const std::string& t_key) {
    if (!t_parent.is_object() || !t_parent.hasMember(t_key.c_str())) {
        return fmt::format("missing '{}'", t_key);
    }
    const Value& p_value = t_parent[t_key.c_str()];
    if (detail::valueIsNumeric(p_value)) {
        return p_value.getDouble();
    }
    return fmt::format("'{}' must be number", t_key);
}

inline Value objectOrEmpty(const Value& t_parent, const char* tp_key, Allocator& t_alloc) {
    if (!t_parent.is_object() || !t_parent.hasMember(tp_key) || t_parent[tp_key].is_null()) {
        Value empty(::sgrn::json::dom::Type::Object);
        return empty;
    }
    if (!t_parent[tp_key].is_object()) {
        Value empty(::sgrn::json::dom::Type::Object);
        return empty;
    }
    Value copy;
    copy.copyFrom(t_parent[tp_key], t_alloc);
    return copy;
}

inline Document objectOrEmpty(const Value& t_parent, const std::string& t_key) {
    Document t_doc;
    Value built = objectOrEmpty(t_parent, t_key.c_str(), t_doc.allocator());
    t_doc.swap(built);
    return t_doc;
}

inline bool isNull(const Value& t_value) {
    return t_value.is_null();
}

inline bool isNull(const Document& t_value) {
    return t_value.is_null();
}

} // namespace sgrn::utils::json
