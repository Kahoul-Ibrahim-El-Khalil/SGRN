#pragma once
#include <fmt/core.h>
#include <sgrn/Result.hpp>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
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

// -----------------------------------------------------------------------------
// Long-lived streaming line I/O over Zstd frames.
//
// These two primitives back the gateway's JSONL write-ahead log. Unlike the
// one-shot helpers above (one complete buffer in -> one complete result out),
// they keep a ZSTD_CStream / ZSTD_DStream open across many independently-timed
// writeLine()/readLine() calls, so peak memory stays bounded by the zstd
// buffer sizes (ZSTD_CStreamOutSize() ~= 128 KiB) instead of scaling with the
// total data volume.
// -----------------------------------------------------------------------------

/// Maximum acceptable length of a single logical line. Protects the reader's
/// pending_ accumulator from unbounded growth on a corrupted stream.
inline constexpr size_t kMaxLineBytes = 16ULL * 1024ULL * 1024ULL;

/// Keeps a ZSTD_CStream open across many writeLine() calls. Unlike
/// compressStringToFileZstd (one-shot), this is for long-lived, line-oriented
/// writers such as the gateway's WAL. Reuses the ZSTD_CStreamInSize()/
/// ZSTD_CStreamOutSize() buffer sizing already established in
/// compressFileStreamingZstd.
class ZstdLineWriter {
public:
    explicit ZstdLineWriter(const std::filesystem::path& t_path, int t_level = 5);
    ~ZstdLineWriter();

    ZstdLineWriter(const ZstdLineWriter&) = delete;
    ZstdLineWriter& operator=(const ZstdLineWriter&) = delete;

    /// Appends '\n' and feeds the line through the open compression stream.
    /// Returns SclError on compression or disk I/O failure; the writer is
    /// considered poisoned after an error and must not be reused.
    ::sgrn::Result<void> writeLine(std::string_view t_line);

    /// Feeds raw binary bytes through the open compression stream without adding a newline.
    ::sgrn::Result<void> writeRaw(const void* t_data, size_t t_size);

    /// Flushes the ZSTD frame end and closes the file. Safe to call once;
    /// the destructor calls this if the caller didn't.
    ::sgrn::Result<void> close();

    size_t bytesWrittenCompressed() const {
        return bytes_out_;
    }

private:
    Result<void> flushOutput();

    std::FILE* p_file_{nullptr};
    ZSTD_CStream* p_cstream_{nullptr};
    std::vector<char> in_buf_;
    std::vector<char> out_buf_;
    size_t bytes_out_{0};
    std::string init_error_;
    bool poisoned_{false};
    bool closed_{false};
};

/// Incremental line reader over a Zstd-compressed file. Unlike
/// decompressStringZstd (whole-buffer), this decompresses just enough of the
/// stream to yield the next '\n'-delimited line, so recovery never holds the
/// full decompressed file in memory.
class ZstdLineReader {
public:
    explicit ZstdLineReader(const std::filesystem::path& t_path);
    ~ZstdLineReader();

    ZstdLineReader(const ZstdLineReader&) = delete;
    ZstdLineReader& operator=(const ZstdLineReader&) = delete;

    /// Reads the next line (without the trailing '\n') into t_out.
    /// Returns false on EOF or error — check ok() to distinguish.
    bool readLine(std::string& t_out);
    bool ok() const {
        return !error_;
    }
    const std::string& errorMessage() const {
        return error_message_;
    }

    /// 1-based index of the next line to be read (i.e. the number of lines
    /// yielded so far). Used by the recovery engine to track which line
    /// holds the anchor without a separate counter.
    size_t lineIndex() const {
        return line_index_;
    }

private:
    bool refill();

    std::FILE* p_file_{nullptr};
    ZSTD_DStream* p_dstream_{nullptr};
    std::vector<char> in_buf_;
    std::vector<char> out_buf_;
    std::string pending_; // leftover decompressed bytes not yet split into a line
    size_t in_pos_{0};
    size_t in_size_{0};
    size_t line_index_{0};
    bool eof_{false};
    bool error_{false};
    std::string error_message_;
};

} // namespace sgrn::utils::compression

namespace sgrn::utils
{
using compression::compressFileStreamingZstd;
using compression::compressStringZstd;
using compression::decompressStringZstd;
} // namespace sgrn::utils
