// Archive convert/merge entry points for sgrn-dataset.
// Split from sgrn_dataset.cpp; consumes the transcode unit.
#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/common/endian_helper.hpp>
#include <sgrn/gateway/database/PersistenceService.hpp>
#include <sgrn/gateway/tools/sgrn_dataset.hpp>
#include <sgrn/gateway/twin/FieldTraversal.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/time.hpp>
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <s7codec/endian.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sgrn::gateway::tools
{
// Transcode unit (sgrn_dataset_transcode.cpp): declared here so this TU can
// call across the split — the definition lives in transcode_detail there.
namespace transcode_detail
{
bool transcodeBinaryToJsonl(const std::string& t_decompressed, const std::string& t_source_name,
    const std::function<void(std::string)>& t_emit, bool* tp_truncated);
} // namespace transcode_detail
using namespace transcode_detail;

// Shared binary-WAL decode primitives (single implementation in
// database/PersistenceService.hpp, used by all readers).
using sgrn::gateway::database::findNextAnchorFrame;
using sgrn::gateway::database::isDictionaryRecord;
using sgrn::gateway::database::parseDictionaryLine;

namespace
{

/// True when decompressed bytes are a binary (.bin.zst) archive.
bool isBinaryContent(const std::string& t_decompressed) {
    return t_decompressed.size() >= 4 && t_decompressed[0] == 'S' && t_decompressed[1] == 'G' && t_decompressed[2] == 'R' &&
           t_decompressed[3] == 'N';
}

/// Reads a file, decompressing `.zst` inputs. Returns the raw bytes or an error.
sgrn::Result<std::string> readDecompressedFile(const std::filesystem::path& t_path) {
    std::ifstream file(t_path, std::ios::binary);
    if (!file.is_open()) {
        return fmt::format("Failed to open input file: {}", t_path.string());
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    if (t_path.extension() == ".zst") {
        auto dec_res = sgrn::utils::compression::decompressStringZstd(content);
        if (dec_res.hasError())
            return dec_res.error();
        return std::move(dec_res).value();
    }
    return content;
}

} // namespace

sgrn::Result<void> DatasetProcessor::convertFormat(
    const std::filesystem::path& t_input_file, const std::filesystem::path& t_output_file, const std::string& t_target_format) {
    if (!std::filesystem::exists(t_input_file)) {
        return fmt::format("Input file does not exist: {}", t_input_file.string());
    }

    std::ifstream file(t_input_file, std::ios::binary);
    if (!file.is_open()) {
        return fmt::format("Failed to open input file: {}", t_input_file.string());
    }
    std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();

    std::string decompressed;
    if (t_input_file.extension() == ".zst") {
        auto dec_res = sgrn::utils::compression::decompressStringZstd(content);
        if (dec_res.hasError())
            return dec_res.error();
        decompressed = std::move(dec_res).value();
    } else {
        decompressed = std::move(content);
    }

    const bool is_input_binary =
        (decompressed.size() >= 4 && decompressed[0] == 'S' && decompressed[1] == 'G' && decompressed[2] == 'R' && decompressed[3] == 'N');
    const bool want_binary = (t_target_format == "binary" || t_target_format == "bin.zst");

    sgrn::utils::compression::ZstdLineWriter writer(t_output_file, 5);

    if (want_binary) {
        if (!is_input_binary) {
            return fmt::format("convertFormat: jsonl -> binary transcoding is not yet implemented "
                               "(input: {}). Refusing to write a placeholder archive.",
                t_input_file.string());
        }
        (void)writer.writeRaw(decompressed.data(), decompressed.size());
    } else {
        if (is_input_binary) {
            bool ok = transcodeBinaryToJsonl(
                decompressed, t_input_file.string(), [&](std::string t_line) { (void)writer.writeLine(t_line); }, nullptr);
            if (!ok)
                return fmt::format("convertFormat: refusing to transcode unsupported archive: {}", t_input_file.string());
        } else {
            (void)writer.writeLine(decompressed);
        }
    }

    (void)writer.close();
    return {};
}

namespace
{

/// Copies one decompressed binary archive's frames verbatim into a binary
/// merge writer: the first file's header, then every frame. Footer control
/// frames are per-file bookkeeping and would lie in merged context, so they
/// are dropped; every other control frame passes through untouched.
sgrn::Result<void> mergeBinaryFrames(const std::string& t_decompressed, const std::string& t_source_name, bool t_first_file,
    const std::string& t_first_schema, sgrn::utils::compression::ZstdLineWriter& t_writer) {
    database::BinaryWalHeader header;
    const auto header_status = database::checkBinaryWalHeader(t_decompressed, header);
    if (header_status != database::BinaryHeaderStatus::kOk) {
        if (header_status == database::BinaryHeaderStatus::kBadVersion) {
            uint16_t ver = 0;
            std::memcpy(&ver, t_decompressed.data() + 4, sizeof(ver));
            std::memcpy(&header.schema_len, t_decompressed.data() + 6, sizeof(uint32_t));
            header.frames_start = 10 + header.schema_len;
            // Unknown framing: the (ts,db,len,payload) envelope itself is
            // still copied verbatim below, but flag it loudly.
            fmt::print(fg(fmt::color::yellow), "[sgrn_dataset] Unexpected binary version {} in {} (copying frames verbatim)\n", ver,
                t_source_name);
        } else if (!isBinaryContent(t_decompressed)) {
            return fmt::format("mergeArchives: not a binary archive: {}", t_source_name);
        } else {
            return fmt::format("mergeArchives: corrupt binary header: {}", t_source_name);
        }
    }
    const uint32_t schema_len = header.schema_len;
    const std::string schema = t_decompressed.substr(10, schema_len);
    if (!t_first_file && schema != t_first_schema) {
        fmt::print(fg(fmt::color::yellow), "[sgrn_dataset] Embedded schema drift in {} (merging anyway)\n", t_source_name);
    }

    if (t_first_file) {
        (void)t_writer.writeRaw(t_decompressed.data(), header.frames_start);
    }

    size_t pos = header.frames_start;
    database::BinaryFrame fr;
    while (true) {
        const auto status = database::decodeBinaryFrame(t_decompressed, pos, fr);
        if (status == database::BinaryFrameStatus::kEnd)
            break;
        if (status == database::BinaryFrameStatus::kTruncated) {
            auto found = findNextAnchorFrame(t_decompressed, pos);
            if (!found) {
                fmt::print(fg(fmt::color::yellow), "[sgrn_dataset] {} trailing bytes discarded (truncated/corrupt archive?): {}\n",
                    t_decompressed.size() - pos, t_source_name);
                break;
            }
            int64_t anchor_ts = 0;
            std::memcpy(&anchor_ts, t_decompressed.data() + *found, sizeof(anchor_ts));
            fmt::print(fg(fmt::color::yellow), "[sgrn_dataset] Corruption at byte {} in {}, resumed at anchor ts={}\n", pos, t_source_name,
                anchor_ts);
            pos = *found;
            continue;
        }

        bool drop = false;
        if (fr.db == sgrn::gateway::database::kControlFrameDbNum) {
            const std::string control_json(reinterpret_cast<const char*>(fr.payload), fr.payload_len);
            rapidjson::Document control_doc;
            if (!control_doc.Parse(control_json.c_str()).HasParseError() && control_doc.IsObject() && control_doc.HasMember("type") &&
                control_doc["type"].IsString() && std::string_view(control_doc["type"].GetString()) == "footer") {
                drop = true;
            }
        }
        if (!drop) {
            (void)t_writer.writeRaw(t_decompressed.data() + fr.header_start, 14 + fr.payload_len);
        }
    }
    return {};
}

} // namespace

sgrn::Result<void> DatasetProcessor::mergeArchives(const std::vector<std::filesystem::path>& t_inputs,
    const std::filesystem::path& t_output_file, const std::string& t_target_format, int t_zstd_level) {
    // Expand entries: directories in sorted filename order, explicit files
    // in the order given.
    std::vector<std::filesystem::path> files;
    for (const auto& entry : t_inputs) {
        std::error_code ec;
        if (std::filesystem::is_directory(entry, ec)) {
            auto dir_files = discoverFiles(entry.string());
            files.insert(files.end(), dir_files.begin(), dir_files.end());
        } else if (std::filesystem::exists(entry)) {
            files.push_back(entry);
        } else {
            return fmt::format("mergeArchives: input not found: {}", entry.string());
        }
    }
    if (files.empty()) {
        return fmt::format("mergeArchives: no archive files to merge ({} input(s))", t_inputs.size());
    }

    bool want_binary = (t_target_format == "binary" || t_target_format == "bin.zst" || t_target_format == "bin");
    if (t_target_format.empty() || t_target_format == "auto") {
        // Infer from the output name; default to jsonl.
        const std::string out_name = t_output_file.filename().string();
        want_binary = out_name.size() > 8 && out_name.ends_with(".bin.zst");
    } else if (t_target_format != "jsonl" && t_target_format != "jsonl.zst" && !want_binary) {
        return fmt::format("mergeArchives: unknown target format '{}' (want 'binary' or 'jsonl')", t_target_format);
    }

    struct DecodedInput {
        std::filesystem::path path;
        std::string content;
        bool is_binary = false;
    };
    std::vector<DecodedInput> inputs;
    inputs.reserve(files.size());
    for (const auto& file_path : files) {
        auto dec_res = readDecompressedFile(file_path);
        if (dec_res.hasError())
            return fmt::format("mergeArchives: {}: {}", file_path.string(), dec_res.error());
        DecodedInput in;
        in.path = file_path;
        in.content = std::move(dec_res).value();
        in.is_binary = isBinaryContent(in.content);
        inputs.push_back(std::move(in));
    }

    if (want_binary) {
        for (const auto& in : inputs) {
            if (!in.is_binary) {
                return fmt::format("mergeArchives: jsonl -> binary transcoding is not yet implemented "
                                   "(input: {}). Convert inputs to one format first.",
                    in.path.string());
            }
        }
    }

    sgrn::utils::compression::ZstdLineWriter writer(t_output_file, t_zstd_level);
    uint64_t files_merged = 0;

    if (want_binary) {
        std::string first_schema;
        bool first_file = true;
        for (const auto& in : inputs) {
            SGRN_RETURN_IF(auto r = mergeBinaryFrames(in.content, in.path.string(), first_file, first_schema, writer);
                r.hasError(), r.error());
            if (first_file) {
                uint32_t schema_len = 0;
                std::memcpy(&schema_len, in.content.data() + 6, sizeof(schema_len));
                first_schema = in.content.substr(10, schema_len);
                first_file = false;
            }
            ++files_merged;
        }
    } else {
        // JSONL merge: first file's schema/manifest win; changed dictionary
        // lines stay inline (readers relearn per line); every footer is
        // dropped and one recomputed footer closes the file.
        bool have_schema = false;
        std::string first_schema_line;
        bool have_dict = false;
        std::string last_dict_line;
        bool have_manifest = false;
        uint64_t line_no = 0;
        int64_t last_anchor_line = 0;

        auto emit_data_line = [&](const std::string& t_line, bool t_is_anchor) {
            (void)writer.writeLine(t_line);
            ++line_no;
            if (t_is_anchor)
                last_anchor_line = static_cast<int64_t>(line_no);
        };

        auto ingest_jsonl_line = [&](const std::string& t_line) {
            if (t_line.empty())
                return;
            rapidjson::Document doc;
            if (doc.Parse(t_line.c_str()).HasParseError() || !doc.IsObject())
                return;
            const bool has_type = doc.HasMember("type") && doc["type"].IsString();
            const std::string_view type = has_type ? std::string_view(doc["type"].GetString()) : std::string_view{};
            if (type == "schema") {
                if (!have_schema) {
                    have_schema = true;
                    first_schema_line = t_line;
                    (void)writer.writeLine(t_line);
                    ++line_no;
                } else if (t_line != first_schema_line) {
                    fmt::print(fg(fmt::color::yellow), "[sgrn_dataset] Embedded schema drift during merge (keeping first)\n");
                }
                return;
            }
            if (type == "dictionary") {
                if (!have_dict || t_line != last_dict_line) {
                    have_dict = true;
                    last_dict_line = t_line;
                    (void)writer.writeLine(t_line);
                    ++line_no;
                }
                return;
            }
            if (type == "manifest") {
                if (!have_manifest) {
                    have_manifest = true;
                    (void)writer.writeLine(t_line);
                    ++line_no;
                }
                return;
            }
            if (type == "footer") {
                return; // recomputed at the end
            }
            emit_data_line(t_line, type == "anchor");
        };

        for (const auto& in : inputs) {
            if (in.is_binary) {
                bool truncated = false;
                const bool ok = transcodeBinaryToJsonl(in.content, in.path.string(), ingest_jsonl_line, &truncated);
                if (!ok)
                    return fmt::format("mergeArchives: refusing unsupported archive: {}", in.path.string());
            } else {
                std::istringstream stream(in.content);
                std::string line;
                while (std::getline(stream, line)) {
                    ingest_jsonl_line(line);
                }
            }
            ++files_merged;
        }

        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        w.StartObject();
        w.Key("type");
        w.String("footer");
        w.Key("last_anchor_line");
        w.Int64(last_anchor_line);
        w.Key("record_count");
        w.Uint64(line_no);
        w.EndObject();
        (void)writer.writeLine(sb.GetString());
    }

    (void)writer.close();
    fmt::print(fg(fmt::color::green), "[sgrn_dataset] Merged {} files -> {}\n", files_merged, t_output_file.string());
    return {};
}

} // namespace sgrn::gateway::tools
