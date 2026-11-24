#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sgrn::utils::file_signature
{

/// A magic-byte signature mapping a leading byte pattern to a file extension.
struct FileSignature {
    const char* extension;
    const uint8_t* pattern;
    std::size_t pattern_len;
    std::size_t offset;
};

/// Ordered magic-byte signatures used to sniff a file's true format.
const std::vector<FileSignature>& getSignatures();

/// Detect a file extension from its leading bytes (magic numbers).
/// Returns std::nullopt when no signature matches.
std::optional<std::string> sniffExtension(std::string_view t_data);

/// Whether a sniffed format is compatible with a declared extension
/// (e.g. jpg/jpeg, docx/zip, exe/dll).
bool isCompatibleSniffedFormat(const std::string& t_declared, const std::string& t_sniffed);

} // namespace sgrn::utils::file_signature