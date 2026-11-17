#pragma once
// codec.hpp — the single wire-format boundary shared by server and clients.
//
// Everything that depends on *how* a value travels over the wire lives here:
// the media types, the format enum, Accept negotiation and the Glaze encode /
// decode entry points. Application code (handlers, filters, clients) deals
// only in concrete reflected structs and never in Glaze options or in a
// particular format.
//
// JSON, MessagePack and BEVE use the same Glaze reflection, so adding a field
// to a wire struct requires no serializer changes.

#include <sgrn/Result.hpp>
#include <sgrn/utils/glaze.hpp>

#include <glaze/glaze.hpp>
#include <glaze/msgpack.hpp>

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace sgrn::wire
{

// ─── Media types ────────────────────────────────────────────────────────────

inline constexpr char kJsonContentType[] = "application/json";
inline constexpr char kBeveContentType[] = "application/x-beve";
inline constexpr char kMsgpackContentType[] = "application/msgpack";

enum class Format : uint8_t {
    Json,    ///< application/json        (human-readable, browser default)
    Beve,    ///< application/x-beve      (Glaze-native, fastest)
    Msgpack, ///< application/msgpack     (compact, opt-in)
};

/// Format used when a client expresses no usable preference.
///
/// JSON: it is readable, always decodable by a browser, and the format of
/// error responses. Binary formats stay opt-in via an explicit Accept header
/// (C++ clients ask for BEVE/MessagePack when they want the smaller payload).
inline constexpr Format kDefaultFormat = Format::Json;

inline constexpr std::string_view mediaType(Format t_format) {
    switch (t_format) {
        case Format::Beve:
            return kBeveContentType;
        case Format::Msgpack:
            return kMsgpackContentType;
        case Format::Json:
        default:
            return kJsonContentType;
    }
}

// ─── Content-Type → Format ──────────────────────────────────────────────────

namespace detail
{

inline char asciiLower(char t_c) noexcept {
    return (t_c >= 'A' && t_c <= 'Z') ? static_cast<char>(t_c + ('a' - 'A')) : t_c;
}

inline bool equalsIgnoreCase(std::string_view t_left, std::string_view t_right) noexcept {
    if (t_left.size() != t_right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < t_left.size(); ++i) {
        if (asciiLower(t_left[i]) != asciiLower(t_right[i])) {
            return false;
        }
    }
    return true;
}

/// Strip parameters (; charset=...) and surrounding whitespace.
inline std::string_view mediaTypeOnly(std::string_view t_value) noexcept {
    const std::size_t semi = t_value.find(';');
    if (semi != std::string_view::npos) {
        t_value = t_value.substr(0, semi);
    }
    const std::size_t begin = t_value.find_first_not_of(" \t\r\n");
    const std::size_t end = t_value.find_last_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    return t_value.substr(begin, end - begin + 1);
}

/// Parse the q parameter of a single Accept entry. Missing / bad q means 1.
inline double quality(std::string_view t_entry) noexcept {
    const std::size_t semi = t_entry.find(';');
    if (semi == std::string_view::npos) {
        return 1.0;
    }
    std::string_view params = t_entry.substr(semi + 1);
    const std::size_t eq = params.find('=');
    if (eq == std::string_view::npos) {
        return 1.0;
    }
    std::string_view value = params.substr(eq + 1);
    const std::size_t begin = value.find_first_not_of(" \t");
    const std::size_t end = value.find_last_not_of(" \t");
    if (begin == std::string_view::npos) {
        return 1.0;
    }
    value = value.substr(begin, end - begin + 1);
    double parsed = 1.0;
    if (std::from_chars(value.data(), value.data() + value.size(), parsed).ec != std::errc{}) {
        return 1.0;
    }
    return parsed;
}

} // namespace detail

/// Map a response Content-Type to the decoder to use. Unknown or absent
/// types fall back to JSON, which is also the server's error format.
inline Format formatFromContentType(std::string_view t_content_type) noexcept {
    const std::string_view media = detail::mediaTypeOnly(t_content_type);
    if (detail::equalsIgnoreCase(media, kBeveContentType) || detail::equalsIgnoreCase(media, "application/beve")) {
        return Format::Beve;
    }
    if (detail::equalsIgnoreCase(media, kMsgpackContentType) || detail::equalsIgnoreCase(media, "application/x-msgpack")) {
        return Format::Msgpack;
    }
    return Format::Json;
}

// ─── Accept negotiation ─────────────────────────────────────────────────────

/// Resolve the response format from an Accept header.
///
/// Explicitly listed formats compete on q-value; on a tie an explicit entry
/// beats a wildcard and binary beats JSON. `*/*` and `application/*` offer the
/// default format and MessagePack (never BEVE), preferring the default. When
/// nothing usable is listed, @p t_default is served.
inline Format negotiateFromAccept(std::string_view t_accept, Format t_default = kDefaultFormat) noexcept {
    double q_json = -1.0;
    double q_msgpack = -1.0;
    double q_beve = -1.0;
    double q_wild = -1.0;

    std::size_t start = 0;
    while (start < t_accept.size()) {
        const std::size_t comma = t_accept.find(',', start);
        std::string_view raw = (comma == std::string_view::npos) ? t_accept.substr(start) : t_accept.substr(start, comma - start);
        start = (comma == std::string_view::npos) ? t_accept.size() : comma + 1;

        const std::string_view entry = detail::mediaTypeOnly(raw);
        if (entry.empty()) {
            continue;
        }
        const double q = detail::quality(raw);

        if (detail::equalsIgnoreCase(entry, kJsonContentType)) {
            q_json = q;
        } else if (detail::equalsIgnoreCase(entry, kMsgpackContentType) || detail::equalsIgnoreCase(entry, "application/x-msgpack")) {
            q_msgpack = q;
        } else if (detail::equalsIgnoreCase(entry, kBeveContentType) || detail::equalsIgnoreCase(entry, "application/beve")) {
            q_beve = q;
        } else if (entry == "*/*" || entry == "application/*") {
            q_wild = q;
        }
    }

    struct Candidate {
        bool valid{false};
        bool explicit_entry{false};
        Format format{Format::Json};
        int priority{0};
        double quality{0.0};
    };
    auto better = [t_default](const Candidate& t_lhs, const Candidate& t_rhs) {
        if (!t_rhs.valid) {
            return true;
        }
        if (t_lhs.quality != t_rhs.quality) {
            return t_lhs.quality > t_rhs.quality;
        }
        if (t_lhs.explicit_entry != t_rhs.explicit_entry) {
            return t_lhs.explicit_entry; // an explicit entry beats a wildcard on a tie
        }
        if (!t_lhs.explicit_entry && (t_lhs.format == t_default) != (t_rhs.format == t_default)) {
            return t_lhs.format == t_default; // `*/*` means "the default", not "the fastest"
        }
        return t_lhs.priority > t_rhs.priority;
    };

    Candidate best;
    auto consider = [&](Format t_format, double t_q, int t_priority, bool t_explicit) {
        if (t_q <= 0.0) {
            return; // absent, or explicitly excluded with q=0
        }
        const Candidate candidate{true, t_explicit, t_format, t_priority, t_q};
        if (better(candidate, best)) {
            best = candidate;
        }
    };

    consider(Format::Json, q_json, 0, true);
    consider(Format::Msgpack, q_msgpack, 1, true);
    consider(Format::Beve, q_beve, 2, true);

    // `*/*` fills in only the formats the client did not name. It offers the
    // default and the compact binary alternative, never BEVE.
    if (q_wild >= 0.0) {
        if (q_json < 0.0) {
            consider(Format::Json, q_wild, 0, false);
        }
        if (q_msgpack < 0.0) {
            consider(Format::Msgpack, q_wild, 1, false);
        }
    }

    if (best.valid) {
        return best.format;
    }
    // Nothing acceptable was offered (empty header, only unknown types, or
    // everything excluded with q=0): serve the default.
    return t_default;
}

// ─── Encode / decode ────────────────────────────────────────────────────────

/// Serialize in @p t_format. On failure the (already descriptive) Glaze error
/// is returned unchanged.
template <class T>
[[nodiscard]] inline Result<std::string, json::JsonError> encode(const T& t_value, Format t_format) {
    switch (t_format) {
        case Format::Beve:
            return json::writeBeve(t_value);
        case Format::Msgpack: {
            std::string out;
            const auto ec = glz::write_msgpack(t_value, out);
            if (ec) {
                return Result<std::string, json::JsonError>::Error(json::detail::glazeWriteError(ec));
            }
            return out;
        }
        case Format::Json:
        default:
            return json::write(t_value);
    }
}

/// Deserialize a body in a known format.
template <class T>
[[nodiscard]] inline Result<T, json::JsonError> decode(std::string_view t_body, Format t_format) {
    switch (t_format) {
        case Format::Beve:
            return json::readBeve<T>(t_body);
        case Format::Msgpack: {
            static constexpr glz::opts kMsgpackReadOpts{.format = glz::MSGPACK, .error_on_unknown_keys = false};
            T out{};
            const auto ec = glz::read<kMsgpackReadOpts>(out, t_body);
            if (ec) {
                return Result<T, json::JsonError>::Error(json::detail::glazeError(ec, t_body));
            }
            return out;
        }
        case Format::Json:
        default:
            return json::read<T>(t_body);
    }
}

/// Deserialize a response body, choosing the format from its Content-Type.
/// This is the whole "wire format" contract a client needs: it never names a
/// format or includes a server header.
template <class T>
[[nodiscard]] inline Result<T, json::JsonError> decode(std::string_view t_body, std::string_view t_content_type) {
    return decode<T>(t_body, formatFromContentType(t_content_type));
}

} // namespace sgrn::wire
