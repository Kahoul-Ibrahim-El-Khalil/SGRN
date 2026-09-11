#pragma once

#include <fmt/core.h>
#include <sgrn/Result.hpp>
#include <sgrn/utils/strings.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace sgrn::utils::json
{

namespace fs = std::filesystem;

using Document = ::rapidjson::Document;
using Value = ::rapidjson::Value;
using Allocator = ::rapidjson::Document::AllocatorType;

namespace detail
{

inline std::string parseErrorString(const ::rapidjson::Document& t_doc) {
    return ::rapidjson::GetParseError_En(t_doc.GetParseError());
}

inline bool valueIsInteger(const Value& t_value) {
    if (t_value.IsInt() || t_value.IsInt64() || t_value.IsUint() || t_value.IsUint64()) {
        return true;
    }
    if (t_value.IsDouble()) {
        const double n = t_value.GetDouble();
        return n == std::trunc(n);
    }
    return false;
}

inline bool valueIsNumeric(const Value& t_value) {
    return t_value.IsNumber();
}

inline void walkFlatten(const Value& t_val, std::string& t_prefix, std::string_view t_sep, Value& t_flat, Allocator& t_alloc) {
    if (t_val.IsObject()) {
        for (auto it = t_val.MemberBegin(); it != t_val.MemberEnd(); ++it) {
            const auto old_len = t_prefix.size();
            t_prefix.append(it->name.GetString(), it->name.GetStringLength());
            t_prefix.append(t_sep);
            walkFlatten(it->value, t_prefix, t_sep, t_flat, t_alloc);
            t_prefix.resize(old_len);
        }
    } else {
        const std::string_view final_key(t_prefix.data(), t_prefix.size() - t_sep.size());
        ::rapidjson::Value tp_key(final_key.data(), static_cast<::rapidjson::SizeType>(final_key.size()), t_alloc);
        if (t_val.IsArray()) {
            ::rapidjson::StringBuffer sb;
            ::rapidjson::Writer<::rapidjson::StringBuffer> writer(sb);
            t_val.Accept(writer);
            t_flat.AddMember(tp_key, ::rapidjson::Value(sb.GetString(), sb.GetSize(), t_alloc), t_alloc);
        } else {
            ::rapidjson::Value copy;
            copy.CopyFrom(t_val, t_alloc);
            t_flat.AddMember(tp_key, copy, t_alloc);
        }
    }
}

inline void walkFlattenToString(const Value& t_val, std::string& t_prefix, std::string_view t_sep, char t_kv, std::string& t_out) {
    if (t_val.IsObject()) {
        for (auto it = t_val.MemberBegin(); it != t_val.MemberEnd(); ++it) {
            const auto old_len = t_prefix.size();
            t_prefix.append(it->name.GetString(), it->name.GetStringLength());
            t_prefix.append(t_sep);
            walkFlattenToString(it->value, t_prefix, t_sep, t_kv, t_out);
            t_prefix.resize(old_len);
        }
    } else {
        t_out.append(t_prefix.data(), t_prefix.size() - t_sep.size());
        t_out.push_back(t_kv);
        if (t_val.IsArray()) {
            ::rapidjson::StringBuffer sb;
            ::rapidjson::Writer<::rapidjson::StringBuffer> writer(sb);
            t_val.Accept(writer);
            t_out.append(sb.GetString(), sb.GetSize());
        } else if (t_val.IsString()) {
            t_out.append(t_val.GetString(), t_val.GetStringLength());
        } else if (t_val.IsBool()) {
            t_out.append(t_val.GetBool() ? "true" : "false");
        } else if (t_val.IsNumber()) {
            ::rapidjson::StringBuffer sb;
            ::rapidjson::Writer<::rapidjson::StringBuffer> writer(sb);
            t_val.Accept(writer);
            t_out.append(sb.GetString(), sb.GetSize());
        } else if (t_val.IsNull()) {
            t_out.append("null");
        }
        t_out.push_back('\n');
    }
}

inline ::rapidjson::Type expectedRapidJsonType(const char* tp_type_name) {
    if (std::string_view(tp_type_name) == "null")
        return ::rapidjson::kNullType;
    if (std::string_view(tp_type_name) == "boolean")
        return ::rapidjson::kTrueType;
    if (std::string_view(tp_type_name) == "integer" || std::string_view(tp_type_name) == "number")
        return ::rapidjson::kNumberType;
    if (std::string_view(tp_type_name) == "string")
        return ::rapidjson::kStringType;
    if (std::string_view(tp_type_name) == "array")
        return ::rapidjson::kArrayType;
    if (std::string_view(tp_type_name) == "object")
        return ::rapidjson::kObjectType;
    return ::rapidjson::kNullType;
}

inline bool matchesExpectedType(const Value& t_value, const char* tp_type_name) {
    const auto expected = expectedRapidJsonType(tp_type_name);
    if (expected == ::rapidjson::kTrueType) {
        return t_value.IsBool();
    }
    if (expected == ::rapidjson::kNumberType) {
        if (std::string_view(tp_type_name) == "integer") {
            return valueIsInteger(t_value);
        }
        return valueIsNumeric(t_value);
    }
    if (t_value.GetType() == expected) {
        return true;
    }
    return false;
}

} // namespace detail

inline sgrn::Result<Document, std::string> deserialize(std::string_view t_json_str, bool /*t_collect_comments*/ = false) {
    Document t_doc;
    // Match jsoncpp CharReaderBuilder defaults: allow // and /* */ comments, trailing commas.
    constexpr unsigned parse_compat = ::rapidjson::kParseCommentsFlag | ::rapidjson::kParseTrailingCommasFlag;
    t_doc.Parse<parse_compat>(t_json_str.data(), t_json_str.size());
    if (t_doc.HasParseError()) {
        return fmt::format("Failed to parse JSON: {} (offset {}", detail::parseErrorString(t_doc), t_doc.GetErrorOffset());
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
    ::rapidjson::StringBuffer buffer;
    if (t_pretty) {
        ::rapidjson::PrettyWriter<::rapidjson::StringBuffer> writer(buffer);
        t_json_value.Accept(writer);
    } else {
        ::rapidjson::Writer<::rapidjson::StringBuffer> writer(buffer);
        t_json_value.Accept(writer);
    }
    return std::string(buffer.GetString(), buffer.GetSize());
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
    if (!t_root_json.IsObject()) {
        return {};
    }

    std::vector<std::string> to_remove;
    for (auto it = t_root_json.MemberBegin(); it != t_root_json.MemberEnd(); ++it) {
        const std::string_view tp_key(it->name.GetString(), it->name.GetStringLength());
        const auto found = std::find(t_allowed_keys.begin(), t_allowed_keys.end(), tp_key);
        if (found == t_allowed_keys.end()) {
            to_remove.emplace_back(it->name.GetString(), it->name.GetStringLength());
        }
    }

    for (const auto& tp_key : to_remove) {
        t_root_json.RemoveMember(tp_key.c_str());
    }
    return {};
}

inline void flattenJsonInplace(Value& t_json, std::string_view t_sep, Allocator& t_alloc) {
    Value t_flat(::rapidjson::kObjectType);
    std::string t_prefix;
    t_prefix.reserve(128);
    detail::walkFlatten(t_json, t_prefix, t_sep, t_flat, t_alloc);
    t_json.Swap(t_flat);
}

inline void flattenJsonInplace(Document& t_json, std::string_view t_sep) {
    flattenJsonInplace(t_json, t_sep, t_json.GetAllocator());
}

inline void applyScope(Value& t_json, std::string_view t_scope, std::string_view t_scope_sep, Allocator& t_alloc) {
    if (t_scope_sep.empty() || !t_json.IsObject()) {
        return;
    }

    Value scoped(::rapidjson::kObjectType);
    for (auto it = t_json.MemberBegin(); it != t_json.MemberEnd(); ++it) {
        std::string new_key;
        new_key.reserve(t_scope.size() + t_scope_sep.size() + it->name.GetStringLength());
        new_key.append(t_scope);
        new_key.append(t_scope_sep);
        new_key.append(it->name.GetString(), it->name.GetStringLength());

        ::rapidjson::Value tp_key(new_key.c_str(), static_cast<::rapidjson::SizeType>(new_key.size()), t_alloc);
        ::rapidjson::Value t_val;
        t_val.CopyFrom(it->value, t_alloc);
        scoped.AddMember(tp_key, t_val, t_alloc);
    }
    t_json.Swap(scoped);
}

inline void applyScope(Document& t_json, std::string_view t_scope, std::string_view t_scope_sep) {
    applyScope(t_json, t_scope, t_scope_sep, t_json.GetAllocator());
}

inline std::string flattenJsonToString(const Value& t_json, std::string_view t_sep, char t_kv = '=') {
    std::string t_out;
    std::string t_prefix;
    t_prefix.reserve(128);
    detail::walkFlattenToString(t_json, t_prefix, t_sep, t_kv, t_out);
    return t_out;
}

inline Value buildDelta(const Value& t_prev, const Value& t_cur, Allocator& t_alloc) {
    if (t_cur.IsObject()) {
        Value t_delta(::rapidjson::kObjectType);
        for (auto it = t_cur.MemberBegin(); it != t_cur.MemberEnd(); ++it) {
            const char* p_ey = it->name.GetString();
            if (!t_prev.IsObject() || !t_prev.HasMember(p_ey)) {
                ::rapidjson::Value copy;
                copy.CopyFrom(it->value, t_alloc);
                t_delta.AddMember(::rapidjson::StringRef(p_ey), copy, t_alloc);
            } else {
                Value sub_delta = buildDelta(t_prev[p_ey], it->value, t_alloc);
                if (!sub_delta.IsNull()) {
                    t_delta.AddMember(::rapidjson::StringRef(p_ey), sub_delta, t_alloc);
                }
            }
        }
        if (t_delta.ObjectEmpty()) {
            Value empty;
            empty.SetNull();
            return empty;
        }
        return t_delta;
    }
    if (!(t_cur == t_prev)) {
        Value changed;
        changed.CopyFrom(t_cur, t_alloc);
        return changed;
    }
    Value empty;
    empty.SetNull();
    return empty;
}

inline Document buildDelta(const Value& t_prev, const Value& t_cur) {
    Document t_delta;
    Value built = buildDelta(t_prev, t_cur, t_delta.GetAllocator());
    t_delta.Swap(built);
    return t_delta;
}

inline void applyDelta(Value& t_base, const Value& t_delta, Allocator& t_alloc) {
    if (!t_delta.IsObject()) {
        t_base.CopyFrom(t_delta, t_alloc);
        return;
    }
    if (!t_base.IsObject()) {
        t_base.SetObject();
    }
    for (auto it = t_delta.MemberBegin(); it != t_delta.MemberEnd(); ++it) {
        const char* p_ey = it->name.GetString();
        if (!t_base.HasMember(p_ey)) {
            ::rapidjson::Value k(it->name, t_alloc);
            ::rapidjson::Value v;
            v.CopyFrom(it->value, t_alloc);
            t_base.AddMember(k, v, t_alloc);
        } else {
            applyDelta(t_base[p_ey], it->value, t_alloc);
        }
    }
}

inline void applyDelta(Document& t_base, const Value& t_delta) {
    applyDelta(t_base, t_delta, t_base.GetAllocator());
}

inline sgrn::Result<Document, std::string> mergeWithSchemaCheck(
    const Value& t_a, const Value& t_b, Allocator& t_alloc, const std::string& t_path = "root") {
    if (t_a.IsNull()) {
        Document t_out;
        t_out.CopyFrom(t_b, t_alloc);
        return t_out;
    }
    if (t_b.IsNull()) {
        Document t_out;
        t_out.CopyFrom(t_a, t_alloc);
        return t_out;
    }

    if (t_a.IsObject() && t_b.IsObject()) {
        Document result;
        result.CopyFrom(t_a, t_alloc);
        for (auto it = t_b.MemberBegin(); it != t_b.MemberEnd(); ++it) {
            const char* p_ey = it->name.GetString();
            if (result.HasMember(p_ey)) {
                auto merged = mergeWithSchemaCheck(result[p_ey], it->value, t_alloc, t_path + "." + p_ey);
                if (merged.hasError()) {
                    return merged.error();
                }
                result[p_ey].CopyFrom(merged.value(), t_alloc);
            } else {
                ::rapidjson::Value k(it->name, t_alloc);
                ::rapidjson::Value v;
                v.CopyFrom(it->value, t_alloc);
                result.AddMember(k, v, t_alloc);
            }
        }
        return result;
    }

    if (t_a.GetType() != t_b.GetType() && !(t_a.IsNumber() && t_b.IsNumber())) {
        return fmt::format(
            "Schema conflict at {}: base is {}, incoming is {}", t_path, static_cast<int>(t_a.GetType()), static_cast<int>(t_b.GetType()));
    }

    Document t_out;
    t_out.CopyFrom(t_b, t_alloc);
    return t_out;
}

inline sgrn::Result<Document, std::string> mergeJsonDeltas(const fs::path& t_base_tree_path, std::span<const fs::path> t_deltas_paths) {
    if (!fs::exists(t_base_tree_path)) {
        return fmt::format("Path to JSON base tree: '{}' does not exist", t_base_tree_path.string());
    }

    auto baseline_res = deserializeFromFile(t_base_tree_path);
    if (baseline_res.hasError()) {
        return baseline_res.error();
    }

    Document result = std::move(baseline_res.value());

    for (const auto& t_path : t_deltas_paths) {
        auto delta_res = deserializeFromFile(t_path);
        if (delta_res.hasError()) {
            return delta_res.error();
        }
        applyDelta(result, delta_res.value());
    }

    return result;
}

inline Document computeResultantTree(std::span<const Value> t_steps, Allocator& t_alloc) {
    if (t_steps.empty()) {
        Document empty;
        empty.SetObject();
        return empty;
    }

    Document result;
    result.CopyFrom(t_steps[0], t_alloc);
    for (size_t i = 1; i < t_steps.size(); ++i) {
        applyDelta(result, t_steps[i]);
    }
    return result;
}

inline Document computeResultantTree(std::span<const Document> t_steps) {
    if (t_steps.empty()) {
        Document empty;
        empty.SetObject();
        return empty;
    }

    Document result;
    result.CopyFrom(t_steps[0], result.GetAllocator());
    for (size_t i = 1; i < t_steps.size(); ++i) {
        applyDelta(result, t_steps[i]);
    }
    return result;
}

inline sgrn::Result<Document, std::string> computeResultantTree(std::span<const fs::path> t_paths) {
    if (t_paths.empty()) {
        Document empty;
        empty.SetObject();
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
    if (t_value.IsInt64()) {
        return t_value.GetInt64();
    }
    if (t_value.IsInt()) {
        return static_cast<int64_t>(t_value.GetInt());
    }
    if (t_value.IsUint64()) {
        const uint64_t unsigned_value = t_value.GetUint64();
        if (unsigned_value <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return static_cast<int64_t>(unsigned_value);
        }
        return std::nullopt;
    }
    if (t_value.IsUint()) {
        return static_cast<int64_t>(t_value.GetUint());
    }
    if (t_value.IsDouble()) {
        return static_cast<int64_t>(t_value.GetDouble());
    }
    if (t_value.IsString()) {
        if (std::optional<int64_t> parsed = sgrn::utils::strings::parseInt64(t_value.GetString())) {
            return parsed.value();
        }
    }
    return std::nullopt;
}

inline std::optional<uint64_t> toUnsignedInteger(const Value& t_value) {
    if (t_value.IsUint64()) {
        return t_value.GetUint64();
    }
    if (t_value.IsUint()) {
        return static_cast<uint64_t>(t_value.GetUint());
    }
    if (t_value.IsInt64()) {
        const int64_t signed_value = t_value.GetInt64();
        if (signed_value >= 0) {
            return static_cast<uint64_t>(signed_value);
        }
        return std::nullopt;
    }
    if (t_value.IsInt()) {
        const int32_t signed_value = t_value.GetInt();
        if (signed_value >= 0) {
            return static_cast<uint64_t>(signed_value);
        }
        return std::nullopt;
    }
    if (t_value.IsDouble()) {
        const double numeric = t_value.GetDouble();
        if (numeric >= 0.0) {
            return static_cast<uint64_t>(numeric);
        }
        return std::nullopt;
    }
    if (t_value.IsString()) {
        if (std::optional<uint64_t> parsed = sgrn::utils::strings::parseUInt64(t_value.GetString())) {
            return parsed.value();
        }
    }
    return std::nullopt;
}

inline std::optional<double> toFloatingPoint(const Value& t_value) {
    if (t_value.IsNumber()) {
        return t_value.GetDouble();
    }
    if (t_value.IsString()) {
        if (std::optional<double> parsed = sgrn::utils::strings::parseDouble(t_value.GetString())) {
            return parsed.value();
        }
    }
    return std::nullopt;
}

inline void insertNestedValue(Value& t_root, const std::string& t_path, const Value& t_value, Allocator& t_alloc, char t_sep = '.') {
    std::size_t start = 0;
    Value* p_cursor = &t_root;
    while (start < t_path.size()) {
        const std::size_t sep_pos = t_path.find(t_sep, start);
        const std::string segment = sep_pos == std::string::npos ? t_path.substr(start) : t_path.substr(start, sep_pos - start);
        if (segment.empty()) {
            return;
        }
        if (sep_pos == std::string::npos) {
            if (!p_cursor->IsObject()) {
                p_cursor->SetObject();
            }
            ::rapidjson::Value tp_key(segment.c_str(), static_cast<::rapidjson::SizeType>(segment.size()), t_alloc);
            ::rapidjson::Value t_val;
            t_val.CopyFrom(t_value, t_alloc);
            if (p_cursor->HasMember(segment.c_str())) {
                (*p_cursor)[segment.c_str()].CopyFrom(t_value, t_alloc);
            } else {
                p_cursor->AddMember(tp_key, t_val, t_alloc);
            }
            return;
        }
        if (!p_cursor->IsObject()) {
            p_cursor->SetObject();
        }
        if (!p_cursor->HasMember(segment.c_str())) {
            ::rapidjson::Value tp_key(segment.c_str(), static_cast<::rapidjson::SizeType>(segment.size()), t_alloc);
            ::rapidjson::Value obj(::rapidjson::kObjectType);
            p_cursor->AddMember(tp_key, obj, t_alloc);
        }
        p_cursor = &((*p_cursor)[segment.c_str()]);
        start = sep_pos + 1;
    }
}

inline void insertNestedValue(Document& t_root, const std::string& t_path, const Value& t_value, char t_sep = '.') {
    insertNestedValue(t_root, t_path, t_value, t_root.GetAllocator(), t_sep);
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
        v.SetNull();
        return v;
    }

    try {
        size_t pos = 0;
        const double d = std::stod(t_val, &pos);
        if (pos == t_val.size()) {
            if (t_val.find('.') == std::string::npos && t_val.find('e') == std::string::npos && t_val.find('E') == std::string::npos) {
                try {
                    Value v;
                    v.SetInt64(std::stoll(t_val));
                    return v;
                } catch (...) {
                    // safe to discard: if stoll fails, we fallback to stod or string
                }
            }
            Value v;
            v.SetDouble(d);
            return v;
        }
    } catch (...) {
        // safe to discard: if stod fails, we fallback to string
    }

    return Value(t_val.c_str(), static_cast<::rapidjson::SizeType>(t_val.size()), t_alloc);
}

inline std::string typeName(::rapidjson::Type t_type) {
    switch (t_type) {
        case ::rapidjson::kNullType:
            return "null";
        case ::rapidjson::kFalseType:
        case ::rapidjson::kTrueType:
            return "boolean";
        case ::rapidjson::kNumberType:
            return "number";
        case ::rapidjson::kStringType:
            return "string";
        case ::rapidjson::kArrayType:
            return "array";
        case ::rapidjson::kObjectType:
            return "object";
    }
    return "unknown";
}

inline sgrn::Result<const Value*, std::string> requireMember(const Value& t_parent, const char* tp_key, const char* tp_type_name) {
    if (!t_parent.IsObject() || !t_parent.HasMember(tp_key)) {
        return fmt::format("missing '{}'", tp_key);
    }
    const Value* p_value = &t_parent[tp_key];
    if (!detail::matchesExpectedType(*p_value, tp_type_name)) {
        return fmt::format("'{}' must be {}", tp_key, tp_type_name);
    }
    return p_value;
}

inline sgrn::Result<const Value*, std::string> requireMember(const Value& t_parent, const std::string& t_key, const char* tp_type_name) {
    return requireMember(t_parent, t_key.c_str(), tp_type_name);
}

inline sgrn::Result<std::string, std::string> requireString(const Value& t_parent, const std::string& t_key) {
    auto res = requireMember(t_parent, t_key, "string");
    if (res.hasError()) {
        return res.error();
    }
    return std::string(res.value()->GetString(), res.value()->GetStringLength());
}

inline sgrn::Result<int, std::string> requireInt(const Value& t_parent, const std::string& t_key) {
    auto res = requireMember(t_parent, t_key, "integer");
    if (res.hasError()) {
        return res.error();
    }
    if (res.value()->IsInt()) {
        return res.value()->GetInt();
    }
    if (res.value()->IsInt64()) {
        return static_cast<int>(res.value()->GetInt64());
    }
    if (res.value()->IsUint()) {
        return static_cast<int>(res.value()->GetUint());
    }
    if (res.value()->IsUint64()) {
        return static_cast<int>(res.value()->GetUint64());
    }
    if (res.value()->IsDouble()) {
        return static_cast<int>(res.value()->GetDouble());
    }
    return fmt::format("'{}' must be integer", t_key);
}

inline sgrn::Result<bool, std::string> requireBool(const Value& t_parent, const std::string& t_key) {
    auto res = requireMember(t_parent, t_key, "boolean");
    if (res.hasError()) {
        return res.error();
    }
    return res.value()->GetBool();
}

inline sgrn::Result<double, std::string> requireDouble(const Value& t_parent, const std::string& t_key) {
    if (!t_parent.IsObject() || !t_parent.HasMember(t_key.c_str())) {
        return fmt::format("missing '{}'", t_key);
    }
    const Value& p_value = t_parent[t_key.c_str()];
    if (detail::valueIsNumeric(p_value)) {
        return p_value.GetDouble();
    }
    return fmt::format("'{}' must be number", t_key);
}

inline Value objectOrEmpty(const Value& t_parent, const char* tp_key, Allocator& t_alloc) {
    if (!t_parent.IsObject() || !t_parent.HasMember(tp_key) || t_parent[tp_key].IsNull()) {
        Value empty(::rapidjson::kObjectType);
        return empty;
    }
    if (!t_parent[tp_key].IsObject()) {
        Value empty(::rapidjson::kObjectType);
        return empty;
    }
    Value copy;
    copy.CopyFrom(t_parent[tp_key], t_alloc);
    return copy;
}

inline Document objectOrEmpty(const Value& t_parent, const std::string& t_key) {
    Document t_doc;
    Value built = objectOrEmpty(t_parent, t_key.c_str(), t_doc.GetAllocator());
    t_doc.Swap(built);
    return t_doc;
}

inline bool isNull(const Value& t_value) {
    return t_value.IsNull();
}

inline bool isNull(const Document& t_value) {
    return t_value.IsNull();
}

} // namespace sgrn::utils::json
