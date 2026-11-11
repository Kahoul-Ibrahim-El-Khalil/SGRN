#pragma once
// sgrn/utils/glaze.hpp — sgrn::json wrapper over Glaze (header-only, C++23)
//
// Provides the sgrn::json namespace with typed read/write helpers that return
// sgrn::Result<T, JsonError> and never throw. All JSON in this project must
// go through this surface; direct glz:: calls are allowed only inside this
// header and in sgrn::json::Writer (twin/serialization hot paths).
//
// Design decisions recorded in docs/json-glaze-decisions.md.

#include <sgrn/Result.hpp>
#include <glaze/glaze.hpp>
#include <optional>
#include <string>
#include <string_view>

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

// ---------------------------------------------------------------------------
// Dynamic / generic value
//
// glz::json_t is Glaze's dynamic value type (null/bool/double/string/array/
// object). Use it for DOM-consumer paths that cannot be expressed as typed
// structs. See docs/json-glaze-decisions.md §dynamic-value.
// ---------------------------------------------------------------------------

using Value = glz::json_t;

/// Parse @p t_text to a dynamic glz::json_t.
[[nodiscard]] inline sgrn::Result<Value, JsonError> parseValue(std::string_view t_text) {
    return read<Value>(t_text);
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
