#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>

#include <sgrn/Result.hpp>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <string>
#include <string_view>
#include <type_traits>

// ---------------------------------------------------------------------------
// Wire format enum — shared between main library, crud port, and gateway.
// JSON is the default; BEVE > Msgpack on tie.
// ---------------------------------------------------------------------------

enum class WireFormat : uint8_t {
    Json,    ///< application/json        (default)
    Beve,    ///< application/x-beve      (Glaze-native C++ clients)
    Msgpack, ///< application/msgpack     (when requested)
};

// ---------------------------------------------------------------------------
// Negotiate wire format from Accept header.
// JSON is the default when no Accept header or unsupported types.
// BEVE > Msgpack on quality tie.
// ---------------------------------------------------------------------------

// Parse quality parameter from "application/type;q=xxx"
inline double extractQuality(std::string_view media_type);

// Resolve the wire format from the request's Accept header.
/// JSON is the default wire format.
/// BEVE > Msgpack on quality tie.
inline WireFormat negotiateFormat(drogon::HttpRequestPtr t_req) {
    if (!t_req)
        return WireFormat::Json;

    auto accept = t_req->getHeader("Accept");
    if (accept.empty())
        return WireFormat::Json;

    // Simple Accept header parsing (comma-separated media types, optional q=)
    WireFormat best = WireFormat::Json;
    double best_q = 0.0;

    std::string_view sv(accept);
    size_t start = 0;
    while (start < sv.size()) {
        size_t comma = sv.find(',', start);
        std::string_view media = (comma == std::string_view::npos) ? sv.substr(start) : sv.substr(start, comma - start);

        // Trim whitespace
        size_t begin = media.find_first_not_of(" \t");
        size_t end = media.find_last_not_of(" \t");
        if (begin == std::string_view::npos) {
            start = (comma == std::string_view::npos) ? sv.size() : comma + 1;
            continue;
        }
        media = media.substr(begin, end - begin + 1);

        // Extract quality parameter
        double q = extractQuality(media);

        // Determine wire format priority
        WireFormat fmt = WireFormat::Json;
        if (media == "application/msgpack" || media == "application/x-msgpack") {
            fmt = WireFormat::Msgpack;
        } else if (media == "application/x-beve" || media == "application/beve") {
            fmt = WireFormat::Beve;
        }
        // Unknown types default to JSON

        // Quality-based selection: higher q wins; on tie, BEVE > Msgpack > JSON
        if (fmt != WireFormat::Json) {
            if (q > best_q || (q == best_q && fmt > best)) {
                best = fmt;
                best_q = q;
            }
        }
        // If fmt == Json, it only replaces if there was no previous match

        start = (comma == std::string_view::npos) ? sv.size() : comma + 1;
    }
    return best;
}

// Helper: parse quality from ";q=xxx"
inline double extractQuality(std::string_view media_type) {
    double q = 1.0;
    size_t q_pos = media_type.find(';');
    if (q_pos != std::string_view::npos) {
        std::string_view q_str = media_type.substr(q_pos + 1);
        size_t eq = q_str.find('=');
        if (eq != std::string_view::npos) {
            std::string_view q_val = q_str.substr(eq + 1);
            size_t qbegin = q_val.find_first_not_of(" \t");
            size_t qend = q_val.find_last_not_of(" \t");
            if (qbegin != std::string_view::npos && qend != std::string_view::npos) {
                q_val = q_val.substr(qbegin, qend - qbegin + 1);
                double parsed;
                if (std::from_chars(q_val.data(), q_val.data() + q_val.size(), parsed).ec == std::errc{}) {
                    q = parsed;
                }
            }
        }
    }
    return q;
}

// ---------------------------------------------------------------------------
// JSON-only typed response for libraries that do not depend on datastore.
// ---------------------------------------------------------------------------

template <typename T>
inline drogon::HttpResponsePtr createTypedResponse(const T& t_value, drogon::HttpStatusCode t_http_code = drogon::k200OK) {
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setStatusCode(t_http_code);
    response->setContentTypeCode(drogon::CT_APPLICATION_JSON);

    std::string body;
    constexpr glz::opts json_opts{};
    if (const auto error = glz::write<json_opts>(t_value, body); error) {
        response->setStatusCode(drogon::k500InternalServerError);
        response->setBody(R"({"error":"Failed to serialize response","scope":"Serialization"})");
        return response;
    }
    response->setBody(std::move(body));
    return response;
}
