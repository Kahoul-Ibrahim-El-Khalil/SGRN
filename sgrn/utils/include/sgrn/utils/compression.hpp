#pragma once
#include <fmt/core.h>
#include <sgrn/Result.hpp>
#include <cstdint>
#include <filesystem>
#include <string>
#include <zstd.h>

namespace sgrn::utils::compression
{

// expected compressed buffer, unexpected: error string;
::sgrn::Result<std::string> compressStringZstd(const std::string_view& t_data, uint8_t t_compression_level);
// expected size of compressed buffer, unexpected error string;
::sgrn::Result<size_t> compressFileStreamingZstd(
    const std::filesystem::path& t_src, const std::filesystem::path& t_dest, uint8_t t_compression_level);
// Stream compress a string directly to file without loading entire compressed buffer into memory
::sgrn::Result<size_t> compressStringToFileZstd(
    const std::string_view& t_data, const std::filesystem::path& t_dest, uint8_t t_compression_level);

::sgrn::Result<std::string> decompressStringZstd(const std::string_view& t_data);
} // namespace sgrn::utils::compression

namespace sgrn::utils
{
using compression::compressFileStreamingZstd;
using compression::compressStringZstd;
using compression::decompressStringZstd;
} // namespace sgrn::utils
