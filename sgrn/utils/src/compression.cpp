#include <fmt/format.h>
#include <sgrn/debug.hpp>
#include <sgrn/utils/compression.hpp>

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

} // namespace sgrn::utils::compression
