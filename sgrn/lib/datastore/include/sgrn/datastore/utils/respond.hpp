#pragma once

#include <drogon/HttpResponse.h>
#include <drogon/drogon.h>
#include <fmt/core.h>
#include <fmt/format.h>
#include <sgrn/Result.hpp>
#include <sgrn/datastore/BackendError.hpp>
#include <sgrn/datastore/ResultJson.hpp>
#include <sgrn/datastore/error/SgrnError.hpp>
#include <sgrn/datastore/wire/codec.hpp>
#include <sgrn/datastore/wire/common.hpp>
#include <sgrn/utils/glaze.hpp>
#include <sgrn/utils/json-serialization.hpp>
#include <algorithm>
#include <cctype>
#include <glaze/msgpack.hpp>

// Glaze provides BEVE and CBOR specializations for basic_raw_json and basic_text,
// but omits MSGPACK. Provide them here so types containing RawJson can be serialized to MessagePack.
namespace glz
{
template <class T>
struct to<MSGPACK, basic_raw_json<T>> final {
    template <auto Opts, class... Args>
    GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, Args&&... args) {
        serialize<MSGPACK>::op<Opts>(value.str, ctx, std::forward<Args>(args)...);
    }
};

template <class T>
struct to<MSGPACK, basic_text<T>> final {
    template <auto Opts, class... Args>
    GLZ_ALWAYS_INLINE static void op(auto&& value, is_context auto&& ctx, Args&&... args) {
        serialize<MSGPACK>::op<Opts>(value.str, ctx, std::forward<Args>(args)...);
    }
};

template <class T>
struct from<MSGPACK, basic_raw_json<T>> final {
    template <auto Opts, class... Args>
    GLZ_ALWAYS_INLINE static void op(auto&& value, Args&&... args) {
        from<MSGPACK, T>::template op<Opts>(value.str, std::forward<Args>(args)...);
    }
};

template <class T>
struct from<MSGPACK, basic_text<T>> final {
    template <auto Opts, class... Args>
    GLZ_ALWAYS_INLINE static void op(auto&& value, Args&&... args) {
        from<MSGPACK, T>::template op<Opts>(value.str, std::forward<Args>(args)...);
    }
};
} // namespace glz
#include <json/json.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <trantor/utils/Date.h>
#include <type_traits>

namespace sgrn
{

using MessageResponse = ::sgrn::datastore::common::Message;

inline constexpr auto json_content_type = ::sgrn::wire::kJsonContentType;
inline constexpr const char plain_content_type[] = "text/plain";
inline constexpr auto beve_content_type = ::sgrn::wire::kBeveContentType;
inline constexpr auto msgpack_content_type = ::sgrn::wire::kMsgpackContentType;

// =========================================================
// JSON string escaping (for dynamic values in fmt responses)
// Only escapes characters that would break a JSON string literal.
// Literal error-message constants need no escaping at the call site.
// =========================================================
inline void appendJsonEscaped(std::string& t_out, std::string_view t_value) {
    t_out.reserve(t_out.size() + t_value.size() + 4);
    for (unsigned char c : t_value) {
        switch (c) {
            case '"':
                t_out += "\\\"";
                break;
            case '\\':
                t_out += "\\\\";
                break;
            case '\n':
                t_out += "\\n";
                break;
            case '\r':
                t_out += "\\r";
                break;
            case '\t':
                t_out += "\\t";
                break;
            default:
                t_out += static_cast<char>(c);
        }
    }
}

inline std::string jsonEscape(std::string_view t_value) {
    std::string out;
    out.reserve(t_value.size() + 4);
    appendJsonEscaped(out, t_value);
    return out;
}

// =========================================================
// 1. Core Response Factory
// =========================================================
namespace detail
{
inline drogon::HttpResponsePtr makeWireResponse(std::string t_body, drogon::HttpStatusCode t_http_code, std::string_view t_content_type) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(t_http_code);
    resp->setContentTypeString(t_content_type);
    resp->setBody(std::move(t_body));
    return resp;
}
} // namespace detail

// =========================================================
// Wire-format content negotiation
//
// Inspect the request's Accept header and return the best supported format.
// JSON is the default: a client that sends no Accept header, `*/*`,
// `application/*`, or only types we do not produce gets JSON. Binary formats
// are served only when the client names them explicitly.
//
// curl / no Accept / `*/*`:           JSON    — the default.
// Dashboard / TS client:              JSON    — sends it explicitly.
// Server-to-server (Drogon, C++):     BEVE    — when explicitly requested.
// `Accept: application/msgpack`:      Msgpack — when explicitly requested.
//
// Error responses are always JSON (see createTypedResponse), whatever was
// negotiated, so a client can read a failure without a binary decoder.
// =========================================================

/// The wire formats themselves live in <sgrn/datastore/wire/codec.hpp>, which
/// has no Drogon dependency and is shared with the C++ client. These aliases
/// keep the existing server-facing spellings (sgrn::WireFormat, sgrn::Json…).
using WireFormat = ::sgrn::wire::Format;

/// Format served when the client expresses no preference we can honor.
inline constexpr WireFormat kDefaultWireFormat = ::sgrn::wire::kDefaultFormat;

/// Resolve the wire format from a raw Accept header value.
inline WireFormat negotiateFormatFromAccept(std::string_view t_accept) {
    return ::sgrn::wire::negotiateFromAccept(t_accept, kDefaultWireFormat);
}

/// Inspect the Accept header of @p t_req and return the negotiated WireFormat.
/// A null request or a missing header yields the default (MessagePack).
inline WireFormat negotiateFormat(const drogon::HttpRequestPtr& t_req) {
    if (!t_req) {
        return kDefaultWireFormat;
    }
    return negotiateFormatFromAccept(t_req->getHeader("Accept"));
}

namespace detail
{
// These keep the historical server-local names. The implementation lives in
// wire/codec.hpp so the client uses exactly the same encoder and decoder.
template <class T>
[[nodiscard]] inline sgrn::Result<std::string, ::sgrn::json::JsonError> writeMsgpack(const T& t_val) {
    return ::sgrn::wire::encode(t_val, WireFormat::Msgpack);
}

// Request bodies are read leniently, exactly like the JSON path: unknown keys
// are skipped, not rejected. This matters beyond convenience — the auth-scope
// filter (OrgBodyProbe) and the rate limiter (AuthProbe) read only one or
// three keys out of a larger body, and with glaze's default
// (error_on_unknown_keys = true) the probe would fail, silently skipping the
// cross-organisation body check and the per-account rate-limit key.
template <class T>
[[nodiscard]] inline sgrn::Result<T, ::sgrn::json::JsonError> readMsgpack(std::string_view t_data) {
    return ::sgrn::wire::decode<T>(t_data, WireFormat::Msgpack);
}
} // namespace detail

// Declared before the convenience overload below: unqualified lookup from a
// template only sees declarations that precede its definition, and ADL does
// not reach sgrn for types declared in sgrn::datastore::*.
template <typename T>
inline drogon::HttpResponsePtr createTypedResponse(const T& t_value, WireFormat t_fmt, drogon::HttpStatusCode t_http_code = drogon::k200OK);

/// Fallback overload for call sites that have no request to negotiate with.
/// Deliberately stays JSON (NOT kDefaultWireFormat): those handlers are read by
/// clients that parse JSON unconditionally, and they cannot have asked for
/// anything else. Handlers that hold the request use the overload below.
template <typename T>
inline drogon::HttpResponsePtr createTypedResponse(const T& t_value, drogon::HttpStatusCode t_http_code = drogon::k200OK) {
    return createTypedResponse(t_value, WireFormat::Json, t_http_code);
}

/// Serialize @p t_value in the requested wire format and return the response.
/// Error responses are always JSON regardless of the negotiated format so that
/// clients can parse them without a binary decoder.
template <typename T>
inline drogon::HttpResponsePtr createTypedResponse(const T& t_value, WireFormat t_fmt, drogon::HttpStatusCode t_http_code) {
    if constexpr (requires { t_value.http_status; }) {
        auto serialized = ::sgrn::json::writeOmitNull(t_value);
        if (serialized.hasError()) {
            return detail::makeWireResponse(R"({"error":"Failed to serialize error response","scope":"Serialization"})",
                drogon::k500InternalServerError, ::sgrn::json_content_type);
        }
        return detail::makeWireResponse(std::move(serialized).value(), t_value.http_status, ::sgrn::json_content_type);
    }

    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(t_http_code);

    switch (t_fmt) {
        case WireFormat::Beve: {
            auto result = ::sgrn::json::writeBeve(t_value);
            if (result.hasError())
                return detail::makeWireResponse(R"({"error":"Failed to serialize BEVE response","scope":"Serialization"})",
                    drogon::k500InternalServerError, ::sgrn::json_content_type);
            resp->setContentTypeString(beve_content_type);
            resp->setBody(std::move(result).value());
            return resp;
        }
        case WireFormat::Msgpack: {
            auto result = detail::writeMsgpack(t_value);
            if (result.hasError())
                return detail::makeWireResponse(R"({"error":"Failed to serialize MessagePack response","scope":"Serialization"})",
                    drogon::k500InternalServerError, ::sgrn::json_content_type);
            resp->setContentTypeString(msgpack_content_type);
            resp->setBody(std::move(result).value());
            return resp;
        }
        case WireFormat::Json:
        default: {
            auto result = ::sgrn::json::write(t_value);
            if (result.hasError())
                return detail::makeWireResponse(R"({"error":"Failed to serialize JSON response","scope":"Serialization"})",
                    drogon::k500InternalServerError, ::sgrn::json_content_type);
            resp->setContentTypeString(json_content_type);
            resp->setBody(std::move(result).value());
            return resp;
        }
    }
}

/// Convenience overload: negotiate the format from the request and serialize.
template <typename T>
inline drogon::HttpResponsePtr createTypedResponse(
    const drogon::HttpRequestPtr& t_req, const T& t_value, drogon::HttpStatusCode t_http_code = drogon::k200OK) {
    return createTypedResponse(t_value, negotiateFormat(t_req), t_http_code);
}

/// Deserialize an incoming request body (JSON, Msgpack, or BEVE) into concrete struct T.
template <typename T>
[[nodiscard]] inline sgrn::Result<T, std::string> parseTypedRequest(const drogon::HttpRequestPtr& t_req) {
    if (!t_req) {
        return sgrn::Result<T, std::string>::Error("Null request pointer");
    }
    const std::string_view body = t_req->body();
    if (body.empty()) {
        return sgrn::Result<T, std::string>::Error("Empty request body");
    }

    auto res = ::sgrn::wire::decode<T>(body, t_req->getHeader("content-type"));
    if (res.hasError()) {
        return sgrn::Result<T, std::string>::Error(res.error().what());
    }
    return res.value();
}

} // namespace sgrn
