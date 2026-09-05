#include <fmt/format.h>
#include <sgrn/debug.hpp>
#include <sgrn/utils/compression.hpp>

#include <algorithm>
#include <cstdio> // FILE, fopen, fclose, fread, fwrite
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <vector>

#include <zstd.h>

namespace sgrn::utils::compression
{

using sgrn::Result;

static constexpr size_t kMaxDecompressedSize = 512ULL * 1024ULL * 1024ULL;

Result<std::string> compressStringZstd(const std::string_view& t_data, uint8_t t_compression_level) {
    size_t const max_dest_size = ZSTD_compressBound(t_data.size());
    std::string out_string;
    out_string.resize(max_dest_size);

    size_t const compressed_size = ZSTD_compress(out_string.data(), max_dest_size, t_data.data(), t_data.size(), t_compression_level);
    if (ZSTD_isError(compressed_size)) {
        return Error(fmt::format("Zstd error: {}", ZSTD_getErrorName(compressed_size)));
    }
    out_string.resize(compressed_size);
    return out_string;
}

// -----------------------------------------------------------------------------
// Decompression helpers (anonymous namespace)
// -----------------------------------------------------------------------------
namespace
{

// Thread‑local buffer reused by the streaming decompressor
static thread_local std::vector<char> tls_out_buffer;

Result<std::string> decompressKnownSize(std::string_view t_data, size_t t_expected_size) {
    if (t_expected_size > kMaxDecompressedSize) {
        return Error(fmt::format("Decompressed size exceeds maximum allowed size ({})", kMaxDecompressedSize));
    }

    std::string out;
    out.resize(t_expected_size);

    size_t const actual = ZSTD_decompress(out.data(), out.size(), t_data.data(), t_data.size());
    if (ZSTD_isError(actual)) {
        return Error(fmt::format("Zstd decompression error: {}", ZSTD_getErrorName(actual)));
    }

    out.resize(actual);
    return out;
}

Result<std::string> decompressStreaming(std::string_view t_data) {
    ZSTD_DStream* dstream = ZSTD_createDStream();
    if (!dstream) {
        return Error("Zstd: failed to create decompression stream");
    }

    auto free_dstream = [&] { ZSTD_freeDStream(dstream); };

    size_t const init_result = ZSTD_initDStream(dstream);
    if (ZSTD_isError(init_result)) {
        free_dstream();
        return Error(fmt::format("Zstd init error: {}", ZSTD_getErrorName(init_result)));
    }

    const size_t out_buf_size = ZSTD_DStreamOutSize();
    if (tls_out_buffer.size() < out_buf_size) {
        tls_out_buffer.resize(out_buf_size);
    }

    std::string result;
    const char* p_src = t_data.data();
    size_t src_remaining = t_data.size();

    while (src_remaining > 0) {
        ZSTD_inBuffer in{p_src, src_remaining, 0};
        bool frame_finished = false;

        while (in.pos < in.size) {
            ZSTD_outBuffer out{tls_out_buffer.data(), out_buf_size, 0};
            size_t const ret = ZSTD_decompressStream(dstream, &out, &in);

            if (ZSTD_isError(ret)) {
                free_dstream();
                return Error(fmt::format("Zstd stream error: {}", ZSTD_getErrorName(ret)));
            }

            result.append(static_cast<const char*>(out.dst), out.pos);

            if (result.size() > kMaxDecompressedSize) {
                free_dstream();
                return Error(fmt::format("Decompressed output exceeds maximum allowed size ({})", kMaxDecompressedSize));
            }

            if (ret == 0) {
                frame_finished = true;
                break;
            }
        }

        p_src += in.pos;
        src_remaining -= in.pos;

        if (frame_finished)
            break;
    }

    free_dstream();
    return result;
}

} // anonymous namespace

Result<std::string> decompressStringZstd(const std::string_view& t_data) {
    unsigned long long const decompressed_size = ZSTD_getFrameContentSize(t_data.data(), t_data.size());

    if (decompressed_size == ZSTD_CONTENTSIZE_ERROR) {
        return Error("Zstd error: input is not a valid zstd frame");
    }

    if (decompressed_size != ZSTD_CONTENTSIZE_UNKNOWN) {
        return decompressKnownSize(t_data, decompressed_size);
    }

    return decompressStreaming(t_data);
}

// -----------------------------------------------------------------------------
// File‑compression helpers (anonymous namespace)
// -----------------------------------------------------------------------------
namespace
{
namespace fs = std::filesystem;
struct FilePair {
    FILE* p_input = nullptr;
    FILE* p_output = nullptr;
};

Result<FilePair> openFiles(const fs::path& t_src, const fs::path& t_dest) {
    FILE* p_in_file = fopen(t_src.string().c_str(), "rb");
    if (!p_in_file) {
        return Error("Failed to open source file for reading");
    }

    struct stat sb;
    if (fstat(fileno(p_in_file), &sb) == -1) {
        fclose(p_in_file);
        return Error("Failed to stat source file");
    }

    if (!S_ISREG(sb.st_mode)) {
        fclose(p_in_file);
        return Error("Source file is not a regular file");
    }

    FILE* p_out_file = fopen(t_dest.string().c_str(), "wb");
    if (!p_out_file) {
        fclose(p_in_file);
        return Error("Failed to open destination file for writing");
    }

    return FilePair{p_in_file, p_out_file};
}

struct ZstdBuffers {
    void* in_buf = nullptr;
    void* out_buf = nullptr;
    size_t in_size = 0;
    size_t out_size = 0;
};

Result<ZstdBuffers> allocateZstdBuffers() {
    ZstdBuffers bufs;
    bufs.in_size = ZSTD_CStreamInSize();
    bufs.out_size = ZSTD_CStreamOutSize();
    bufs.in_buf = malloc(bufs.in_size);
    bufs.out_buf = malloc(bufs.out_size);

    if (!bufs.in_buf || !bufs.out_buf) {
        free(bufs.in_buf);
        free(bufs.out_buf);
        return Error("Failed to allocate buffers");
    }

    return bufs;
}

Result<ZSTD_CStream*> createAndInitCStream(uint8_t t_level) {
    ZSTD_CStream* p_cstream = ZSTD_createCStream();
    if (!p_cstream) {
        return Error("Failed to create compression stream");
    }

    size_t const init_result = ZSTD_initCStream(p_cstream, t_level);
    if (ZSTD_isError(init_result)) {
        ZSTD_freeCStream(p_cstream);
        return Error(fmt::format("Zstd init error: {}", ZSTD_getErrorName(init_result)));
    }

    return p_cstream;
}

Result<void> compressStreamLoop(ZSTD_CStream* tp_cstream, FILE* tp_input_file, FILE* tp_output_file, void* tp_in_buf, size_t t_in_size,
    void* tp_out_buf, size_t t_out_size) {
    const size_t to_read = t_in_size;

    while (size_t read = fread(tp_in_buf, 1, to_read, tp_input_file)) {
        ZSTD_inBuffer input{tp_in_buf, read, 0};

        while (input.pos < input.size) {
            ZSTD_outBuffer output{tp_out_buf, t_out_size, 0};
            size_t const ret = ZSTD_compressStream(tp_cstream, &output, &input);

            if (ZSTD_isError(ret)) {
                return Error(fmt::format("Zstd error: {}", ZSTD_getErrorName(ret)));
            }

            fwrite(tp_out_buf, 1, output.pos, tp_output_file);
        }
    }

    return {};
}

Result<void> flushAndFinish(ZSTD_CStream* tp_cstream, FILE* tp_output_file, void* tp_out_buf, size_t t_out_size) {
    size_t remaining = 1; // any non‑zero value starts the loop

    while (remaining != 0) {
        ZSTD_outBuffer output{tp_out_buf, t_out_size, 0};
        remaining = ZSTD_endStream(tp_cstream, &output);

        if (ZSTD_isError(remaining)) {
            return Error(fmt::format("Zstd end stream error: {}", ZSTD_getErrorName(remaining)));
        }

        fwrite(tp_out_buf, 1, output.pos, tp_output_file);
    }

    return {};
}

} // anonymous namespace

Result<size_t> compressFileStreamingZstd(const fs::path& t_src, const fs::path& t_dest, uint8_t t_compression_level) {

    // 1) Open input & output files
    auto file_res = openFiles(t_src, t_dest);
    if (!file_res) {
        return Error(file_res.error());
    }
    FilePair files = file_res.value();

    // 2) Allocate Zstd buffers
    auto buf_res = allocateZstdBuffers();
    if (!buf_res) {
        fclose(files.p_input);
        fclose(files.p_output);
        return Error(buf_res.error());
    }
    ZstdBuffers bufs = *buf_res;

    // 3) Create and initialise compression stream
    auto cstream_res = createAndInitCStream(t_compression_level);
    if (!cstream_res) {
        free(bufs.in_buf);
        free(bufs.out_buf);
        fclose(files.p_input);
        fclose(files.p_output);
        return Error(cstream_res.error());
    }
    ZSTD_CStream* cstream = *cstream_res;

    // 4) Compress main data stream
    auto loop_res = compressStreamLoop(cstream, files.p_input, files.p_output, bufs.in_buf, bufs.in_size, bufs.out_buf, bufs.out_size);
    if (!loop_res) {
        ZSTD_freeCStream(cstream);
        free(bufs.in_buf);
        free(bufs.out_buf);
        fclose(files.p_input);
        fclose(files.p_output);
        return Error(loop_res.error());
    }

    // 5) Flush remaining compressed data
    auto flush_res = flushAndFinish(cstream, files.p_output, bufs.out_buf, bufs.out_size);
    if (!flush_res) {
        ZSTD_freeCStream(cstream);
        free(bufs.in_buf);
        free(bufs.out_buf);
        fclose(files.p_input);
        fclose(files.p_output);
        return Error(flush_res.error());
    }

    // 6) Clean up resources
    ZSTD_freeCStream(cstream);
    free(bufs.in_buf);
    free(bufs.out_buf);
    fclose(files.p_input);
    fclose(files.p_output);

    // 7) Return final compressed size
    return fs::file_size(t_dest);
}

// -----------------------------------------------------------------------------
// ZstdLineWriter — long-lived incremental line-oriented compression stream.
// -----------------------------------------------------------------------------

ZstdLineWriter::ZstdLineWriter(const fs::path& t_path, int t_level) {
    p_file_ = fopen(t_path.string().c_str(), "wb");
    if (!p_file_) {
        init_error_ = "ZstdLineWriter: cannot open output file: " + t_path.string();
        return;
    }

    p_cstream_ = ZSTD_createCStream();
    if (!p_cstream_) {
        init_error_ = "ZstdLineWriter: failed to create compression stream";
        return;
    }

    size_t const init_result = ZSTD_initCStream(p_cstream_, t_level);
    if (ZSTD_isError(init_result)) {
        init_error_ = fmt::format("ZstdLineWriter: zstd init error: {}", ZSTD_getErrorName(init_result));
        ZSTD_freeCStream(p_cstream_);
        p_cstream_ = nullptr;
        fclose(p_file_);
        p_file_ = nullptr;
        return;
    }

    in_buf_.resize(ZSTD_CStreamInSize());
    out_buf_.resize(ZSTD_CStreamOutSize());
    if (in_buf_.empty() || out_buf_.empty()) {
        init_error_ = "ZstdLineWriter: failed to allocate stream buffers";
        ZSTD_freeCStream(p_cstream_);
        p_cstream_ = nullptr;
        fclose(p_file_);
        p_file_ = nullptr;
    }
}

ZstdLineWriter::~ZstdLineWriter() {
    if (!closed_) {
        (void)close();
    }
}

Result<void> ZstdLineWriter::writeLine(std::string_view t_line) {
    if (closed_) {
        return Error("ZstdLineWriter: write failed — writer already closed");
    }
    if (!init_error_.empty()) {
        return Error(init_error_);
    }
    if (poisoned_) {
        return Error("ZstdLineWriter: write failed — writer in poisoned state");
    }
    if (t_line.size() > kMaxLineBytes) {
        poisoned_ = true;
        return Error(fmt::format("ZstdLineWriter: line exceeds maximum allowed size ({})", kMaxLineBytes));
    }

    // Feed the line through the stream in in_buf_-sized chunks so a very large
    // line never needs a contiguous copy of itself.
    size_t processed = 0;
    while (processed < t_line.size()) {
        const size_t chunk = std::min(t_line.size() - processed, in_buf_.size());
        ZSTD_inBuffer input{t_line.data() + processed, chunk, 0};
        while (input.pos < input.size) {
            ZSTD_outBuffer output{out_buf_.data(), out_buf_.size(), 0};
            size_t const ret = ZSTD_compressStream(p_cstream_, &output, &input);
            if (ZSTD_isError(ret)) {
                poisoned_ = true;
                return Error(fmt::format("ZstdLineWriter: zstd error: {}", ZSTD_getErrorName(ret)));
            }
            if (output.pos > 0 && fwrite(out_buf_.data(), 1, output.pos, p_file_) != output.pos) {
                poisoned_ = true;
                return Error("ZstdLineWriter: failed to write compressed output to file");
            }
            bytes_out_ += output.pos;
        }
        processed += chunk;
    }

    // Terminate the JSONL record.
    const char newline = '\n';
    ZSTD_inBuffer input{&newline, 1, 0};
    while (input.pos < input.size) {
        ZSTD_outBuffer output{out_buf_.data(), out_buf_.size(), 0};
        size_t const ret = ZSTD_compressStream(p_cstream_, &output, &input);
        if (ZSTD_isError(ret)) {
            poisoned_ = true;
            return Error(fmt::format("ZstdLineWriter: zstd error: {}", ZSTD_getErrorName(ret)));
        }
        if (output.pos > 0 && fwrite(out_buf_.data(), 1, output.pos, p_file_) != output.pos) {
            poisoned_ = true;
            return Error("ZstdLineWriter: failed to write compressed output to file");
        }
        bytes_out_ += output.pos;
    }

    return {};
}

Result<void> ZstdLineWriter::writeRaw(const void* t_data, size_t t_size) {
    if (closed_) {
        return Error("ZstdLineWriter: write failed — writer already closed");
    }
    if (!init_error_.empty()) {
        return Error(init_error_);
    }
    if (poisoned_) {
        return Error("ZstdLineWriter: write failed — writer in poisoned state");
    }
    if (!t_data || t_size == 0) {
        return {};
    }

    const char* ptr = static_cast<const char*>(t_data);
    size_t processed = 0;
    while (processed < t_size) {
        const size_t chunk = std::min(t_size - processed, in_buf_.size());
        ZSTD_inBuffer input{ptr + processed, chunk, 0};
        while (input.pos < input.size) {
            ZSTD_outBuffer output{out_buf_.data(), out_buf_.size(), 0};
            size_t const ret = ZSTD_compressStream(p_cstream_, &output, &input);
            if (ZSTD_isError(ret)) {
                poisoned_ = true;
                return Error(fmt::format("ZstdLineWriter: zstd error: {}", ZSTD_getErrorName(ret)));
            }
            if (output.pos > 0 && fwrite(out_buf_.data(), 1, output.pos, p_file_) != output.pos) {
                poisoned_ = true;
                return Error("ZstdLineWriter: failed to write compressed output to file");
            }
            bytes_out_ += output.pos;
        }
        processed += chunk;
    }
    return {};
}

Result<void> ZstdLineWriter::close() {
    if (closed_)
        return {};

    Result<void> res;

    if (p_cstream_ && p_file_) {
        // Finish the ZSTD frame (drains any data buffered inside the compressor).
        size_t remaining = 1;
        while (remaining != 0) {
            ZSTD_outBuffer output{out_buf_.data(), out_buf_.size(), 0};
            remaining = ZSTD_endStream(p_cstream_, &output);
            if (ZSTD_isError(remaining)) {
                res = Error(fmt::format("ZstdLineWriter: end-stream error: {}", ZSTD_getErrorName(remaining)));
                poisoned_ = true;
                break;
            }
            if (output.pos > 0 && fwrite(out_buf_.data(), 1, output.pos, p_file_) != output.pos) {
                res = Error("ZstdLineWriter: failed to flush compressed output to file");
                poisoned_ = true;
                break;
            }
            bytes_out_ += output.pos;
        }
    }

    if (p_cstream_) {
        ZSTD_freeCStream(p_cstream_);
        p_cstream_ = nullptr;
    }
    if (p_file_) {
        fclose(p_file_);
        p_file_ = nullptr;
    }
    closed_ = true;
    return res;
}
// -----------------------------------------------------------------------------
// ZstdLineReader — incremental line-oriented decompression stream.
// -----------------------------------------------------------------------------

ZstdLineReader::ZstdLineReader(const fs::path& t_path) {
    p_file_ = fopen(t_path.string().c_str(), "rb");
    if (!p_file_) {
        error_ = true;
        error_message_ = "ZstdLineReader: cannot open input file: " + t_path.string();
        return;
    }

    p_dstream_ = ZSTD_createDStream();
    if (!p_dstream_) {
        error_ = true;
        error_message_ = "ZstdLineReader: failed to create decompression stream";
        fclose(p_file_);
        p_file_ = nullptr;
        return;
    }

    size_t const init_result = ZSTD_initDStream(p_dstream_);
    if (ZSTD_isError(init_result)) {
        error_ = true;
        error_message_ = fmt::format("ZstdLineReader: zstd init error: {}", ZSTD_getErrorName(init_result));
        ZSTD_freeDStream(p_dstream_);
        p_dstream_ = nullptr;
        fclose(p_file_);
        p_file_ = nullptr;
        return;
    }

    in_buf_.resize(ZSTD_DStreamInSize());
    out_buf_.resize(ZSTD_DStreamOutSize());
    if (in_buf_.empty() || out_buf_.empty()) {
        error_ = true;
        error_message_ = "ZstdLineReader: failed to allocate stream buffers";
        ZSTD_freeDStream(p_dstream_);
        p_dstream_ = nullptr;
        fclose(p_file_);
        p_file_ = nullptr;
    }
}

ZstdLineReader::~ZstdLineReader() {
    if (p_dstream_) {
        ZSTD_freeDStream(p_dstream_);
        p_dstream_ = nullptr;
    }
    if (p_file_) {
        fclose(p_file_);
        p_file_ = nullptr;
    }
}

bool ZstdLineReader::refill() {
    // Decompress until new output is available, the compressed input runs out,
    // or the frame ends.
    size_t safety = 0;
    while (true) {
        if (in_pos_ == in_size_) {
            in_size_ = fread(in_buf_.data(), 1, in_buf_.size(), p_file_);
            in_pos_ = 0;
            if (in_size_ == 0) {
                eof_ = true;
                return false;
            }
        }

        ZSTD_inBuffer in{in_buf_.data(), in_size_, in_pos_};
        ZSTD_outBuffer out{out_buf_.data(), out_buf_.size(), 0};
        size_t const ret = ZSTD_decompressStream(p_dstream_, &out, &in);
        if (ZSTD_isError(ret)) {
            error_ = true;
            error_message_ = fmt::format("ZstdLineReader: zstd stream error: {}", ZSTD_getErrorName(ret));
            return false;
        }

        in_pos_ = in.pos;

        if (out.pos > 0) {
            pending_.append(out_buf_.data(), out.pos);
            if (pending_.size() > kMaxLineBytes) {
                error_ = true;
                error_message_ = fmt::format("ZstdLineReader: line exceeds maximum allowed size ({})", kMaxLineBytes);
                return false;
            }
            return true;
        }

        // No output this round but the stream is progressing — a frame just
        // completed. Keep feeding until output appears or input runs out.
        if (ret == 0 && in_pos_ == in_size_) {
            eof_ = true;
            return false;
        }

        // Safety net against pathological streams that burn CPU without ever
        // producing output.
        if (++safety > (1ULL << 20)) {
            error_ = true;
            error_message_ = "ZstdLineReader: decompression made no progress";
            return false;
        }
    }
}

bool ZstdLineReader::readLine(std::string& t_out) {
    if (error_)
        return false;

    while (true) {
        const size_t nl = pending_.find('\n');
        if (nl != std::string::npos) {
            t_out.assign(pending_.data(), nl);
            pending_.erase(0, nl + 1);
            ++line_index_;
            return true;
        }

        if (pending_.size() >= kMaxLineBytes) {
            error_ = true;
            error_message_ = fmt::format("ZstdLineReader: line exceeds maximum allowed size ({})", kMaxLineBytes);
            return false;
        }

        if (eof_ || !refill()) {
            if (error_)
                return false;
            // Clean EOF — yield a trailing unterminated record if one exists.
            if (!pending_.empty()) {
                t_out = std::move(pending_);
                pending_.clear();
                ++line_index_;
                return true;
            }
            return false;
        }
    }
}

} // namespace sgrn::utils::compression
