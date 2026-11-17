#pragma once
// json-doc.hpp — bridge between JSON *text* (Postgres jsonb columns, SQL
// binds) and typed values on the wire.
//
// Handlers used to carry such documents as sgrn::json::RawJson, a string that
// Glaze splices verbatim into JSON but that MessagePack/BEVE can only encode
// as a string — so the same endpoint answered with a nested object in JSON
// and with a string in MessagePack. Wire structs now hold either a concrete
// type (std::vector<std::string> for role permissions, a spec struct for the
// system-config schema) or sgrn::json::Value for genuinely open-ended
// documents; both encode as real nested values in every wire format.
#include <sgrn/utils/json-serialization.hpp>

#include <string>
#include <string_view>
#include <utility>

namespace sgrn::datastore::jsondoc
{

/// Parse stored JSON text into a native document. Empty or invalid text
/// yields @p t_fallback_text parsed instead (default: an empty object).
inline sgrn::json::Value parse(std::string_view t_text, std::string_view t_fallback_text = "{}") {
    if (!t_text.empty()) {
        auto parsed = sgrn::json::parseValue(std::string(t_text));
        if (!parsed.hasError()) {
            return std::move(parsed).value();
        }
    }
    auto fallback = sgrn::json::parseValue(std::string(t_fallback_text));
    return fallback.hasError() ? sgrn::json::Value{} : std::move(fallback).value();
}

/// Parse stored JSON text straight into a concrete type, or @p t_default when
/// the text is empty, invalid, or has the wrong shape.
template <class T>
inline T parseAs(std::string_view t_text, T t_default = T{}) {
    auto parsed = sgrn::json::read<T>(std::string(t_text));
    return parsed.hasError() ? std::move(t_default) : std::move(parsed).value();
}

/// Compact JSON text for any wire value. A write failure, or a null document,
/// yields @p t_fallback_text so a SQL bind is never the literal string "null".
template <class T>
inline std::string toText(const T& t_value, std::string_view t_fallback_text = "{}") {
    auto written = sgrn::json::write(t_value);
    if (written.hasError()) {
        return std::string(t_fallback_text);
    }
    std::string text = std::move(written).value();
    return text == "null" ? std::string(t_fallback_text) : text;
}

} // namespace sgrn::datastore::jsondoc
