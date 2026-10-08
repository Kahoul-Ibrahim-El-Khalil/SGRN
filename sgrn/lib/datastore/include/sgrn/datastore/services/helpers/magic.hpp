#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sgrn::datastore::services::helpers
{

struct MagicSignature {
    const char* extension;
    const uint8_t* pattern;
    size_t pattern_len;
    size_t offset;
};

const std::vector<MagicSignature>& getMagicSignatures();

std::optional<std::string> sniffExtension(const std::string& t_data);

bool isCompatibleSniffedFormat(const std::string& t_declared, const std::string& t_sniffed);

} // namespace sgrn::datastore::services::helpers