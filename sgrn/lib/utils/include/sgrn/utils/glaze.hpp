#pragma once
// sgrn/utils/glaze.hpp — sgrn::json wrapper over Glaze (header-only, C++23)
//
// Backend implementation for the sgrn::json facade. Application code should
// include <sgrn/utils/json-serialization.hpp>. This header owns the Glaze-backed
// implementation and Glaze-specific reflection declarations.
// sgrn::Result<T, JsonError> and never throw. All JSON in this project must
// go through this surface; direct glz:: calls are allowed only inside this
// header and in sgrn::json::Writer (twin/serialization hot paths).
//
// Design decisions recorded in docs/json-glaze-decisions.md.

#include <sgrn/Result.hpp>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <glaze/glaze.hpp>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace sgrn::json
{

// ---------------------------------------------------------------------------
// JsonError
// ---------------------------------------------------------------------------

struct JsonError {
    std::string message;
    std::optional<uint32_t> ec; ///< glz::error_code cast to uint32_t

    explicit JsonError(std::string t_msg, std::optional<uint32_t> t_ec = std::nullopt)
        : message(std::move(t_msg))
        , ec(t_ec) {
    }

    [[nodiscard]] const std::string& what() const noexcept {
        return message;
    }
};

// ---------------------------------------------------------------------------
// Default option sets
// ---------------------------------------------------------------------------

/// Write options: emit null members (preserves wire format for optional fields).
inline constexpr glz::opts kWriteOpts{
    .skip_null_members = false,
    .prettify = false,
};

/// Read options: tolerate unknown keys so the reader is forward-compatible.
inline constexpr glz::opts kReadOpts{
    .error_on_unknown_keys = false,
};

/// JSONC read options. This deliberately enables comments only; trailing
/// comma compatibility is handled by the legacy parser until its callers are
/// migrated and its accepted input grammar can be retired deliberately.
inline constexpr glz::opts kJsoncReadOpts{
    .comments = true,
    .error_on_unknown_keys = false,
};

/// Pretty-print options (human-facing output: schema dump, shell, debug).
inline constexpr glz::opts kPrettyOpts{
    .skip_null_members = false,
    .prettify = true,
};

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

namespace detail
{

[[nodiscard]] inline JsonError glazeError(const glz::error_ctx& t_ec, std::string_view t_input) {
    // glz::format_error produces a short, position-annotated message.
    // We never echo the raw input body — only the formatted diagnostic.
    std::string msg = glz::format_error(t_ec, t_input);
    return JsonError{std::move(msg), static_cast<uint32_t>(t_ec.ec)};
}

[[nodiscard]] inline JsonError glazeWriteError(const glz::error_ctx& t_ec) {
    // Write errors have no input body to echo.
    std::string msg = "JSON write error (code " + std::to_string(static_cast<uint32_t>(t_ec.ec)) + ")";
    return JsonError{std::move(msg), static_cast<uint32_t>(t_ec.ec)};
}

} // namespace detail

// ---------------------------------------------------------------------------
// Write
// ---------------------------------------------------------------------------

/// Serialize @p t_val to a new std::string.
/// @param t_size_hint  Optional initial buffer reservation (bytes).
template <class T>
[[nodiscard]] inline sgrn::Result<std::string, JsonError> write(const T& t_val, std::size_t t_size_hint = 0) {
    std::string out;
    if (t_size_hint > 0)
        out.reserve(t_size_hint);
    const auto ec = glz::write<kWriteOpts>(t_val, out);
    if (ec) {
        return sgrn::Result<std::string, JsonError>::Error(detail::glazeWriteError(ec));
    }
    return out;
}

/// Serialize @p t_val into an existing string (append semantics: clears first).
template <class T>
[[nodiscard]] inline sgrn::Result<void, JsonError> writeTo(std::string& t_out, const T& t_val) {
    t_out.clear();
    const auto ec = glz::write<kWriteOpts>(t_val, t_out);
    if (ec) {
        return sgrn::Result<void, JsonError>::Error(detail::glazeWriteError(ec));
    }
    return {};
}

/// Serialize @p t_val to a pretty-printed string.
template <class T>
[[nodiscard]] inline sgrn::Result<std::string, JsonError> writePretty(const T& t_val) {
    std::string out;
    const auto ec = glz::write<kPrettyOpts>(t_val, out);
    if (ec) {
        return sgrn::Result<std::string, JsonError>::Error(detail::glazeWriteError(ec));
    }
    return out;
}

/// Write options that omit unset optionals instead of emitting nulls. Used by
/// wire forms (e.g. the schema JSON) where an absent key means "default" and
/// an explicit null would break legacy readers.
inline constexpr glz::opts kOmitNullOpts{
    .skip_null_members = true,
    .prettify = false,
};

/// Pretty variant of kOmitNullOpts.
inline constexpr glz::opts kOmitNullPrettyOpts{
    .skip_null_members = true,
    .prettify = true,
};

/// Serialize @p t_val omitting unset optionals (compact).
template <class T>
[[nodiscard]] inline sgrn::Result<std::string, JsonError> writeOmitNull(const T& t_val) {
    std::string out;
    const auto ec = glz::write<kOmitNullOpts>(t_val, out);
    if (ec) {
        return sgrn::Result<std::string, JsonError>::Error(detail::glazeWriteError(ec));
    }
    return out;
}

/// Serialize @p t_val omitting unset optionals (pretty-printed).
template <class T>
[[nodiscard]] inline sgrn::Result<std::string, JsonError> writeOmitNullPretty(const T& t_val) {
    std::string out;
    const auto ec = glz::write<kOmitNullPrettyOpts>(t_val, out);
    if (ec) {
        return sgrn::Result<std::string, JsonError>::Error(detail::glazeWriteError(ec));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Read
// ---------------------------------------------------------------------------

/// Parse @p t_text into a new T.
/// Unknown keys are silently ignored (kReadOpts).
template <class T>
[[nodiscard]] inline sgrn::Result<T, JsonError> read(std::string_view t_text) {
    T out{};
    const auto ec = glz::read<kReadOpts>(out, t_text);
    if (ec) {
        return sgrn::Result<T, JsonError>::Error(detail::glazeError(ec, t_text));
    }
    return out;
}

/// Parse @p t_text into an existing @p t_out (in-place, avoids a copy).
template <class T>
[[nodiscard]] inline sgrn::Result<void, JsonError> readInto(T& t_out, std::string_view t_text) {
    const auto ec = glz::read<kReadOpts>(t_out, t_text);
    if (ec) {
        return sgrn::Result<void, JsonError>::Error(detail::glazeError(ec, t_text));
    }
    return {};
}

/// Read a whole file into a string. Never throws.
[[nodiscard]] inline sgrn::Result<std::string, JsonError> readFileText(const std::filesystem::path& t_path) {
    std::ifstream ifs(t_path, std::ios::binary);
    if (!ifs) {
        return sgrn::Result<std::string, JsonError>::Error(JsonError{"Cannot open file: " + t_path.string()});
    }
    return std::string(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
}

/// Read @p t_path and parse it into a new T (readFileText + read<T>).
template <class T>
[[nodiscard]] inline sgrn::Result<T, JsonError> readFile(const std::filesystem::path& t_path) {
    auto text = readFileText(t_path);
    if (text.hasError()) {
        return sgrn::Result<T, JsonError>::Error(text.error());
    }
    return read<T>(text.value());
}

// ---------------------------------------------------------------------------
// Dynamic / generic value
//
// glz::generic is Glaze's dynamic value type (null/number/string/bool/array/
// object). Use it for DOM-consumer paths that cannot be expressed as typed
// structs — e.g. HTTP responses that are passed through or probed field by
// field. See docs/json-glaze-decisions.md §dynamic-value.
//
// glz::generic_i64 keeps integer literals as int64_t (the f64 default would
// turn every number into a double), and objects preserve insertion order
// (ordered_small_map), so writing a parsed value back out keeps key order.
// ---------------------------------------------------------------------------

using Value = glz::generic_i64;
using Object = Value::object_t;
using Array = Value::array_t;

/// Construct empty dynamic JSON containers. The default Value is JSON null.
[[nodiscard]] inline Value object() {
    Value out{};
    out = Object{};
    return out;
}

[[nodiscard]] inline Value array() {
    Value out{};
    out = Array{};
    return out;
}

/// Find a member without inserting it. A non-object or missing key returns
/// nullptr. The returned pointer remains valid until the object is mutated.
[[nodiscard]] inline const Value* find(const Value& t_object, std::string_view t_key) {
    if (!t_object.is_object())
        return nullptr;
    const auto& members = t_object.get_object();
    const auto it = members.find(std::string{t_key});
    return it == members.end() ? nullptr : &it->second;
}

[[nodiscard]] inline Value* find(Value& t_object, std::string_view t_key) {
    if (!t_object.is_object())
        return nullptr;
    auto& members = t_object.get_object();
    const auto it = members.find(std::string{t_key});
    return it == members.end() ? nullptr : &it->second;
}

[[nodiscard]] inline bool contains(const Value& t_object, std::string_view t_key) {
    return find(t_object, t_key) != nullptr;
}

/// Insert or replace an object member. A non-object input is replaced with an
/// empty object, making this helper suitable for building nested responses.
inline void insertOrAssign(Value& t_object, std::string_view t_key, Value t_value) {
    if (!t_object.is_object())
        t_object = Object{};
    t_object.get_object().insert_or_assign(std::string{t_key}, std::move(t_value));
}

/// Erase an object member. Returns false when the input is not an object or
/// the member did not exist.
[[nodiscard]] inline bool erase(Value& t_object, std::string_view t_key) {
    if (!t_object.is_object())
        return false;
    auto& members = t_object.get_object();
    const auto it = members.find(std::string{t_key});
    if (it == members.end())
        return false;
    members.erase(it);
    return true;
}

/// Append to an existing JSON array. Wrong-type use is a programming error;
/// unlike insertOrAssign, this function never silently replaces a value.
inline void append(Value& t_array, Value t_value) {
    if (!t_array.is_array())
        throw std::invalid_argument("sgrn::json::append requires an array");
    t_array.get_array().push_back(std::move(t_value));
}

[[nodiscard]] inline Value string(std::string t_value) {
    Value out{};
    out = std::move(t_value);
    return out;
}

[[nodiscard]] inline Value boolean(bool t_value) {
    Value out{};
    out = t_value;
    return out;
}

[[nodiscard]] inline Value integer(int64_t t_value) {
    Value out{};
    out = t_value;
    return out;
}

[[nodiscard]] inline Value number(double t_value) {
    Value out{};
    out = t_value;
    return out;
}

/// Parse @p t_text to a dynamic glz::generic_i64.
[[nodiscard]] inline sgrn::Result<Value, JsonError> parseValue(std::string_view t_text) {
    return read<Value>(t_text);
}

/// Parse JSONC (JSON with // and /* */ comments) to a dynamic value.
/// Trailing commas are intentionally not enabled here because Glaze's JSONC
/// mode does not define them as part of the accepted grammar.
[[nodiscard]] inline sgrn::Result<Value, JsonError> parseJsoncValue(std::string_view t_text) {
    Value out{};
    const auto ec = glz::read<kJsoncReadOpts>(out, t_text);
    if (ec) {
        return sgrn::Result<Value, JsonError>::Error(detail::glazeError(ec, t_text));
    }
    return out;
}

/// Convert a dynamic Value into a typed struct via Glaze reflection (dump +
/// re-parse, unknown keys tolerated). Missing members keep their defaults.
template <class T>
[[nodiscard]] inline sgrn::Result<T, JsonError> readFromValue(const Value& t_val) {
    T out{};
    const auto ec = glz::read<kReadOpts>(out, t_val);
    if (ec) {
        return sgrn::Result<T, JsonError>::Error(detail::glazeError(ec, ""));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Raw JSON splicing
//
// glz::raw_json wraps a pre-serialized JSON fragment so Glaze copies it
// verbatim into the output stream without re-parsing. Use for pre-formatted
// REAL/LREAL values, NaN→null, and pre-serialized sub-trees.
// See docs/json-glaze-decisions.md §raw-json.
// ---------------------------------------------------------------------------

using RawJson = glz::raw_json;

} // namespace sgrn::json
