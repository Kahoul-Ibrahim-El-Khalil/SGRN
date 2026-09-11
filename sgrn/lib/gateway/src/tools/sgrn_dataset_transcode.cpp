// Binary-archive -> JSONL transcode helpers for sgrn-dataset.
// Split from sgrn_dataset.cpp; shared by the pipeline and merge units.
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
namespace transcode_detail
{

// Shared binary-WAL decode primitives (single implementation in
// database/PersistenceService.hpp, used by all readers).
using sgrn::gateway::database::findNextAnchorFrame;
using sgrn::gateway::database::isDictionaryRecord;
using sgrn::gateway::database::parseDictionaryLine;

/// Bytes endian_helper::loadValue() actually dereferences for a type.
/// Must stay in sync with loadValue's switch: types it doesn't decode
/// (Date/DateTime/String/...) read nothing, so any bound is safe for them.
/// Defined up here (rather than next to its other caller below) so the
/// transcode decoder can share it.
size_t s7TypeByteSize(sgrn::scl::DataType t_type) {
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
    using sgrn::common::endian_helper::loadFromBuffer;
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

} // namespace transcode_detail

} // namespace sgrn::gateway::tools
