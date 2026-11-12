#pragma once

// JSON operations built on SGRN's Glaze-backed DOM facade.
// Keep backend-specific Glaze calls out of this header: callers depend only
// on sgrn::json::dom and the stable sgrn::utils::json API.

#include <sgrn/utils/json.hpp>

#include <cctype>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sgrn::utils::json
{

namespace dom = ::sgrn::json::dom;

/**
 * Parse a JSON object.
 *
 * This is the strict counterpart to deserialize(), whose legacy compatibility
 * behavior intentionally accepts comments and trailing commas.
 */
[[nodiscard]] inline sgrn::Result<Document, std::string> parse(std::string_view t_json) {
    Document doc;
    doc.parse(t_json.data(), t_json.size());
    if (doc.hasParseError()) {
        return "JSON parse error at offset " + std::to_string(doc.errorOffset());
    }
    if (!doc.is_object()) {
        return "JSON root must be an object";
    }
    return doc;
}

[[nodiscard]] inline bool equalsIgnoreCase(std::string_view t_left, std::string_view t_right) noexcept {
    if (t_left.size() != t_right.size())
        return false;
    // Locale-independent ASCII fold: avoids the per-character std::tolower
    // call (locale lookup, not inlinable) in member-name scans.
    const auto fold = [](unsigned char c) noexcept -> unsigned char {
        return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c | 0x20) : c;
    };
    for (std::size_t i = 0; i < t_left.size(); ++i) {
        if (fold(static_cast<unsigned char>(t_left[i])) != fold(static_cast<unsigned char>(t_right[i])))
            return false;
    }
    return true;
}

/**
 * Extract a subtree by slash-separated object path.
 * Member matching is ASCII case-insensitive. An empty path returns the
 * original input unchanged.
 */
[[nodiscard]] inline std::optional<std::string> extractSubtree(std::string_view t_json, std::string_view t_path, char t_separator = '/') {
    if (t_json.empty())
        return std::nullopt;
    if (t_path.empty())
        return std::string(t_json);

    Document doc;
    doc.parse(t_json.data(), t_json.size());
    if (doc.hasParseError())
        return std::nullopt;

    const Value* current = &doc;
    Value cursor;
    std::size_t start = 0;
    while (start < t_path.size()) {
        const std::size_t end = t_path.find(t_separator, start);
        const std::string_view segment = end == std::string_view::npos ? t_path.substr(start) : t_path.substr(start, end - start);

        if (!current->is_object())
            return std::nullopt;

        bool found = false;
        for (auto it = current->memberBegin(); it != current->memberEnd(); ++it) {
            const std::string_view member_name{it->name.getString(), it->name.getStringLength()};
            if (equalsIgnoreCase(member_name, segment)) {
                cursor.rebindTo(it->value);
                current = &cursor;
                found = true;
                break;
            }
        }
        if (!found)
            return std::nullopt;

        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }

    return serializeCompact(*current);
}

namespace detail
{

inline void copyJsonPath(
    const Value& t_source, Value& t_destination, Allocator& t_allocator, const std::vector<std::string>& t_tokens, std::size_t t_depth) {
    if (t_depth >= t_tokens.size() || !t_source.is_object())
        return;

    const std::string& token = t_tokens[t_depth];
    // Single lookup; `child` is a non-owning view bound to the source member.
    const auto member = t_source.findMember(std::string_view{token});
    if (member == t_source.memberEnd())
        return;

    const Value& child = member->value;
    if (t_depth + 1 == t_tokens.size()) {
        // AddMember copies the selected subtree once into the destination.
        // The former CopyFrom()+AddMember() sequence duplicated that copy.
        t_destination.addMember(std::string_view{token}, child, t_allocator);
        return;
    }

    if (!child.is_object())
        return;

    Value nested(dom::Type::Object);
    copyJsonPath(child, nested, t_allocator, t_tokens, t_depth + 1);
    if (!nested.objectEmpty()) {
        Value key(token.c_str(), static_cast<dom::SizeType>(token.size()), t_allocator);
        t_destination.addMember(key, nested, t_allocator);
    }
}

// Merge selected paths for the same root object without emitting duplicate
// nested keys when callers request multiple leaves under one branch.
inline void mergeFilteredObjects(Value& t_destination, const Value& t_source, Allocator& t_allocator) {
    if (!t_destination.is_object())
        t_destination.setObject();
    if (!t_source.is_object())
        return;

    for (auto it = t_source.memberBegin(); it != t_source.memberEnd(); ++it) {
        const std::string_view key{it->name.getString(), it->name.getStringLength()};
        const auto dest = t_destination.findMember(key);
        if (dest == t_destination.memberEnd()) {
            t_destination.addMember(key, it->value, t_allocator);
            continue;
        }

        Value& existing = dest->value; // view into the destination member
        if (existing.is_object() && it->value.is_object()) {
            mergeFilteredObjects(existing, it->value, t_allocator);
        } else {
            existing.copyFrom(it->value, t_allocator);
        }
    }
}

} // namespace detail

/**
 * Return compact JSON containing only the requested dotted field paths.
 * Paths use the first segment as the root object name (typically a DB name).
 */
[[nodiscard]] inline std::string filterFields(const Value& t_document, const std::set<std::string>& t_fields) {
    Document filtered;
    filtered.setObject();
    Allocator& allocator = filtered.allocator();

    for (const auto& field : t_fields) {
        const auto tokens = sgrn::utils::strings::tokenize(field, '.');
        if (tokens.size() < 2)
            continue;

        const std::string& root_name = tokens.front();
        if (!t_document.is_object())
            continue;
        const auto root_it = t_document.findMember(std::string_view{root_name});
        if (root_it == t_document.memberEnd())
            continue;

        const Value& root_source = root_it->value;
        Value root_filtered(dom::Type::Object);
        detail::copyJsonPath(root_source, root_filtered, allocator, tokens, 1);
        if (root_filtered.objectEmpty())
            continue;

        if (const auto existing_it = filtered.findMember(std::string_view{root_name}); existing_it != filtered.memberEnd()) {
            detail::mergeFilteredObjects(existing_it->value, root_filtered, allocator);
        } else {
            filtered.addMember(std::string_view{root_name}, root_filtered, allocator);
        }
    }

    return serializeCompact(filtered);
}

/** Serialize a key/value pair as an object with string-valued members. */
[[nodiscard]] inline std::string serializeValue(std::string_view t_key, std::string_view t_value) {
    Document doc;
    doc.setObject();
    Allocator& allocator = doc.allocator();

    Value key(t_key.data(), static_cast<dom::SizeType>(t_key.size()), allocator);
    Value value(t_value.data(), static_cast<dom::SizeType>(t_value.size()), allocator);
    doc.addMember(key, value, allocator);
    return serializeCompact(doc);
}

/** Serialize string key/value pairs as a compact JSON object. */
[[nodiscard]] inline std::string serializeObject(const std::vector<std::pair<std::string, std::string>>& t_pairs) {
    Document doc;
    doc.setObject();
    Allocator& allocator = doc.allocator();

    for (const auto& [key_text, value_text] : t_pairs) {
        Value key(key_text.data(), static_cast<dom::SizeType>(key_text.size()), allocator);
        Value value(value_text.data(), static_cast<dom::SizeType>(value_text.size()), allocator);
        doc.addMember(key, value, allocator);
    }
    return serializeCompact(doc);
}

} // namespace sgrn::utils::json
