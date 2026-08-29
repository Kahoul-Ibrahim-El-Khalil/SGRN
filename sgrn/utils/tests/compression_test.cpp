#include <sgrn/utils/compression.hpp>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

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