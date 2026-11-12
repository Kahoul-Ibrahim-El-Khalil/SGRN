#include <sgrn/utils/json-dom.hpp>
// Binary-archive -> JSONL transcode helpers for sgrn-dataset.
// Split from sgrn-dataset.cpp; shared by the pipeline and merge units.
#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/common/endian-helper.hpp>
#include <sgrn/gateway/database/PersistenceService.hpp>
#include <sgrn/gateway/tools/sgrn-dataset.hpp>
#include <sgrn/gateway/twin/FieldTraversal.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/schema/SchemaSerializer.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/time.hpp>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <s7codec/endian.hpp>
#include <stdexcept>
#include <string>

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
        case DT::DTL:
            return 12;
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
    /// Array span of the WAL dictionary leaf (1 = scalar). The dictionary
    /// collapses struct/primitive arrays to one un-indexed path; emitting
    /// every element under that one id lets JSONL consumers fan values out
    /// to the per-element ("a.b[0].c") feature columns.
    uint32_t count = 1;
    /// Per-element byte stride for non-Bool arrays (struct: parent
    /// struct_size; primitive: type width). Bool arrays are bit-packed:
    /// element i lives in bit (bit + i), encoded as stride = 0.
    uint32_t stride = 0;
};

/// Recursively collects leaves from a schema-JSON "fields" array.
void collectTranscodeLeaves(const sgrn::json::dom::Value& t_fields, const std::string& t_db_prefix, size_t t_base_offset,
    s7codec::Endian t_db_endian, std::vector<TranscodeLeaf>& t_out) {
    if (!t_fields.is_array())
        return;
    for (const auto& field : t_fields.elements()) {
        if (!field.is_object() || !field.hasMember("name") || !field["name"].is_string())
            continue;
        const std::string name = field["name"].getString();
        if (!field.hasMember("offset") || (!field["offset"].isUint() && !field["offset"].isInt()))
            continue;
        const int rel = field["offset"].isUint() ? static_cast<int>(field["offset"].getUint()) : field["offset"].getInt();
        if (rel < 0)
            continue;
        const std::string path = t_db_prefix.empty() ? name : t_db_prefix + "." + name;
        const size_t abs_offset = t_base_offset + static_cast<size_t>(rel);

        uint32_t arr_count = 1;
        uint32_t arr_stride = 0; // 0 = Bit-packed Bool semantics (decoded specially)
        if (field.hasMember("count") && field["count"].isUint())
            arr_count = field["count"].getUint();
        if (arr_count > 1) {
            if (field.hasMember("struct_size") && field["struct_size"].isUint())
                arr_stride = field["struct_size"].getUint();
        }

        const bool has_children = field.hasMember("children") && field["children"].is_array() && !field["children"].empty();
        if (has_children) {
            // Struct (array): child offsets are relative to element 0. Leaves
            // keep the collapsed path but carry the array span so the decoder
            // can walk every element with the parent's per-element stride.
            const size_t before = t_out.size();
            collectTranscodeLeaves(field["children"], path, abs_offset, t_db_endian, t_out);
            if (arr_count > 1) {
                // struct_size missing/0 → derive from child extents
                // (max child offset + width), still an element span.
                if (arr_stride == 0) {
                    for (size_t i = before; i < t_out.size(); ++i) {
                        const size_t end = t_out[i].offset + s7TypeByteSize(t_out[i].type);
                        arr_stride = std::max(arr_stride, static_cast<uint32_t>(end - abs_offset));
                    }
                    arr_stride = std::max(arr_stride, 1u);
                }
                for (size_t i = before; i < t_out.size(); ++i) {
                    t_out[i].count = arr_count;
                    t_out[i].stride = arr_stride;
                }
            }
            continue;
        }
        if (!field.hasMember("type") || !field["type"].is_string())
            continue;
        auto type = sgrn::scl::parseDataType(field["type"].getString());
        if (!type.has_value())
            continue;
        const int bit = (field.hasMember("bit") && field["bit"].isUint()) ? static_cast<int>(field["bit"].getUint()) : 0;
        s7codec::Endian endian = t_db_endian;
        if (field.hasMember("endianness") && field["endianness"].is_string() &&
            std::string_view(field["endianness"].getString()) == "little") {
            endian = s7codec::Endian::Little;
        }
        TranscodeLeaf leaf{path, abs_offset, *type, bit, endian};
        if (arr_count > 1) {
            leaf.count = arr_count;
            leaf.stride = (*type == sgrn::scl::DataType::Bool) ? 0u : s7TypeByteSize(*type);
        }
        t_out.push_back(std::move(leaf));
    }
}

/// Builds db_number -> leaves from an embedded schema document. Accepts the
/// "dbs" member as either a list (with "number"/"name") or a keyed object.
bool buildTranscodeLayout(const sgrn::json::dom::Document& t_schema, std::unordered_map<uint16_t, std::vector<TranscodeLeaf>>& t_out) {
    if (!t_schema.is_object() || !t_schema.hasMember("dbs"))
        return false;
    const auto& dbs = t_schema["dbs"];
    auto handle_db = [&](const sgrn::json::dom::Value& t_db) {
        uint16_t db_num = 0;
        if (t_db.hasMember("number") && t_db["number"].isUint())
            db_num = static_cast<uint16_t>(t_db["number"].getUint());
        else
            return;
        std::string db_name = (t_db.hasMember("name") && t_db["name"].is_string()) ? t_db["name"].getString() : "";
        const std::string prefix = db_name.empty() ? fmt::format("DB{}", db_num) : db_name;
        s7codec::Endian db_endian = s7codec::Endian::Little;
        if (t_db.hasMember("endianness") && t_db["endianness"].is_string() &&
            std::string_view(t_db["endianness"].getString()) == "little") {
            db_endian = s7codec::Endian::Little;
        }
        if (!t_db.hasMember("fields"))
            return;
        std::vector<TranscodeLeaf> leaves;
        collectTranscodeLeaves(t_db["fields"], prefix, 0, db_endian, leaves);
        if (!leaves.empty())
            t_out[db_num] = std::move(leaves);
    };
    if (dbs.is_array()) {
        for (const auto& db : dbs.elements()) {
            if (db.is_object())
                handle_db(db);
        }
    } else if (dbs.is_object()) {
        for (auto it = dbs.memberBegin(); it != dbs.memberEnd(); ++it) {
            if (it->value.is_object())
                handle_db(it->value);
        }
    }
    return !t_out.empty();
}

/// Decodes one leaf to a typed JSON value (mirrors endian_helper::loadValue
/// reads, but type-preserving). Returns false when undecodable (out of
/// range, unsupported type, non-finite float) — caller skips the leaf.
bool decodeTranscodeLeaf(const uint8_t* t_image, size_t t_image_len, const TranscodeLeaf& t_leaf, sgrn::json::dom::Value& t_out,
    sgrn::json::dom::Document::AllocatorType& t_alloc) {
    using DT = sgrn::scl::DataType;
    using sgrn::common::endian_helper::loadFromBuffer;
    const size_t width = s7TypeByteSize(t_leaf.type);
    if (t_leaf.offset + width > t_image_len)
        return false;
    const uint8_t* p = t_image + t_leaf.offset;
    switch (t_leaf.type) {
        case DT::Bool:
            t_out.setBool(((p[0] >> t_leaf.bit) & 1) != 0);
            return true;
        case DT::Byte:
        case DT::USInt:
        case DT::Char:
            t_out.setUint(p[0]);
            return true;
        case DT::SInt:
            t_out.setInt(static_cast<int8_t>(p[0]));
            return true;
        case DT::Word:
        case DT::UInt:
            t_out.setUint(loadFromBuffer<uint16_t>(p, t_leaf.endian));
            return true;
        case DT::Int:
            t_out.setInt(loadFromBuffer<int16_t>(p, t_leaf.endian));
            return true;
        case DT::DWord:
        case DT::UDInt:
        case DT::Time:
        case DT::TimeOfDay:
            t_out.setUint64(loadFromBuffer<uint32_t>(p, t_leaf.endian));
            return true;
        case DT::DInt:
            t_out.setInt(loadFromBuffer<int32_t>(p, t_leaf.endian));
            return true;
        case DT::Real: {
            const float v = loadFromBuffer<float>(p, t_leaf.endian);
            if (std::isinf(v) || std::isnan(v))
                return false;
            t_out.setDouble(static_cast<double>(v));
            return true;
        }
        case DT::LWord:
        case DT::ULInt:
        case DT::LTime:
            t_out.setUint64(loadFromBuffer<uint64_t>(p, t_leaf.endian));
            return true;
        case DT::LInt:
            t_out.setInt64(loadFromBuffer<int64_t>(p, t_leaf.endian));
            return true;
        case DT::LReal: {
            const double v = loadFromBuffer<double>(p, t_leaf.endian);
            if (std::isinf(v) || std::isnan(v))
                return false;
            t_out.setDouble(v);
            return true;
        }
        case DT::DTL: {
            // S7 DTL (12 bytes): Year u16 | Month | Day | Weekday | Hour |
            // Minute | Second | Nanoseconds u32 → ISO-ish string the gateway
            // twin emits ("2000-01-31 23:59:00.000000000"). Unset/out-of-range
            // DTLs are skipped like other undecodable leaves.
            if (t_leaf.offset + 12 > t_image_len)
                return false;
            const uint16_t year = loadFromBuffer<uint16_t>(p, t_leaf.endian);
            const uint32_t nanos = loadFromBuffer<uint32_t>(p + 8, t_leaf.endian);
            if (year < 1 || year > 9999 || p[2] < 1 || p[2] > 12 || p[3] < 1 || p[3] > 31 || p[5] > 23 || p[6] > 59 || p[7] > 59)
                return false;
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u.%09u", static_cast<unsigned>(year), static_cast<unsigned>(p[2]),
                static_cast<unsigned>(p[3]), static_cast<unsigned>(p[5]), static_cast<unsigned>(p[6]), static_cast<unsigned>(p[7]), nanos);
            // Copy into allocator-owned storage: SetString(buf, len) without
            // an allocator stores the (stack) pointer — dangling on return.
            t_out.setString(buf, static_cast<sgrn::json::dom::SizeType>(std::strlen(buf)), t_alloc);
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
        const std::string schema_bytes = t_decompressed.substr(10, schema_len);
        if (sgrn::scl::isBinarySchemaPayload(schema_bytes)) {
            // v4 header: decode the binary schema to its JSON form so the
            // schema line below and the layout builder work unchanged.
            auto store_res = sgrn::scl::PlcSchemaStore::loadFromBinary(schema_bytes);
            if (store_res.hasError()) {
                fmt::print(fg(fmt::color::yellow), "[sgrn_dataset] Cannot decode binary schema from {} ({}), emitting metadata records\n",
                    t_source_name, sgrn::scl::toString(store_res.error()));
            } else {
                schema_json = std::move(store_res).value().toJson();
            }
        } else {
            schema_json = schema_bytes;
        }
    }
    if (!schema_json.empty()) {
        sgrn::json::dom::StringBuffer sb;
        sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> w(sb);
        w.startObject();
        w.key("type");
        w.string("schema");
        w.key("schema");
        w.rawJson(schema_json.c_str(), schema_json.size(), sgrn::json::dom::Type::Object);
        w.endObject();
        t_emit(sb.getString());
    }

    // Value-preserving state: schema layout for decoding, dictionary maps
    // for ID resolution, and per-DB images for delta patching. Without a
    // usable schema + dictionary the transcoder falls back to metadata-only
    // synthetic records (historic behavior), never dropping frames.
    std::unordered_map<uint16_t, std::vector<TranscodeLeaf>> layout;
    bool have_layout = false;
    // Function scope: decode_image borrows schema_doc's allocator for
    // allocator-owned (copied) leaf strings (e.g. DTL timestamps).
    sgrn::json::dom::Document schema_doc;
    if (!schema_json.empty()) {
        if (!schema_doc.parse(schema_json.c_str()).hasParseError() && schema_doc.is_object()) {
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
                           std::vector<std::pair<std::string, sgrn::json::dom::Value>> t_entries) {
        sgrn::json::dom::StringBuffer sb;
        sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> w(sb);
        w.startObject();
        w.key("type");
        w.string(t_type);
        w.key("ts");
        w.int64(t_ts);
        w.key(t_payload_key);
        w.startObject();
        for (auto& [id_str, val] : t_entries) {
            w.key(id_str.c_str(), static_cast<sgrn::json::dom::SizeType>(id_str.size()));
            val.accept(w);
        }
        w.endObject();
        w.endObject();
        t_emit(sb.getString());
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
            sgrn::json::dom::Document control_doc;
            if (!control_doc.parse(control_json.c_str()).hasParseError() && control_doc.is_object()) {
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
            std::vector<std::pair<std::string, sgrn::json::dom::Value>> entries;
            if (!have_layout || path_to_id.empty())
                return entries;
            auto layout_it = layout.find(t_db);
            if (layout_it == layout.end())
                return entries;
            for (const auto& leaf : layout_it->second) {
                auto id_it = path_to_id.find(leaf.path);
                if (id_it == path_to_id.end())
                    continue;
                // Tombstone span for one leaf, scaled to its array when the
                // WAL dictionary leaf fans out over elements. Bool arrays
                // pack bits starting at leaf.bit (stride 0).
                auto leaf_span_bytes = [&](const TranscodeLeaf& t_leaf) -> size_t {
                    if (t_leaf.count <= 1)
                        return s7TypeByteSize(t_leaf.type);
                    if (t_leaf.type == sgrn::scl::DataType::Bool && t_leaf.stride == 0)
                        return static_cast<size_t>(t_leaf.bit + static_cast<int>(t_leaf.count) - 1) / 8 + 1;
                    return static_cast<size_t>(t_leaf.count) * t_leaf.stride;
                };
                if (t_changed_runs != nullptr) {
                    const size_t span = leaf_span_bytes(leaf);
                    bool touched = false;
                    for (const auto& run : *t_changed_runs) {
                        if (leaf.offset < run.offset + run.len && run.offset < leaf.offset + span) {
                            touched = true;
                            break;
                        }
                    }
                    if (!touched)
                        continue;
                }
                sgrn::json::dom::Value val;
                auto& alloc = schema_doc.allocator();
                if (leaf.count > 1) {
                    // Emit the full array under the collapsed dictionary id:
                    // readers fan each element into its "a.b[i].c" column.
                    val.setArray();
                    for (uint32_t i = 0; i < leaf.count; ++i) {
                        TranscodeLeaf el = leaf;
                        if (leaf.type == sgrn::scl::DataType::Bool && leaf.stride == 0) {
                            const int total_bit = leaf.bit + static_cast<int>(i);
                            el.offset += static_cast<size_t>(total_bit / 8);
                            el.bit = total_bit % 8;
                        } else {
                            el.offset += static_cast<size_t>(i) * leaf.stride;
                        }
                        sgrn::json::dom::Value ev;
                        if (decodeTranscodeLeaf(t_image, t_image_len, el, ev, alloc))
                            val.pushBack(std::move(ev), alloc);
                        else
                            val.pushBack(sgrn::json::dom::Value().setNull(), alloc);
                    }
                } else if (decodeTranscodeLeaf(t_image, t_image_len, leaf, val, alloc)) {
                    entries.emplace_back(std::to_string(id_it->second), std::move(val));
                    continue;
                } else {
                    continue;
                }
                entries.emplace_back(std::to_string(id_it->second), std::move(val));
            }
            return entries;
        };

        // Legacy synthetic record for frames decoded without schema or
        // dictionary (historic behavior): metadata only, no values.
        auto emit_synthetic = [&](const char* t_marker, int64_t t_ts, uint16_t t_db) {
            sgrn::json::dom::StringBuffer sb;
            sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> w(sb);
            w.startObject();
            w.key("type");
            w.string("anchor");
            w.key("ts");
            w.int64(t_ts);
            w.key("db");
            w.uint32(t_db);
            if (t_marker != nullptr) {
                w.key(t_marker);
                w.boolean(true);
            }
            w.key("bytes_len");
            w.uint32(payload_len);
            w.endObject();
            t_emit(sb.getString());
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
