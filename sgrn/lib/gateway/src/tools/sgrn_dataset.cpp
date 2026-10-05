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
// Shared binary-WAL decode primitives (single implementation in
// database/PersistenceService.hpp, used by all readers).
using sgrn::gateway::database::findNextAnchorFrame;
using sgrn::gateway::database::isDictionaryRecord;
using sgrn::gateway::database::parseDictionaryLine;

// Transcode unit (sgrn_dataset_transcode.cpp): declared here so this TU can
// call across the split — the definition lives in transcode_detail there.
namespace transcode_detail
{
size_t s7TypeByteSize(sgrn::scl::DataType t_type);
} // namespace transcode_detail
using namespace transcode_detail;

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

    auto is_string_like = [](sgrn::scl::DataType t) {
        return t == sgrn::scl::DataType::String || t == sgrn::scl::DataType::WString || t == sgrn::scl::DataType::XString ||
               t == sgrn::scl::DataType::XWString;
    };

    std::function<void(const sgrn::scl::DbField&, const std::string&, int, uint16_t, const std::string&)> add_leaf =
        [&](const sgrn::scl::DbField& field, const std::string& path, int abs_offset, uint16_t db_num, const std::string& db_name) {
            FeatureMeta meta;
            meta.db_name = db_name;
            meta.field_path = path;
            meta.full_name = fmt::format("{}.{}", db_name, path);
            meta.data_type = s7codec::s7TypeToString(field.type);
            meta.unit = field.unit.value_or("");
            meta.dimension = field.dimension.value_or("");
            meta.is_categorical = isCategoricalType(field.type);
            meta.db_num = db_num;
            meta.offset = static_cast<size_t>(abs_offset);
            meta.bit_index = field.bit_index;
            meta.raw_type = field.type;

            if (!field.enum_map.empty() || sgrn::scl::kind_of(field) == sgrn::scl::FieldKind::Enum) {
                meta.is_categorical = true;
                meta.data_type = "ENUM";
                meta.enum_map = field.enum_map;
            }

            feature_index_map_[meta.full_name] = features_.size();
            features_.push_back(std::move(meta));
        };

    // Recursive walk with array expansion: array-of-struct and primitive
    // arrays fan out to one feature per element ("path[i].leaf" / "path[i]"),
    // so every element is decodable from binary images and addressable from
    // JSON leaves. Matches the WS leaf-dictionary wire contract.
    std::function<void(const std::vector<sgrn::scl::DbField>&, const std::string&, int, uint16_t, const std::string&)> walk =
        [&](const std::vector<sgrn::scl::DbField>& fields, const std::string& prefix, int base, uint16_t db_num,
            const std::string& db_name) {
            for (const auto& field : fields) {
                const std::string p = prefix.empty() ? field.name : prefix + "." + field.name;
                const int abs = base + field.offset;
                const bool is_arr = field.count > 1 && !is_string_like(field.type) && field.type != sgrn::scl::DataType::String;
                if (!field.children.empty() && !is_arr) {
                    walk(field.children, p, abs, db_num, db_name);
                    continue;
                }
                if (!field.children.empty() && is_arr) {
                    // Struct array: stride is the per-element span.
                    const int stride = std::max(1, static_cast<int>(field.struct_size));
                    const uint32_t n = std::min(field.count, 4096u);
                    for (uint32_t i = 0; i < n; ++i)
                        walk(field.children, p + "[" + std::to_string(i) + "]", abs + static_cast<int>(i * stride), db_num, db_name);
                    continue;
                }
                if (field.children.empty() && is_arr) {
                    // Primitive array: expand per element (bool arrays are
                    // bit-packed from bit_index).
                    const uint32_t n = std::min(field.count, 4096u);
                    if (field.type == sgrn::scl::DataType::Bool) {
                        for (uint32_t i = 0; i < n; ++i) {
                            const int total_bit = static_cast<int>(field.bit_index) + static_cast<int>(i);
                            sgrn::scl::DbField el = field;
                            el.count = 1;
                            el.bit_index = static_cast<uint8_t>(total_bit % 8);
                            add_leaf(el, p + "[" + std::to_string(i) + "]", abs + total_bit / 8, db_num, db_name);
                        }
                    } else {
                        const int stride = std::max(1, static_cast<int>(s7TypeByteSize(field.type)));
                        for (uint32_t i = 0; i < n; ++i) {
                            sgrn::scl::DbField el = field;
                            el.count = 1;
                            add_leaf(el, p + "[" + std::to_string(i) + "]", abs + static_cast<int>(i * stride), db_num, db_name);
                        }
                    }
                    continue;
                }
                add_leaf(field, p, abs, db_num, db_name);
            }
        };

    for (const auto& db_pair : schema_store_.dbs()) {
        const uint16_t db_num = db_pair.first;
        const auto& db = db_pair.second;
        walk(db.fields, "", 0, db_num, db.db_name);
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

        auto splitDots = [](const std::string& t_s) {
            std::vector<std::string> out;
            std::string cur;
            for (char c : t_s) {
                if (c == '.') {
                    out.push_back(cur);
                    cur.clear();
                } else {
                    cur.push_back(c);
                }
            }
            out.push_back(cur);
            return out;
        };

        auto storeScalar = [&](const std::string& key, const rapidjson::Value& v) {
            std::string val_str;
            if (v.IsString())
                val_str = v.GetString();
            else if (v.IsNumber())
                val_str = std::to_string(v.GetDouble());
            else if (v.IsBool())
                val_str = v.GetBool() ? "true" : "false";
            else if (v.IsNull())
                val_str = "null";
            else {
                rapidjson::StringBuffer sb;
                rapidjson::Writer<rapidjson::StringBuffer> w(sb);
                v.Accept(w);
                val_str = sb.GetString();
            }
            current_state_[key] = val_str;
        };

        // Store one leaf; array values fan out to the indexed schema features
        // ("a.b[i].c"), resolved against feature_index_map_ so the keys match
        // the CSV columns exactly. Unknown shapes are skipped, as before.
        // walkNested is assigned below; object (elements) recurse through it.
        std::function<void(const std::string&, const rapidjson::Value&)> walkNested;
        auto storeLeaf = [&](const std::string& key, const rapidjson::Value& v) {
            if (v.IsObject()) {
                if (walkNested)
                    walkNested(key, v);
                return;
            }
            if (!v.IsArray()) {
                if (v.IsString() || v.IsNumber() || v.IsBool())
                    storeScalar(key, v);
                return;
            }
            const auto segs = splitDots(key);
            for (size_t p = 0; p < segs.size(); ++p) {
                // candidate with the array index at segment p:
                // "<segs[0..p]>[0]<.rest>"
                std::string cand;
                for (size_t k = 0; k <= p; ++k)
                    cand += (k ? "." : "") + segs[k];
                cand += "[0]";
                for (size_t k = p + 1; k < segs.size(); ++k)
                    cand += "." + segs[k];
                if (feature_index_map_.find(cand) == feature_index_map_.end())
                    continue;
                for (rapidjson::SizeType i = 0; i < v.Size(); ++i) {
                    std::string ikey;
                    for (size_t k = 0; k <= p; ++k)
                        ikey += (k ? "." : "") + segs[k];
                    ikey += "[" + std::to_string(i) + "]";
                    for (size_t k = p + 1; k < segs.size(); ++k)
                        ikey += "." + segs[k];
                    if (feature_index_map_.find(ikey) == feature_index_map_.end())
                        continue;
                    const auto& el = v[i];
                    if (el.IsString() || el.IsNumber() || el.IsBool() || el.IsNull())
                        storeScalar(ikey, el);
                    else if (el.IsObject() && walkNested)
                        walkNested(ikey, el);
                }
                return;
            }
        };

        walkNested = [&](const std::string& prefix, const rapidjson::Value& v) {
            if (!v.IsObject()) {
                storeLeaf(prefix, v);
                return;
            }
            for (auto m = v.MemberBegin(); m != v.MemberEnd(); ++m)
                walkNested(prefix.empty() ? m->name.GetString() : prefix + "." + m->name.GetString(), m->value);
        };

        auto ingestKeyedValues = [&](const rapidjson::Value& t_obj) {
            for (auto it = t_obj.MemberBegin(); it != t_obj.MemberEnd(); ++it) {
                std::string key = it->name.GetString();

                if (!t_dict.empty() && std::all_of(key.begin(), key.end(), ::isdigit)) {
                    size_t id = std::stoul(key);
                    if (id < t_dict.size() && !t_dict[id].empty())
                        key = t_dict[id];
                }

                storeLeaf(key, it->value);
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
                    if (!db_it->value.IsObject())
                        continue;
                    walkNested(db_it->name.GetString(), db_it->value);
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
                        std::string val_str = sgrn::common::endian_helper::loadValue(
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

        writer.Key("dimension");
        writer.String(feat.dimension.c_str());

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

} // namespace sgrn::gateway::tools
