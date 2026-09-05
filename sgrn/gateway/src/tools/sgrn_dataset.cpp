#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/gateway/common/endian_helper.hpp>
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

namespace
{

// Shared binary-WAL decode primitives (single implementation in
// database/PersistenceService.hpp, used by all readers).
using sgrn::gateway::database::findNextAnchorFrame;
using sgrn::gateway::database::isDictionaryRecord;
using sgrn::gateway::database::parseDictionaryLine;

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

/// Bytes endian_helper::loadValue() actually dereferences for a type.
/// Must stay in sync with loadValue's switch: types it doesn't decode
/// (Date/DateTime/String/...) read nothing, so any bound is safe for them.
/// Defined up here (rather than next to its other caller below) so the
/// transcode decoder can share it.
static size_t s7TypeByteSize(sgrn::scl::DataType t_type) {
    using DT = sgrn::scl::DataType;
    switch (t_type) {
        case DT::Bool:
        case DT::Byte:
        case DT::USInt:
        case DT::Char:
        case DT::SInt:
            return 1;
        case DT::Word:
        case DT::UInt:
        case DT::Int:
            return 2;
        case DT::DWord:
        case DT::UDInt:
        case DT::DInt:
        case DT::Real:
        case DT::Time:
        case DT::TimeOfDay:
            return 4;
        case DT::LWord:
        case DT::ULInt:
        case DT::LInt:
        case DT::LReal:
        case DT::LTime:
        case DT::LTimeOfDay:
            return 8;
        default:
            return 1;
    }
}

/// One schema leaf for value-preserving transcode: dotted path replicating
/// visitDbFields traversal (parent-prefixed names, base+relative offsets,
/// arrays collapsed to element 0), so offsets agree with the compiled
/// schema every other binary consumer uses.
struct TranscodeLeaf {
    std::string path; // DB-prefixed dotted path, e.g. "TankSkid.tank_level"
    size_t offset;    // absolute byte offset in the DB image
    sgrn::scl::DataType type;
    int bit; // for Bool
    s7codec::Endian endian;
};

/// Recursively collects leaves from a schema-JSON "fields" array.
void collectTranscodeLeaves(const rapidjson::Value& t_fields, const std::string& t_db_prefix, size_t t_base_offset,
    s7codec::Endian t_db_endian, std::vector<TranscodeLeaf>& t_out) {
    if (!t_fields.IsArray())
        return;
    for (const auto& field : t_fields.GetArray()) {
        if (!field.IsObject() || !field.HasMember("name") || !field["name"].IsString())
            continue;
        const std::string name = field["name"].GetString();
        if (!field.HasMember("offset") || (!field["offset"].IsUint() && !field["offset"].IsInt()))
            continue;
        const int rel = field["offset"].IsUint() ? static_cast<int>(field["offset"].GetUint()) : field["offset"].GetInt();
        if (rel < 0)
            continue;
        const std::string path = t_db_prefix.empty() ? name : t_db_prefix + "." + name;
        const size_t abs_offset = t_base_offset + static_cast<size_t>(rel);

        const bool has_children = field.HasMember("children") && field["children"].IsArray() && !field["children"].Empty();
        if (has_children) {
            collectTranscodeLeaves(field["children"], path, abs_offset, t_db_endian, t_out);
            continue;
        }
        if (!field.HasMember("type") || !field["type"].IsString())
            continue;
        auto type = sgrn::scl::parseDataType(field["type"].GetString());
        if (!type.has_value())
            continue;
        const int bit = (field.HasMember("bit") && field["bit"].IsUint()) ? static_cast<int>(field["bit"].GetUint()) : 0;
        s7codec::Endian endian = t_db_endian;
        if (field.HasMember("endianness") && field["endianness"].IsString() &&
            std::string_view(field["endianness"].GetString()) == "little") {
            endian = s7codec::Endian::Little;
        }
        t_out.push_back(TranscodeLeaf{path, abs_offset, *type, bit, endian});
    }
}

/// Builds db_number -> leaves from an embedded schema document. Accepts the
/// "dbs" member as either a list (with "number"/"name") or a keyed object.
bool buildTranscodeLayout(const rapidjson::Document& t_schema, std::unordered_map<uint16_t, std::vector<TranscodeLeaf>>& t_out) {
    if (!t_schema.IsObject() || !t_schema.HasMember("dbs"))
        return false;
    const auto& dbs = t_schema["dbs"];
    auto handle_db = [&](const rapidjson::Value& t_db) {
        uint16_t db_num = 0;
        if (t_db.HasMember("number") && t_db["number"].IsUint())
            db_num = static_cast<uint16_t>(t_db["number"].GetUint());
        else
            return;
        std::string db_name = (t_db.HasMember("name") && t_db["name"].IsString()) ? t_db["name"].GetString() : "";
        const std::string prefix = db_name.empty() ? fmt::format("DB{}", db_num) : db_name;
        s7codec::Endian db_endian = s7codec::Endian::Big;
        if (t_db.HasMember("endianness") && t_db["endianness"].IsString() && std::string_view(t_db["endianness"].GetString()) == "little") {
            db_endian = s7codec::Endian::Little;
        }
        if (!t_db.HasMember("fields"))
            return;
        std::vector<TranscodeLeaf> leaves;
        collectTranscodeLeaves(t_db["fields"], prefix, 0, db_endian, leaves);
        if (!leaves.empty())
            t_out[db_num] = std::move(leaves);
    };
    if (dbs.IsArray()) {
        for (const auto& db : dbs.GetArray()) {
            if (db.IsObject())
                handle_db(db);
        }
    } else if (dbs.IsObject()) {
        for (auto it = dbs.MemberBegin(); it != dbs.MemberEnd(); ++it) {
            if (it->value.IsObject())
                handle_db(it->value);
        }
    }
    return !t_out.empty();
}

/// Decodes one leaf to a typed JSON value (mirrors endian_helper::loadValue
/// reads, but type-preserving). Returns false when undecodable (out of
/// range, unsupported type, non-finite float) — caller skips the leaf.
bool decodeTranscodeLeaf(const uint8_t* t_image, size_t t_image_len, const TranscodeLeaf& t_leaf, rapidjson::Value& t_out) {
    using DT = sgrn::scl::DataType;
    using sgrn::gateway::common::endian_helper::loadFromBuffer;
    const size_t width = s7TypeByteSize(t_leaf.type);
    if (t_leaf.offset + width > t_image_len)
        return false;
    const uint8_t* p = t_image + t_leaf.offset;
    switch (t_leaf.type) {
        case DT::Bool:
            t_out.SetBool(((p[0] >> t_leaf.bit) & 1) != 0);
            return true;
        case DT::Byte:
        case DT::USInt:
        case DT::Char:
            t_out.SetUint(p[0]);
            return true;
        case DT::SInt:
            t_out.SetInt(static_cast<int8_t>(p[0]));
            return true;
        case DT::Word:
        case DT::UInt:
            t_out.SetUint(loadFromBuffer<uint16_t>(p, t_leaf.endian));
            return true;
        case DT::Int:
            t_out.SetInt(loadFromBuffer<int16_t>(p, t_leaf.endian));
            return true;
        case DT::DWord:
        case DT::UDInt:
        case DT::Time:
        case DT::TimeOfDay:
            t_out.SetUint64(loadFromBuffer<uint32_t>(p, t_leaf.endian));
            return true;
        case DT::DInt:
            t_out.SetInt(loadFromBuffer<int32_t>(p, t_leaf.endian));
            return true;
        case DT::Real: {
            const float v = loadFromBuffer<float>(p, t_leaf.endian);
            if (std::isinf(v) || std::isnan(v))
                return false;
            t_out.SetDouble(static_cast<double>(v));
            return true;
        }
        case DT::LWord:
        case DT::ULInt:
        case DT::LTime:
            t_out.SetUint64(loadFromBuffer<uint64_t>(p, t_leaf.endian));
            return true;
        case DT::LInt:
            t_out.SetInt64(loadFromBuffer<int64_t>(p, t_leaf.endian));
            return true;
        case DT::LReal: {
            const double v = loadFromBuffer<double>(p, t_leaf.endian);
            if (std::isinf(v) || std::isnan(v))
                return false;
            t_out.SetDouble(v);
            return true;
        }
        default:
            return false;
    }
}

/// Transcodes one decompressed binary archive into JSONL lines, in stream
/// order: the schema line (when embedded), control-frame JSON verbatim
/// (parse-guarded), one synthetic anchor record per full-image data frame,
/// and one synthetic anchor (with "delta":true and the real DB) per delta
/// frame. Calls t_emit once per output line. Sets *tp_truncated (when
/// non-null) if the tail is cut off mid-frame. Returns false when the header
/// is unreadable or the version is unsupported (emits nothing then).
bool transcodeBinaryToJsonl(const std::string& t_decompressed, const std::string& t_source_name,
    const std::function<void(std::string)>& t_emit, bool* tp_truncated) {
    database::BinaryWalHeader header;
    if (database::checkBinaryWalHeader(t_decompressed, header) != database::BinaryHeaderStatus::kOk) {
        uint16_t ver = 0;
        if (t_decompressed.size() >= 6)
            std::memcpy(&ver, t_decompressed.data() + 4, sizeof(ver));
        fmt::print(fg(fmt::color::red), "[sgrn_dataset] Unsupported binary version {} in {}\n", ver, t_source_name);
        return false;
    }
    const uint32_t schema_len = header.schema_len;
    std::string schema_json;
    if (schema_len > 0) {
        schema_json = t_decompressed.substr(10, schema_len);
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        w.StartObject();
        w.Key("type");
        w.String("schema");
        w.Key("schema");
        w.RawValue(schema_json.c_str(), schema_json.size(), rapidjson::kObjectType);
        w.EndObject();
        t_emit(sb.GetString());
    }

    // Value-preserving state: schema layout for decoding, dictionary maps
    // for ID resolution, and per-DB images for delta patching. Without a
    // usable schema + dictionary the transcoder falls back to metadata-only
    // synthetic records (historic behavior), never dropping frames.
    std::unordered_map<uint16_t, std::vector<TranscodeLeaf>> layout;
    bool have_layout = false;
    if (!schema_json.empty()) {
        rapidjson::Document schema_doc;
        if (!schema_doc.Parse(schema_json.c_str()).HasParseError() && schema_doc.IsObject()) {
            have_layout = buildTranscodeLayout(schema_doc, layout);
        }
        if (!have_layout) {
            fmt::print(
                fg(fmt::color::yellow), "[sgrn_dataset] Cannot decode values from {} (schema), emitting metadata records\n", t_source_name);
        }
    }
    std::vector<std::string> dict_paths;
    std::unordered_map<std::string, uint32_t> path_to_id;
    std::unordered_map<uint16_t, std::vector<uint8_t>> images;

    // Streams one valued record: {"type":..,"ts":..,"data"|"changes":{"id":value}}.
    auto emit_valued = [&](const char* t_type, int64_t t_ts, const char* t_payload_key,
                           std::vector<std::pair<std::string, rapidjson::Value>> t_entries) {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        w.StartObject();
        w.Key("type");
        w.String(t_type);
        w.Key("ts");
        w.Int64(t_ts);
        w.Key(t_payload_key);
        w.StartObject();
        for (auto& [id_str, val] : t_entries) {
            w.Key(id_str.c_str(), static_cast<rapidjson::SizeType>(id_str.size()));
            val.Accept(w);
        }
        w.EndObject();
        w.EndObject();
        t_emit(sb.GetString());
    };

    size_t pos = header.frames_start;
    database::BinaryFrame fr;
    while (true) {
        const auto status = database::decodeBinaryFrame(t_decompressed, pos, fr);
        if (status == database::BinaryFrameStatus::kEnd)
            break;
        const int64_t ts = fr.ts;
        const uint16_t db_num = fr.db;
        const uint32_t payload_len = fr.payload_len;
        const size_t frame_start = fr.header_start;
        const uint8_t* payload = fr.payload;

        // Resync helper: corruption jumps to the next verifiable anchor,
        // clean stop when none follows.
        auto resync_or_stop = [&]() {
            auto found = findNextAnchorFrame(t_decompressed, frame_start);
            if (!found)
                return false;
            int64_t anchor_ts = 0;
            std::memcpy(&anchor_ts, t_decompressed.data() + *found, sizeof(anchor_ts));
            fmt::print(fg(fmt::color::yellow), "[sgrn_dataset] Corruption at byte {} in {}, resumed at anchor ts={}\n", frame_start,
                t_source_name, anchor_ts);
            pos = *found;
            return true;
        };

        if (status == database::BinaryFrameStatus::kTruncated) {
            if (resync_or_stop())
                continue;
            fmt::print(fg(fmt::color::yellow), "[sgrn_dataset] {} trailing bytes discarded (truncated/corrupt archive?): {}\n",
                t_decompressed.size() - pos, t_source_name);
            if (tp_truncated != nullptr)
                *tp_truncated = true;
            break;
        }

        // Control frames already carry JSON WAL lines (dictionary /
        // manifest / anchor / footer) — pass them through verbatim. The
        // dictionary line additionally (re)builds ID resolution.
        if (db_num == sgrn::gateway::database::kControlFrameDbNum) {
            std::string control_json(reinterpret_cast<const char*>(payload), payload_len);
            rapidjson::Document control_doc;
            if (!control_doc.Parse(control_json.c_str()).HasParseError() && control_doc.IsObject()) {
                if (isDictionaryRecord(control_doc)) {
                    dict_paths.clear();
                    path_to_id.clear();
                    parseDictionaryLine(control_doc, dict_paths);
                    for (uint32_t id = 0; id < dict_paths.size(); ++id) {
                        if (!dict_paths[id].empty())
                            path_to_id[dict_paths[id]] = id;
                    }
                }
                t_emit(std::move(control_json));
            }
            continue;
        }

        // Decodes one DB image into (id-string, value) entries for every
        // dictionary-mapped leaf, optionally restricted to leaves touched by
        // t_changed_runs (delta frames). Unresolvable or undecodable leaves
        // are skipped, never fabricated.
        auto decode_image = [&](uint16_t t_db, const uint8_t* t_image, size_t t_image_len,
                                const std::vector<sgrn::gateway::database::BinaryDeltaRun>* t_changed_runs) {
            std::vector<std::pair<std::string, rapidjson::Value>> entries;
            if (!have_layout || path_to_id.empty())
                return entries;
            auto layout_it = layout.find(t_db);
            if (layout_it == layout.end())
                return entries;
            for (const auto& leaf : layout_it->second) {
                auto id_it = path_to_id.find(leaf.path);
                if (id_it == path_to_id.end())
                    continue;
                if (t_changed_runs != nullptr) {
                    const size_t width = s7TypeByteSize(leaf.type);
                    bool touched = false;
                    for (const auto& run : *t_changed_runs) {
                        if (leaf.offset < run.offset + run.len && run.offset < leaf.offset + width) {
                            touched = true;
                            break;
                        }
                    }
                    if (!touched)
                        continue;
                }
                rapidjson::Value val;
                if (decodeTranscodeLeaf(t_image, t_image_len, leaf, val))
                    entries.emplace_back(std::to_string(id_it->second), std::move(val));
            }
            return entries;
        };

        // Legacy synthetic record for frames decoded without schema or
        // dictionary (historic behavior): metadata only, no values.
        auto emit_synthetic = [&](const char* t_marker, int64_t t_ts, uint16_t t_db) {
            rapidjson::StringBuffer sb;
            rapidjson::Writer<rapidjson::StringBuffer> w(sb);
            w.StartObject();
            w.Key("type");
            w.String("anchor");
            w.Key("ts");
            w.Int64(t_ts);
            w.Key("db");
            w.Uint(t_db);
            if (t_marker != nullptr) {
                w.Key(t_marker);
                w.Bool(true);
            }
            w.Key("bytes_len");
            w.Uint(payload_len);
            w.EndObject();
            t_emit(sb.GetString());
        };

        // Delta frames (v2+): patch the cached image, then emit the changed
        // leaves. Anchor frames (v3+): verify, adopt, emit all leaves.
        // Anything unresolvable falls back to synthetic metadata records.
        if (db_num == sgrn::gateway::database::kDeltaFrameDbNum || db_num == sgrn::gateway::database::kAnchorFrameDbNum) {
            const bool is_anchor = (db_num == sgrn::gateway::database::kAnchorFrameDbNum);
            uint16_t real_db = 0;
            const uint8_t* image_ptr = nullptr;
            size_t image_len = 0;
            std::vector<sgrn::gateway::database::BinaryDeltaRun> runs;
            std::vector<uint8_t> new_image;
            bool frame_ok = false;
            bool corrupt = false; // structurally invalid (vs. merely unresolvable)
            if (is_anchor) {
                const uint8_t* anchor_image = nullptr;
                size_t anchor_len = 0;
                frame_ok = sgrn::gateway::database::verifyAnchorFrame(payload, payload_len, real_db, anchor_image, anchor_len);
                corrupt = !frame_ok;
                if (frame_ok)
                    new_image.assign(anchor_image, anchor_image + anchor_len);
            } else if (payload_len >= 2) {
                std::memcpy(&real_db, payload, sizeof(real_db));
                auto img_it = images.find(real_db);
                if (img_it == images.end()) {
                    frame_ok = false; // no keyframe yet — synthetic fallback below
                } else if (sgrn::gateway::database::parseDeltaRuns(payload, payload_len, img_it->second.size(), real_db, runs)) {
                    frame_ok = true;
                    new_image = img_it->second;
                    for (const auto& run : runs)
                        std::memcpy(new_image.data() + run.offset, payload + run.data_pos, run.len);
                } else {
                    frame_ok = false;
                    corrupt = true;
                }
            } else {
                corrupt = true;
            }
            if (!frame_ok) {
                // Corrupt content seeks resync: stream position is
                // untrustworthy. A valid envelope for an unseen DB just falls
                // back to metadata.
                if (corrupt) {
                    fmt::print(fg(fmt::color::yellow), "[sgrn_dataset] Corrupt {} frame in {}, seeking resync\n",
                        is_anchor ? "anchor" : "delta", t_source_name);
                    if (resync_or_stop())
                        continue;
                    break;
                }
                emit_synthetic("delta", ts, real_db);
                continue;
            }
            images[real_db] = new_image;
            image_ptr = images[real_db].data();
            image_len = images[real_db].size();
            auto entries = decode_image(real_db, image_ptr, image_len, is_anchor ? nullptr : &runs);
            if (entries.empty()) {
                // Nothing resolvable (no dictionary yet, or drifted offsets):
                // keep a metadata trace rather than dropping the frame.
                emit_synthetic(is_anchor ? "anchor" : "delta", ts, real_db);
            } else {
                emit_valued(is_anchor ? "anchor" : "delta", ts, is_anchor ? "data" : "changes", std::move(entries));
            }
            continue;
        }

        // Full-image frames: cache and emit every resolvable leaf.
        {
            images[db_num].assign(payload, payload + payload_len);
            auto entries = decode_image(db_num, images[db_num].data(), payload_len, nullptr);
            if (!entries.empty()) {
                emit_valued("anchor", ts, "data", std::move(entries));
            } else {
                emit_synthetic(nullptr, ts, db_num);
            }
        }
    }
    return true;
}

} // namespace

static bool isCategoricalType(sgrn::scl::DataType t_type) {
    switch (t_type) {
        case sgrn::scl::DataType::Bool:
        case sgrn::scl::DataType::Byte:
        case sgrn::scl::DataType::Word:
        case sgrn::scl::DataType::DWord:
        case sgrn::scl::DataType::LWord:
        case sgrn::scl::DataType::String:
            return true;
        default:
            return false;
    }
}

sgrn::Result<void> DatasetProcessor::loadSchema(const std::string& t_scl_path) {
    SGRN_RETURN_IF(t_scl_path.empty() || !std::filesystem::exists(t_scl_path), fmt::format("SCL Schema file not found: {}", t_scl_path));

    auto res = sgrn::scl::PlcSchemaStore::loadFromFile(t_scl_path);
    if (res.hasError()) {
        return fmt::format("Failed to parse SCL schema: {}", toString(res.error()));
    }
    schema_store_ = std::move(res.value());

    features_.clear();
    feature_index_map_.clear();

    for (const auto& db_pair : schema_store_.dbs()) {
        const uint16_t db_num = db_pair.first;
        const auto& db = db_pair.second;

        sgrn::gateway::twin::visitDbFields(db.fields, [&](const sgrn::gateway::twin::DbFieldVisitInfo& t_info) {
            if (!t_info.is_leaf)
                return; // only leaf fields hold decodable values

            const auto& field = *t_info.field;
            FeatureMeta meta;
            meta.db_name = db.db_name;
            meta.field_path = t_info.path;
            meta.full_name = fmt::format("{}.{}", db.db_name, t_info.path);
            meta.data_type = s7codec::s7TypeToString(field.type);
            meta.unit = field.unit.value_or("");
            meta.is_categorical = isCategoricalType(field.type);
            meta.db_num = db_num;
            meta.offset = static_cast<size_t>(t_info.absolute_offset);
            meta.bit_index = field.bit_index;
            meta.raw_type = field.type;

            if (!field.enum_map.empty() || sgrn::scl::kind_of(field) == sgrn::scl::FieldKind::Enum) {
                meta.is_categorical = true;
                meta.data_type = "ENUM";
                meta.enum_map = field.enum_map;
            }

            feature_index_map_[meta.full_name] = features_.size();
            features_.push_back(std::move(meta));
        });
    }

    return {};
}

std::vector<std::filesystem::path> DatasetProcessor::discoverFiles(const std::string& t_dir) {
    std::vector<std::filesystem::path> files;
    if (!std::filesystem::exists(t_dir))
        return files;

    std::error_code ec;
    if (std::filesystem::is_regular_file(t_dir, ec)) {
        const std::string ext = std::filesystem::path(t_dir).extension().string();
        if (ext == ".zst" || ext == ".jsonl")
            files.push_back(t_dir);
        return files;
    }

    for (const auto& entry : std::filesystem::recursive_directory_iterator(t_dir)) {
        if (entry.is_regular_file()) {
            std::string ext = entry.path().extension().string();
            if (ext == ".zst" || ext == ".jsonl") {
                files.push_back(entry.path());
            }
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

sgrn::Result<DatasetSummary> DatasetProcessor::process(const DatasetConfig& t_config) {
    auto schema_res = loadSchema(t_config.scl_schema_path);
    if (schema_res.hasError()) {
        return schema_res.error();
    }

    auto files = discoverFiles(t_config.input_dir);
    if (files.empty()) {
        return fmt::format("No telemetry archive files found in directory: {}", t_config.input_dir);
    }

    std::ofstream csv_out;
    if (!t_config.output_csv_path.empty()) {
        csv_out.open(t_config.output_csv_path, std::ios::out | std::ios::trunc);
        if (!csv_out.is_open()) {
            return fmt::format("Failed to open CSV output file: {}", t_config.output_csv_path);
        }
        // Write Header
        csv_out << "timestamp_ms";
        for (const auto& feat : features_) {
            csv_out << "," << feat.full_name;
        }
        csv_out << "\n";
    }

    DatasetSummary summary;
    summary.features = features_;

    // Last full DB image per DB, carried across files like the JSONL
    // current_state_: delta frames patch these in place.
    std::unordered_map<uint16_t, std::vector<uint8_t>> last_images;

    // One CSV row from the carried-forward state, shared by the binary and
    // JSONL branches so both formats produce identical row semantics.
    auto writeCsvRow = [&](int64_t t_ts) {
        if (!csv_out.is_open())
            return;
        csv_out << t_ts;
        for (size_t f_idx = 0; f_idx < summary.features.size(); ++f_idx) {
            auto& feat = summary.features[f_idx];
            auto st_it = current_state_.find(feat.full_name);
            if (st_it != current_state_.end()) {
                csv_out << "," << st_it->second;
                feat.total_samples++;

                double dval = 0.0;
                if (st_it->second == "true" || st_it->second == "TRUE")
                    dval = 1.0;
                else if (st_it->second == "false" || st_it->second == "FALSE")
                    dval = 0.0;
                else {
                    try {
                        dval = std::stod(st_it->second);
                    } catch (...) {
                        dval = 0.0;
                    }
                }

                if (feat.total_samples == 1 || dval < feat.min_val)
                    feat.min_val = dval;
                if (feat.total_samples == 1 || dval > feat.max_val)
                    feat.max_val = dval;
            } else {
                csv_out << ",";
                feat.null_count++;
            }
        }
        csv_out << "\n";
    };

    // Ingests one parsed JSONL-shape document (anchor / delta / legacy flat
    // / control anchor): numeric ID keys resolve through t_dict, then the
    // shared full-state row is written. Returns false for timestamp-less
    // lines (schema / dictionary / manifest / footer), which carry no data.
    // Used by the JSONL branch and, for JSON anchor controls, by the binary
    // branch — so transcoded output and direct reads agree row for row.
    auto ingestJsonRecord = [&](const rapidjson::Document& t_doc, const std::vector<std::string>& t_dict) {
        int64_t ts = 0;
        if (t_doc.HasMember("ts") && t_doc["ts"].IsInt64()) {
            ts = t_doc["ts"].GetInt64();
        } else if (t_doc.HasMember("timestamp") && t_doc["timestamp"].IsInt64()) {
            ts = t_doc["timestamp"].GetInt64();
        }
        if (ts == 0)
            return false;

        if (summary.start_timestamp_ms == 0 || ts < summary.start_timestamp_ms) {
            summary.start_timestamp_ms = ts;
        }
        if (ts > summary.end_timestamp_ms) {
            summary.end_timestamp_ms = ts;
        }

        auto ingestKeyedValues = [&](const rapidjson::Value& t_obj) {
            for (auto it = t_obj.MemberBegin(); it != t_obj.MemberEnd(); ++it) {
                std::string key = it->name.GetString();

                if (!t_dict.empty() && std::all_of(key.begin(), key.end(), ::isdigit)) {
                    size_t id = std::stoul(key);
                    if (id < t_dict.size() && !t_dict[id].empty())
                        key = t_dict[id];
                }

                std::string val_str;
                if (it->value.IsString())
                    val_str = it->value.GetString();
                else if (it->value.IsNumber())
                    val_str = std::to_string(it->value.GetDouble());
                else if (it->value.IsBool())
                    val_str = it->value.GetBool() ? "true" : "false";
                else
                    continue;

                current_state_[key] = val_str;
            }
        };

        // 1. Delta record: {"type":"delta","ts":123,"changes":{"<id>":val,...}}
        if (t_doc.HasMember("changes") && t_doc["changes"].IsObject()) {
            ingestKeyedValues(t_doc["changes"]);
        }
        // 2. Anchor record — current format: {"type":"anchor","ts":123,"data":{"<id>":val,...}}
        //    Legacy format (pre-dictionary): {"data":{"DbName":{"field":val}}}
        else if (t_doc.HasMember("data") && t_doc["data"].IsObject()) {
            const auto& data = t_doc["data"];
            bool looks_nested = false;
            for (auto it = data.MemberBegin(); it != data.MemberEnd(); ++it) {
                if (it->value.IsObject()) {
                    looks_nested = true;
                    break;
                }
            }
            if (looks_nested) {
                for (auto db_it = data.MemberBegin(); db_it != data.MemberEnd(); ++db_it) {
                    std::string db_name = db_it->name.GetString();
                    if (!db_it->value.IsObject())
                        continue;
                    for (auto f_it = db_it->value.MemberBegin(); f_it != db_it->value.MemberEnd(); ++f_it) {
                        std::string full_key = fmt::format("{}.{}", db_name, f_it->name.GetString());
                        std::string val_str;
                        if (f_it->value.IsString())
                            val_str = f_it->value.GetString();
                        else if (f_it->value.IsNumber())
                            val_str = std::to_string(f_it->value.GetDouble());
                        else if (f_it->value.IsBool())
                            val_str = f_it->value.GetBool() ? "true" : "false";
                        current_state_[full_key] = val_str;
                    }
                }
            } else {
                ingestKeyedValues(data);
            }
        }
        // 3. Legacy flat record: {"timestamp":123,"db":"TankSkid","path":"tank_level","val":"30.5"}
        else if (t_doc.HasMember("db") && t_doc.HasMember("path") && t_doc.HasMember("val")) {
            std::string db = t_doc["db"].GetString();
            std::string path = t_doc["path"].GetString();
            std::string full_key = fmt::format("{}.{}", db, path);
            std::string val_str;
            if (t_doc["val"].IsString())
                val_str = t_doc["val"].GetString();
            else if (t_doc["val"].IsNumber())
                val_str = std::to_string(t_doc["val"].GetDouble());
            else if (t_doc["val"].IsBool())
                val_str = t_doc["val"].GetBool() ? "true" : "false";
            current_state_[full_key] = val_str;
        }

        writeCsvRow(ts);
        ++summary.total_timestamps;
        ++summary.total_records_processed;
        return true;
    };

    for (const auto& file_path : files) {
        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open())
            continue;

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        std::string decompressed;
        if (file_path.extension() == ".zst") {
            auto dec_res = sgrn::utils::compression::decompressStringZstd(content);
            if (dec_res.hasError()) {
                continue;
            }
            decompressed = std::move(dec_res).value();
        } else {
            decompressed = std::move(content);
        }

        const bool is_binary = (decompressed.size() >= 4 && decompressed[0] == 'S' && decompressed[1] == 'G' && decompressed[2] == 'R' &&
                                decompressed[3] == 'N');

        if (is_binary) {
            database::BinaryWalHeader header;
            if (database::checkBinaryWalHeader(decompressed, header) != database::BinaryHeaderStatus::kOk) {
                uint16_t ver = 0;
                if (decompressed.size() >= 6)
                    std::memcpy(&ver, decompressed.data() + 4, sizeof(ver));
                fmt::print(fg(fmt::color::yellow), "[sgrn_dataset] Unsupported binary version {} in {}, skipping file.\n", ver,
                    file_path.string());
                continue;
            }
            size_t pos = header.frames_start;

            // File-local dictionary for JSON anchor controls (reset per file,
            // like the JSONL branch's path_by_id).
            std::vector<std::string> bin_paths;

            // On stream corruption, jump forward to the next verifiable
            // anchor instead of abandoning the file; fall back to stopping
            // cleanly when no anchor follows. Returns false to break.
            auto resync_or_stop = [&](size_t t_frame_start) {
                auto found = findNextAnchorFrame(decompressed, t_frame_start);
                if (!found)
                    return false;
                int64_t anchor_ts = 0;
                std::memcpy(&anchor_ts, decompressed.data() + *found, sizeof(anchor_ts));
                fmt::print(fg(fmt::color::yellow), "[sgrn_dataset] Corruption at byte {} in {}, resumed at anchor ts={}\n", t_frame_start,
                    file_path.string(), anchor_ts);
                pos = *found;
                return true;
            };

            database::BinaryFrame fr;
            while (true) {
                const auto status = database::decodeBinaryFrame(decompressed, pos, fr);
                if (status == database::BinaryFrameStatus::kEnd)
                    break;
                const int64_t ts = fr.ts;
                const uint16_t db_num = fr.db;
                const uint32_t payload_len = fr.payload_len;
                const size_t frame_start = fr.header_start;
                const uint8_t* payload = fr.payload;

                if (status == database::BinaryFrameStatus::kTruncated) {
                    if (resync_or_stop(frame_start))
                        continue;
                    fmt::print(fg(fmt::color::yellow), "[sgrn_dataset] {} trailing bytes discarded (truncated/corrupt archive?): {}\n",
                        decompressed.size() - pos, file_path.string());
                    break;
                }

                // Control frames carry JSON WAL lines. Dictionary lines build
                // the file-local id map; JSON anchor lines carry real snapshot
                // state (e.g. startup, before any binary anchor exists) and
                // are ingested exactly like JSONL lines so direct reads agree
                // with transcoded output row for row. Everything else skips.
                if (db_num == sgrn::gateway::database::kControlFrameDbNum) {
                    rapidjson::Document control_doc;
                    if (!control_doc.Parse(reinterpret_cast<const char*>(payload), payload_len).HasParseError() && control_doc.IsObject()) {
                        if (isDictionaryRecord(control_doc)) {
                            parseDictionaryLine(control_doc, bin_paths);
                        } else {
                            ingestJsonRecord(control_doc, bin_paths);
                        }
                    }
                    continue;
                }

                // Resolve the frame to a full DB image: full frames replace
                // the cached image, verifiable anchors (v3+) replace it after
                // a CRC check, delta frames (v2+) patch runs into it.
                uint16_t target_db = db_num;
                const uint8_t* image = payload;
                size_t image_len = payload_len;
                if (db_num == sgrn::gateway::database::kAnchorFrameDbNum) {
                    const uint8_t* anchor_image = nullptr;
                    size_t anchor_len = 0;
                    uint16_t anchor_db = 0;
                    if (!sgrn::gateway::database::verifyAnchorFrame(payload, payload_len, anchor_db, anchor_image, anchor_len)) {
                        fmt::print(
                            fg(fmt::color::yellow), "[sgrn_dataset] Anchor CRC mismatch in {}, seeking resync\n", file_path.string());
                        if (resync_or_stop(frame_start))
                            continue;
                        break;
                    }
                    last_images[anchor_db].assign(anchor_image, anchor_image + anchor_len);
                    target_db = anchor_db;
                    image = last_images[anchor_db].data();
                    image_len = anchor_len;
                } else if (db_num == sgrn::gateway::database::kDeltaFrameDbNum) {
                    // Delta for a DB with no cached keyframe yet (e.g. a file
                    // starting mid-stream): nothing to patch, skip silently.
                    // Structurally corrupt runs warn below.
                    uint16_t delta_db = 0;
                    std::vector<sgrn::gateway::database::BinaryDeltaRun> runs;
                    auto img_it = last_images.end();
                    bool have_image = false;
                    if (payload_len >= 2) {
                        uint16_t candidate = 0;
                        std::memcpy(&candidate, payload, sizeof(candidate));
                        img_it = last_images.find(candidate);
                        have_image = (img_it != last_images.end());
                    }
                    bool frame_ok = false;
                    bool corrupt = false;
                    if (have_image &&
                        sgrn::gateway::database::parseDeltaRuns(payload, payload_len, img_it->second.size(), delta_db, runs)) {
                        frame_ok = true;
                        for (const auto& run : runs)
                            std::memcpy(img_it->second.data() + run.offset, payload + run.data_pos, run.len);
                    } else if (have_image) {
                        corrupt = true;
                    }
                    if (!frame_ok) {
                        if (corrupt) {
                            fmt::print(
                                fg(fmt::color::yellow), "[sgrn_dataset] Corrupt delta frame in {}, seeking resync\n", file_path.string());
                            if (resync_or_stop(frame_start))
                                continue;
                            break;
                        }
                        continue;
                    }
                    target_db = delta_db;
                    image = img_it->second.data();
                    image_len = img_it->second.size();
                } else {
                    last_images[db_num].assign(payload, payload + payload_len);
                }

                if (ts > 0) {
                    if (summary.start_timestamp_ms == 0 || ts < summary.start_timestamp_ms)
                        summary.start_timestamp_ms = ts;
                    if (ts > summary.end_timestamp_ms)
                        summary.end_timestamp_ms = ts;
                }

                if (csv_out.is_open()) {
                    // Merge this frame's leaves into the carried-forward
                    // state (same semantics as the JSONL branch below), then
                    // emit one full-state row. Undecodable leaves ("null")
                    // stay absent rather than poisoning the row.
                    for (auto& feat : features_) {
                        if (feat.db_num != target_db || feat.offset + s7TypeByteSize(feat.raw_type) > image_len) {
                            continue;
                        }
                        std::string val_str = sgrn::gateway::common::endian_helper::loadValue(
                            image + feat.offset, feat.raw_type, s7codec::Endian::Big, feat.bit_index);
                        if (val_str == "null")
                            continue;
                        current_state_[feat.full_name] = val_str;
                    }
                }
                writeCsvRow(ts);

                summary.total_timestamps++;
                summary.total_records_processed++;
            }
            summary.total_files_processed++;
            continue;
        }

        std::istringstream stream(decompressed);
        std::string line;
        std::vector<std::string> path_by_id; // reset per file: dictionary line is file-local

        while (std::getline(stream, line)) {
            if (line.empty())
                continue;

            rapidjson::Document doc;
            if (doc.Parse(line.c_str()).HasParseError())
                continue;

            // Learn the id->path mapping before we ever see anchor/delta lines.
            if (isDictionaryRecord(doc)) {
                parseDictionaryLine(doc, path_by_id);
                continue;
            }

            ingestJsonRecord(doc, path_by_id);
        }
        summary.total_files_processed++;
    }

    if (csv_out.is_open()) {
        csv_out.close();
    }

    if (!t_config.manifest_path.empty()) {
        (void)generateManifest(t_config.manifest_path, summary);
    }

    return summary;
}

sgrn::Result<void> DatasetProcessor::generateManifest(const std::string& t_manifest_path, const DatasetSummary& t_summary) {
    rapidjson::StringBuffer sb;
    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);

    writer.StartObject();
    writer.Key("generator");
    writer.String("sgrn_dataset");

    writer.Key("created_at_ms");
    writer.Int64(sgrn::utils::time::nowMilliseconds());

    writer.Key("start_timestamp_ms");
    writer.Int64(t_summary.start_timestamp_ms);

    writer.Key("end_timestamp_ms");
    writer.Int64(t_summary.end_timestamp_ms);

    writer.Key("total_files_processed");
    writer.Uint64(t_summary.total_files_processed);

    writer.Key("total_records");
    writer.Uint64(t_summary.total_records_processed);

    writer.Key("features");
    writer.StartArray();

    for (const auto& feat : t_summary.features) {
        writer.StartObject();
        writer.Key("name");
        writer.String(feat.full_name.c_str());

        writer.Key("db");
        writer.String(feat.db_name.c_str());

        writer.Key("path");
        writer.String(feat.field_path.c_str());

        writer.Key("type");
        writer.String(feat.data_type.c_str());

        writer.Key("unit");
        writer.String(feat.unit.c_str());

        writer.Key("is_categorical");
        writer.Bool(feat.is_categorical);

        if (!feat.enum_map.empty()) {
            writer.Key("enum_values");
            writer.StartObject();
            for (const auto& kv : feat.enum_map) {
                writer.Key(std::to_string(kv.first).c_str());
                writer.String(kv.second.c_str());
            }
            writer.EndObject();
        }

        writer.Key("min_val");
        writer.Double(feat.min_val);

        writer.Key("max_val");
        writer.Double(feat.max_val);

        writer.Key("null_count");
        writer.Uint64(feat.null_count);

        writer.Key("total_samples");
        writer.Uint64(feat.total_samples);
        writer.EndObject();
    }

    writer.EndArray();
    writer.EndObject();

    std::ofstream out(t_manifest_path);
    if (!out.is_open()) {
        return fmt::format("Failed to write manifest: {}", t_manifest_path);
    }
    out << sb.GetString();
    out.close();

    return {};
}

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
