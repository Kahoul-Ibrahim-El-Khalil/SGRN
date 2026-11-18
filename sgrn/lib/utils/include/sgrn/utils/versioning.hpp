// =======================================================
//  Versioning Utilities
// =======================================================
#pragma once

#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/hashing.hpp>
#include <sgrn/utils/strings.hpp>
#include <span>

namespace sgrn::utils::versioning
{

// Calculate SHA-512 hex hash of data (128 char base64url)
// Used for content checksums and version integrity
inline std::string sha512Hex(std::string_view t_data) {
    auto r = sgrn::utils::computeSha512Data(t_data, sgrn::utils::HashEncoding::base64url);
    if (r.hasError()) {
        return "";
    }
    return r.value();
}

// Convert string to byte span
inline std::span<const uint8_t> asBytes(std::string_view t_s) {
    return {reinterpret_cast<const uint8_t*>(t_s.data()), t_s.size()};
}

// Convert string view to string
inline std::string toString(std::string_view t_s) {
    return std::string(t_s);
}

} // namespace sgrn::utils::versioning