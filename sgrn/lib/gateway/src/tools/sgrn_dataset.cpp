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
#include <limits>
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
    db_endianness_.clear();

    auto is_string_like = [](sgrn::scl::DataType t) {
        return t == sgrn::scl::DataType::String || t == sgrn::scl::DataType::WString || t == sgrn::scl::DataType::XString ||
               t == sgrn::scl::DataType::XWString;
    };

    std::function<void(const sgrn::scl::DbField&, const std::string&, int, uint16_t, const std::string&)> add_leaf =
        [&](const sgrn::scl::DbField& field, const std::string& path, int abs_offset, uint16_t db_num, const std::string& db_name) {
            // #TRANSIENT leaves are excluded from the WAL by design and stay
            // out of datasets. #LABEL leaves keep their CSV column (models
            // need targets) but are flagged in the manifest so trainers can
            // exclude them from X (see trainAndSelectBest target_feature).
            if (field.is_transient)
                return;
            FeatureMeta meta;
            meta.is_label = field.is_label;
            meta.db_name = db_name;
            meta.field_path = path;
            meta.full_name = fmt::format("{}.{}", db_name, path);
            meta.data_type = s7codec::s7TypeToString(field.type);
            meta.unit = field.unit.value_or("");
            meta.dimension = field.dimension.value_or("");
            meta.description = field.description.value_or("");
            if (field.precision.has_value())
                meta.precision = field.precision.value();
            if (field.nominal.has_value()) {
                meta.nominal = field.nominal.value();
                meta.has_nominal = true;
            }
            if (field.alarm_lo.has_value() && field.alarm_hi.has_value()) {
                meta.alarm_lo = field.alarm_lo.value();
                meta.alarm_hi = field.alarm_hi.value();
                meta.has_alarm = true;
            }
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
        db_endianness_[db_num] = db.endianness;
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

/// Parse a rendered DTL string ("2000-01-31 03:00:00.000000000", the
/// fixed-width format endian_helper emits) into a naive wall-clock epoch
/// in ms — days-from-civil, timezone-free by construction, since the
/// simulated clock is an absolute wall datetime plus ticks. Returns 0 when
/// the string is not a valid settled DTL ("null", year-0 startup rows) so
/// callers can fall back to the WAL frame stamp.
static int64_t parseDtlWallEpochMs(const std::string& t_s) {
    if (t_s.size() != 29 || t_s[4] != '-' || t_s[7] != '-' || t_s[10] != ' ' ||
        t_s[13] != ':' || t_s[16] != ':' || t_s[19] != '.')
        return 0;
    auto digits = [&t_s](size_t t_begin, size_t t_end) -> int {
        int v = 0;
        for (size_t i = t_begin; i < t_end; ++i) {
            if (t_s[i] < '0' || t_s[i] > '9')
                return -1;
            v = v * 10 + (t_s[i] - '0');
        }
        return v;
    };
    const int year = digits(0, 4), month = digits(5, 7), day = digits(8, 10);
    const int hour = digits(11, 13), minute = digits(14, 16), second = digits(17, 19);
    const int ms = digits(20, 23); // leading ns digits → ms
    if (year < 1900 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59 || ms < 0)
        return 0;
    // Howard Hinnant's days_from_civil
    int64_t y = year - (month <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy =
        (153u * static_cast<unsigned>(month + (month > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(day) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;
    return ((days * 24 + hour) * 60 + minute) * 60000 + second * 1000 + ms;
}

static bool isBinaryArchiveContent(const std::string& t_content) {
    return t_content.size() >= 4 && t_content[0] == 'S' && t_content[1] == 'G' && t_content[2] == 'R' &&
           t_content[3] == 'N';
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

    auto writers = openWriters(t_config);
    if (writers.hasError()) {
        return writers.error();
    }

    summary_ = DatasetSummary{};
    summary_.features = features_;

    // Index the features that carry the simulated clock (each DB's
    // "…timestamp" DTL leaf is written every tick): resolveRowTs reads the
    // current state through them, falling back to the WAL frame stamp.
    dtl_ts_feature_idx_.clear();
    for (size_t i = 0; i < features_.size(); ++i) {
        const std::string& n = features_[i].full_name;
        if (n.size() > 10 && n.compare(n.size() - 10, 10, ".timestamp") == 0)
            dtl_ts_feature_idx_.push_back(i);
    }
    cache_rows_ = 0;
    last_images_.clear();
    deferred_row_ts_ = 0;
    row_pending_ = false;

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

        if (isBinaryArchiveContent(decompressed))
            processBinaryArchive(file_path, decompressed);
        else
            processJsonlArchive(file_path, decompressed);
        ++summary_.total_files_processed;
    }

    // Emit the final deferred binary-frame row (ts change never happened).
    flushPendingRow();

    closeWriters(t_config);

    if (!t_config.manifest_path.empty()) {
        (void)generateManifest(t_config.manifest_path, summary_);
    }

    return summary_;
}

sgrn::Result<void> DatasetProcessor::openWriters(const DatasetConfig& t_config) {
    if (!t_config.output_csv_path.empty()) {
        csv_out_.open(t_config.output_csv_path, std::ios::out | std::ios::trunc);
        if (!csv_out_.is_open()) {
            return fmt::format("Failed to open CSV output file: {}", t_config.output_csv_path);
        }
        // Write Header
        csv_out_ << "timestamp_ms";
        for (const auto& feat : features_) {
            csv_out_ << "," << feat.full_name;
        }
        csv_out_ << "\n";
    }

    // Streaming cache (compact numeric twin of the CSV rows, no schema/
    // headers to parse downstream): fixed-order float32 feature rows plus
    // per-row timestamps. Written at the same row boundary as the CSV,
    // so both stay row-identical when produced together.
    if (t_config.output_cache_prefix.empty())
        return {};
    cache_f32_.open(t_config.output_cache_prefix + ".f32", std::ios::binary | std::ios::trunc);
    cache_ts_.open(t_config.output_cache_prefix + ".ts.i64", std::ios::binary | std::ios::trunc);
    if (!cache_f32_.is_open() || !cache_ts_.is_open()) {
        return fmt::format("Failed to open cache output files under prefix: {}", t_config.output_cache_prefix);
    }
    return {};
}

void DatasetProcessor::closeWriters(const DatasetConfig& t_config) {
    if (csv_out_.is_open())
        csv_out_.close();

    // Cache metadata: feature order/names so downstream streaming readers
    // can interpret (<prefix>.f32 rows × features, .ts.i64 per-row stamp).
    if (!cache_f32_.is_open())
        return;
    cache_f32_.close();
    cache_ts_.flush();
    cache_ts_.close();
    rapidjson::Document meta_doc(rapidjson::kObjectType);
    auto& alloc = meta_doc.GetAllocator();
    meta_doc.AddMember("format", rapidjson::Value("sgrn-window-cache/1"), alloc);
    // Row timestamps: naive wall epoch from the datablocks' DTLs (see
    // resolveRowTs) — deterministic across machines, no frame-stamp tz.
    meta_doc.AddMember("ts_domain", rapidjson::Value("dtl_wall_naive"), alloc);
    meta_doc.AddMember("rows", rapidjson::Value(static_cast<uint64_t>(cache_rows_)), alloc);
    meta_doc.AddMember("features", rapidjson::Value(static_cast<uint64_t>(summary_.features.size())), alloc);
    rapidjson::Value names(rapidjson::kArrayType);
    rapidjson::Value labels(rapidjson::kArrayType);
    for (const auto& feat : summary_.features) {
        names.PushBack(rapidjson::Value(feat.full_name.c_str(), alloc), alloc);
        labels.PushBack(rapidjson::Value(feat.is_label), alloc);
    }
    meta_doc.AddMember("feature_names", names, alloc);
    meta_doc.AddMember("is_label", labels, alloc);
    rapidjson::StringBuffer meta_sb;
    rapidjson::Writer<rapidjson::StringBuffer> meta_w(meta_sb);
    meta_doc.Accept(meta_w);
    std::ofstream cache_meta(std::string(t_config.output_cache_prefix) + ".json", std::ios::out | std::ios::trunc);
    if (cache_meta.is_open())
        cache_meta << meta_sb.GetString() << "\n";
    fmt::print("[sgrn_dataset] Streaming cache written: {}.f32 ({} rows x {} features) + ts + json\n",
        t_config.output_cache_prefix, cache_rows_, summary_.features.size());
}

/// Split a dotted key into its segments ("a.b.c" → {"a","b","c"}).
static std::vector<std::string> splitDots(const std::string& t_s) {
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
}

int64_t DatasetProcessor::resolveRowTs(int64_t t_frame_ts) const {
    for (size_t idx : dtl_ts_feature_idx_) {
        auto st_it = current_state_.find(features_[idx].full_name);
        if (st_it == current_state_.end())
            continue;
        const int64_t dtl_ms = parseDtlWallEpochMs(st_it->second);
        if (dtl_ms > 0)
            return dtl_ms;
    }
    return t_frame_ts;
}

void DatasetProcessor::deferRow(int64_t t_ts) {
    if (row_pending_ && t_ts != deferred_row_ts_)
        emitRow(deferred_row_ts_);
    deferred_row_ts_ = t_ts;
    row_pending_ = true;
}

void DatasetProcessor::flushPendingRow() {
    if (row_pending_) {
        emitRow(deferred_row_ts_);
        row_pending_ = false;
    }
}

/// One row from the carried-forward state, shared by the binary and
/// JSONL branches so both formats produce identical row semantics.
void DatasetProcessor::emitRow(int64_t t_ts) {
    if (!csv_out_.is_open() && !cache_f32_.is_open())
        return;
    if (csv_out_.is_open())
        csv_out_ << resolveRowTs(t_ts); // DTL wall clock — identical to the cache ts
    for (size_t f_idx = 0; f_idx < summary_.features.size(); ++f_idx) {
        auto& feat = summary_.features[f_idx];
        auto st_it = current_state_.find(feat.full_name);
        float cache_val = std::nanf("");
        if (st_it != current_state_.end()) {
            const std::string& raw = st_it->second;
            // CSV cells must stay printable ASCII: decodes of unseeded
            // memory can yield raw byte garbage (e.g. String/Char leaves
            // in startup anchor frames) that would break UTF-8 parsers.
            bool printable = true;
            for (const unsigned char c : raw) {
                if (c < 0x20 || c > 0x7E) {
                    printable = false;
                    break;
                }
            }
            if (printable) {
                if (csv_out_.is_open())
                    csv_out_ << "," << raw;
            } else {
                if (csv_out_.is_open())
                    csv_out_ << ",";
                feat.null_count++;
                feat.total_samples++;
                if (cache_f32_.is_open())
                    cache_f32_.write(reinterpret_cast<const char*>(&cache_val), sizeof(cache_val));
                continue;
            }
            feat.total_samples++;

            double dval = 0.0;
            if (raw == "true" || raw == "TRUE")
                dval = 1.0;
            else if (raw == "false" || raw == "FALSE")
                dval = 0.0;
            else {
                try {
                    size_t parsed_to = 0;
                    dval = std::stod(raw, &parsed_to);
                    if (parsed_to == 0)
                        dval = std::numeric_limits<double>::quiet_NaN();
                } catch (...) {
                    dval = std::numeric_limits<double>::quiet_NaN();
                }
            }
            cache_val = static_cast<float>(dval);
            if (std::isnan(cache_val) || std::isinf(cache_val))
                cache_val = std::nanf("");

            if (!std::isnan(dval)) {
                if (!feat.min_max_set || dval < feat.min_val)
                    feat.min_val = dval;
                if (!feat.min_max_set || dval > feat.max_val)
                    feat.max_val = dval;
                feat.min_max_set = true;
            }
        } else {
            if (csv_out_.is_open())
                csv_out_ << ",";
            feat.null_count++;
        }
        if (cache_f32_.is_open())
            cache_f32_.write(reinterpret_cast<const char*>(&cache_val), sizeof(cache_val));
    }
    if (csv_out_.is_open())
        csv_out_ << "\n";
    if (cache_f32_.is_open()) {
        const int64_t row_ts_ms = resolveRowTs(t_ts);
        const int64_t ts_out = (row_ts_ms > 0) ? row_ts_ms : std::numeric_limits<int64_t>::min();
        cache_ts_.write(reinterpret_cast<const char*>(&ts_out), sizeof(ts_out));
        ++cache_rows_;
    }
}
void DatasetProcessor::storeStateValue(const std::string& t_key, const rapidjson::Value& t_v) {
    std::string val_str;
    if (t_v.IsString())
        val_str = t_v.GetString();
    else if (t_v.IsNumber())
        val_str = std::to_string(t_v.GetDouble());
    else if (t_v.IsBool())
        val_str = t_v.GetBool() ? "true" : "false";
    else if (t_v.IsNull())
        val_str = "null";
    else {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        t_v.Accept(w);
        val_str = sb.GetString();
    }
    current_state_[t_key] = val_str;
}

void DatasetProcessor::walkStateObject(const std::string& t_prefix, const rapidjson::Value& t_v) {
    if (!t_v.IsObject()) {
        storeStateLeaf(t_prefix, t_v);
        return;
    }
    for (auto m = t_v.MemberBegin(); m != t_v.MemberEnd(); ++m)
        walkStateObject(t_prefix.empty() ? m->name.GetString() : t_prefix + "." + m->name.GetString(), m->value);
}

/// Store one leaf; array values fan out to the indexed schema features
/// ("a.b[i].c"), resolved against feature_index_map_ so the keys match
/// the CSV columns exactly. Unknown shapes are skipped, as before.
void DatasetProcessor::storeStateLeaf(const std::string& t_key, const rapidjson::Value& t_v) {
    if (t_v.IsObject()) {
        walkStateObject(t_key, t_v);
        return;
    }
    if (!t_v.IsArray()) {
        if (t_v.IsString() || t_v.IsNumber() || t_v.IsBool())
            storeStateValue(t_key, t_v);
        return;
    }
    const auto segs = splitDots(t_key);
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
        for (rapidjson::SizeType i = 0; i < t_v.Size(); ++i) {
            std::string ikey;
            for (size_t k = 0; k <= p; ++k)
                ikey += (k ? "." : "") + segs[k];
            ikey += "[" + std::to_string(i) + "]";
            for (size_t k = p + 1; k < segs.size(); ++k)
                ikey += "." + segs[k];
            if (feature_index_map_.find(ikey) == feature_index_map_.end())
                continue;
            const auto& el = t_v[i];
            if (el.IsString() || el.IsNumber() || el.IsBool() || el.IsNull())
                storeStateValue(ikey, el);
            else if (el.IsObject())
                walkStateObject(ikey, el);
        }
        return;
    }
}

void DatasetProcessor::ingestKeyedValues(const rapidjson::Value& t_obj, const std::vector<std::string>& t_dict) {
    for (auto it = t_obj.MemberBegin(); it != t_obj.MemberEnd(); ++it) {
        std::string key = it->name.GetString();

        if (!t_dict.empty() && std::all_of(key.begin(), key.end(), ::isdigit)) {
            size_t id = std::stoul(key);
            if (id < t_dict.size() && !t_dict[id].empty())
                key = t_dict[id];
        }

        storeStateLeaf(key, it->value);
    }
}

/// Ingests one parsed JSONL-shape document (anchor / delta / legacy flat
/// / control anchor): numeric ID keys resolve through t_dict, then the
/// shared row is deferred until the timestamp changes. Returns false for
/// timestamp-less lines (schema / dictionary / manifest / footer), which
/// carry no data. Used by the JSONL branch and, for JSON anchor controls,
/// by the binary branch — so transcoded output and direct reads agree row
/// for row.
bool DatasetProcessor::ingestJsonRecord(const rapidjson::Document& t_doc, const std::vector<std::string>& t_dict) {
    int64_t ts = 0;
    if (t_doc.HasMember("ts") && t_doc["ts"].IsInt64()) {
        ts = t_doc["ts"].GetInt64();
    } else if (t_doc.HasMember("timestamp") && t_doc["timestamp"].IsInt64()) {
        ts = t_doc["timestamp"].GetInt64();
    }
    if (ts == 0)
        return false;

    if (summary_.start_timestamp_ms == 0 || ts < summary_.start_timestamp_ms) {
        summary_.start_timestamp_ms = ts;
    }
    if (ts > summary_.end_timestamp_ms) {
        summary_.end_timestamp_ms = ts;
    }

    // 1. Delta record: {"type":"delta","ts":123,"changes":{"<id>":val,...}}
    if (t_doc.HasMember("changes") && t_doc["changes"].IsObject()) {
        ingestKeyedValues(t_doc["changes"], t_dict);
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
                walkStateObject(db_it->name.GetString(), db_it->value);
            }
        } else {
            ingestKeyedValues(data, t_dict);
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

    // Merged archives transcode binary frames into one JSONL line per
    // dirty DB per tick; defer the row write so co-timestamp lines merge
    // into one complete row (rows carry the carried-forward state).
    deferRow(ts);
    ++summary_.total_timestamps;
    ++summary_.total_records_processed;
    return true;
}

/// Merge one binary DB frame image's decodable leaves into the
/// carried-forward state (same semantics as the JSONL ingest). The actual
/// row write is deferred until the timestamp changes so co-timestamp
/// per-DB frames land in one complete row. Undecodable leaves ("null")
/// stay absent rather than poisoning the row.
void DatasetProcessor::ingestBinaryImage(uint16_t t_db, const uint8_t* t_image, size_t t_len) {
    for (auto& feat : features_) {
        if (feat.db_num != t_db || feat.offset + s7TypeByteSize(feat.raw_type) > t_len) {
            continue;
        }
        std::string val_str = sgrn::common::endian_helper::loadValue(t_image + feat.offset, feat.raw_type,
            db_endianness_.count(t_db) ? db_endianness_.at(t_db) : s7codec::Endian::Big, feat.bit_index);
        if (val_str == "null")
            continue;
        current_state_[feat.full_name] = val_str;
    }
}
void DatasetProcessor::processBinaryArchive(const std::filesystem::path& t_path, const std::string& t_content) {
    database::BinaryWalHeader header;
    if (database::checkBinaryWalHeader(t_content, header) != database::BinaryHeaderStatus::kOk) {
        uint16_t ver = 0;
        if (t_content.size() >= 6)
            std::memcpy(&ver, t_content.data() + 4, sizeof(ver));
        fmt::print(fg(fmt::color::yellow), "[sgrn_dataset] Unsupported binary version {} in {}, skipping file.\n", ver,
            t_path.string());
        return;
    }
    size_t pos = header.frames_start;

    // File-local dictionary for JSON anchor controls (reset per file,
    // like the JSONL branch's path_by_id).
    std::vector<std::string> bin_paths;

    // On stream corruption, jump forward to the next verifiable anchor
    // instead of abandoning the file; fall back to stopping cleanly when
    // no anchor follows. Returns false to break.
    auto resync_or_stop = [&](size_t t_frame_start) {
        auto found = findNextAnchorFrame(t_content, t_frame_start);
        if (!found)
            return false;
        int64_t anchor_ts = 0;
        std::memcpy(&anchor_ts, t_content.data() + *found, sizeof(anchor_ts));
        fmt::print(fg(fmt::color::yellow), "[sgrn_dataset] Corruption at byte {} in {}, resumed at anchor ts={}\n", t_frame_start,
            t_path.string(), anchor_ts);
        pos = *found;
        return true;
    };

    database::BinaryFrame fr;
    while (true) {
        const auto status = database::decodeBinaryFrame(t_content, pos, fr);
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
                t_content.size() - pos, t_path.string());
            break;
        }

        // Control frames carry JSON WAL lines. Dictionary lines build the
        // file-local id map; JSON anchor lines carry real snapshot state
        // (e.g. startup, before any binary anchor exists) and are ingested
        // exactly like JSONL lines so direct reads agree with transcoded
        // output row for row. Everything else skips.
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

        // Resolve the frame to a full DB image: full frames replace the
        // cached image, verifiable anchors (v3+) replace it after a CRC
        // check, delta frames (v2+) patch runs into it.
        uint16_t target_db = db_num;
        const uint8_t* image = payload;
        size_t image_len = payload_len;
        if (db_num == sgrn::gateway::database::kAnchorFrameDbNum) {
            const uint8_t* anchor_image = nullptr;
            size_t anchor_len = 0;
            uint16_t anchor_db = 0;
            if (!sgrn::gateway::database::verifyAnchorFrame(payload, payload_len, anchor_db, anchor_image, anchor_len)) {
                fmt::print(
                    fg(fmt::color::yellow), "[sgrn_dataset] Anchor CRC mismatch in {}, seeking resync\n", t_path.string());
                if (resync_or_stop(frame_start))
                    continue;
                break;
            }
            last_images_[anchor_db].assign(anchor_image, anchor_image + anchor_len);
            target_db = anchor_db;
            image = last_images_[anchor_db].data();
            image_len = anchor_len;
        } else if (db_num == sgrn::gateway::database::kDeltaFrameDbNum) {
            // Delta for a DB with no cached keyframe yet (e.g. a file
            // starting mid-stream): nothing to patch, skip silently.
            // Structurally corrupt runs warn below.
            uint16_t delta_db = 0;
            std::vector<sgrn::gateway::database::BinaryDeltaRun> runs;
            auto img_it = last_images_.end();
            bool have_image = false;
            if (payload_len >= 2) {
                uint16_t candidate = 0;
                std::memcpy(&candidate, payload, sizeof(candidate));
                img_it = last_images_.find(candidate);
                have_image = (img_it != last_images_.end());
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
                        fg(fmt::color::yellow), "[sgrn_dataset] Corrupt delta frame in {}, seeking resync\n", t_path.string());
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
            last_images_[db_num].assign(payload, payload + payload_len);
        }

        if (ts > 0) {
            if (summary_.start_timestamp_ms == 0 || ts < summary_.start_timestamp_ms)
                summary_.start_timestamp_ms = ts;
            if (ts > summary_.end_timestamp_ms)
                summary_.end_timestamp_ms = ts;
        }

        if (csv_out_.is_open() || cache_f32_.is_open())
            ingestBinaryImage(target_db, image, image_len);
        deferRow(ts);

        summary_.total_timestamps++;
        summary_.total_records_processed++;
    }
}

void DatasetProcessor::processJsonlArchive(const std::filesystem::path& t_path, const std::string& t_content) {
    std::istringstream stream(t_content);
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

        writer.Key("is_label");
        writer.Bool(feat.is_label);

        if (!feat.description.empty()) {
            writer.Key("description");
            writer.String(feat.description.c_str());
        }
        if (feat.precision >= 0) {
            writer.Key("precision");
            writer.Int(feat.precision);
        }
        if (feat.has_nominal) {
            writer.Key("nominal");
            writer.Double(feat.nominal);
        }
        if (feat.has_alarm) {
            writer.Key("alarm_lo");
            writer.Double(feat.alarm_lo);
            writer.Key("alarm_hi");
            writer.Double(feat.alarm_hi);
        }

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
