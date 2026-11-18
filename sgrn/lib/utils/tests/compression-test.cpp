// Compression round-trip test for the streaming zstd JSONL primitives that
// back the WAL (incremental writer, bounded-memory reader, line index).
#include <sgrn/utils/compression.hpp>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

static void check(bool t_cond, const char* t_msg) {
    if (!t_cond) {
        std::cerr << "FAIL: " << t_msg << '\n';
        std::exit(1);
    }
}

// Round-trip test for the streaming JSONL zstd primitives that back the WAL:
//   - ZstdLineWriter writes N '\n'-terminated lines incrementally
//   - ZstdLineReader re-reads them one at a time, without slurping the whole
//     decompressed stream into memory
//   - lineIndex() counts yielded lines, so the recovery engine can position
//     itself at the last anchor without a separate counter

int main() {
    const std::filesystem::path test_file = (std::filesystem::temp_directory_path() / "sgrn_compression_test.jsonl.zst");

    // Clean up any previous run's artifact.
    std::error_code ec;
    std::filesystem::remove(test_file, ec);

    // A mix of small, boundary-sized, and >ZSTD_CStreamInSize() lines so the
    // chunked writer path and the reader's refill logic are both exercised.
    std::vector<std::string> lines;
    lines.push_back(R"({"type":"schema","dbs":["DB10"]})");
    lines.push_back(R"({"type":"manifest","start_time":"2026-01-01T00:00:00Z"})");
    lines.push_back(R"({"type":"anchor","ts":1,"data":{"DB10":"ready"}})");
    lines.push_back(R"({"type":"delta","ts":2,"changes":{"DB10.value":1.5}})");
    lines.push_back(R"({"type":"delta","ts":3,"changes":{"DB10.value":100}})");
    lines.push_back(std::string(64 * 1024, 'a') + R"({"type":"delta","ts":4})");
    lines.push_back(std::string(300 * 1024, 'b')); // > single compressor input buffer
    lines.push_back(R"({"type":"footer","last_anchor_line":3,"record_count":8})");

    {
        sgrn::utils::compression::ZstdLineWriter writer(test_file, 5);
        for (const auto& line : lines) {
            auto res = writer.writeLine(line);
            assert(res.has_value());
        }
        assert(writer.bytesWrittenCompressed() > 0);
        auto close_res = writer.close();
        assert(close_res.has_value());
        // Double-close is reported as success and is harmless.
        auto close_again = writer.close();
        assert(close_again.has_value());
    }

    assert(std::filesystem::exists(test_file));

    {
        sgrn::utils::compression::ZstdLineReader reader(test_file);
        assert(reader.ok());

        std::vector<std::string> read_lines;
        std::string line;
        while (reader.readLine(line)) {
            read_lines.push_back(line);
        }

        assert(reader.ok());
        assert(read_lines.size() == lines.size());
        for (size_t i = 0; i < lines.size(); ++i) {
            assert(read_lines[i] == lines[i]);
        }
        // lineIndex() is 1-based — after all lines are consumed it equals count.
        assert(reader.lineIndex() == lines.size());

        // The next line attempt must return a clean EOF.
        std::string extra;
        assert(!reader.readLine(extra));
        assert(reader.ok());
    }

    // Cross-compat: the whole frame must also be decodable by the one-shot
    // whole-buffer decompressor (proves we emit a plain, standard zstd frame).
    {
        std::ifstream ifs(test_file, std::ios::binary);
        std::string compressed((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        auto decompressed_res = sgrn::utils::compression::decompressStringZstd(compressed);
        assert(decompressed_res.has_value());

        std::string expect;
        for (const auto& l : lines) {
            expect += l;
            expect += '\n';
        }
        assert(decompressed_res.value() == expect);
    }

    // One-shot decompression with an explicit logical size: exact match
    // succeeds; mismatched, truncated and garbage inputs are rejected.
    {
        const std::string payload = "expected-size payload: " + std::string(1000, 'x');
        auto compressed_res = sgrn::utils::compression::compressStringZstd(payload, 3);
        check(compressed_res.has_value(), "compressStringZstd should succeed");
        const std::string& compressed = compressed_res.value();

        auto exact = sgrn::utils::compression::decompressStringZstd(compressed, payload.size());
        check(exact.has_value(), "exact expected size should decompress");
        check(exact.value() == payload, "exact expected size must round-trip");

        check(sgrn::utils::compression::decompressStringZstd(compressed, payload.size() - 1).hasError(),
            "too-small expected size must be rejected");
        check(sgrn::utils::compression::decompressStringZstd(compressed, payload.size() + 1).hasError(),
            "too-large expected size must be rejected");
        check(sgrn::utils::compression::decompressStringZstd(compressed.substr(0, compressed.size() / 2)).hasError(),
            "truncated input must be rejected");
        check(sgrn::utils::compression::decompressStringZstd(std::string(64, '\x7F')).hasError(), "garbage input must be rejected");

        auto empty_compressed = sgrn::utils::compression::compressStringZstd("", 3);
        check(empty_compressed.has_value(), "empty payload should compress");
        auto empty_out = sgrn::utils::compression::decompressStringZstd(empty_compressed.value(), 0);
        check(empty_out.has_value(), "empty payload should decompress");
        check(empty_out->empty(), "empty payload must stay empty");
    }

    // The zstd helpers accept any contiguous 1-byte range, not just
    // std::string / std::string_view.
    {
        namespace comp = sgrn::utils::compression;

        const std::string payload = "generic byte container payload: " + std::string(512, 'g');
        const std::vector<char> payload_chars(payload.begin(), payload.end());

        auto compressed_res = comp::compressStringZstd(payload_chars, 3);
        check(compressed_res.has_value(), "vector<char> compression should succeed");

        const std::string& compressed = compressed_res.value();
        const std::vector<unsigned char> compressed_uchars(compressed.begin(), compressed.end());
        check(comp::verifyCompressionSignature(compressed_uchars, "zstd"), "vector<unsigned char> should be recognised as zstd");

        auto restored = comp::decompressStringZstd(compressed_uchars, payload.size());
        check(restored.has_value(), "vector<unsigned char> decompression should succeed");
        check(restored.value() == payload, "vector<unsigned char> round-trip must be byte-exact");

        std::vector<std::byte> compressed_raw(compressed.size());
        std::memcpy(compressed_raw.data(), compressed.data(), compressed.size());

        auto restored_raw = comp::decompressStringZstd(compressed_raw);
        check(restored_raw.has_value(), "vector<std::byte> decompression should succeed");
        check(restored_raw.value() == payload, "vector<std::byte> round-trip must be byte-exact");

        // A fixed-size std::array is a valid input too.
        const std::array<char, 5> magic{
            {static_cast<char>(0x28), static_cast<char>(0xB5), static_cast<char>(0x2F), static_cast<char>(0xFD)}};
        check(comp::verifyCompressionSignature(magic, "zstd"), "std::array magic bytes should be recognised");
        check(!comp::verifyCompressionSignature(magic, "gzip"), "std::array must still be checked against the algorithm");

        const std::filesystem::path generic_file = std::filesystem::temp_directory_path() / "sgrn_compression_generic.zst";
        std::filesystem::remove(generic_file, ec);
        auto written = comp::compressStringToFileZstd(payload_chars, generic_file, 3);
        check(written.has_value(), "vector<char> streaming-to-file compression should succeed");
        check(std::filesystem::exists(generic_file), "streaming-to-file compression should create the file");
        std::filesystem::remove(generic_file, ec);
    }

    // POISONED writer: once a write fails the writer must refuse further use.
    {
        // Write to a path inside a directory that does not exist → constructor
        // cannot open the file, so the writer starts poisoned.
        sgrn::utils::compression::ZstdLineWriter writer("/nonexistent_dir/foo.jsonl.zst", 5);
        auto res = writer.writeLine(R"({"type":"delta","changes":{}})");
        assert(res.hasError());
        // And stays poisoned — a second attempt is an error too.
        auto res2 = writer.writeLine(R"({"type":"delta","changes":{}})");
        assert(res2.hasError());
        // close() on a never-opened writer is a no-op success.
        auto close_res = writer.close();
        assert(close_res.has_value());
    }

    std::filesystem::remove(test_file, ec);
    return 0;
}