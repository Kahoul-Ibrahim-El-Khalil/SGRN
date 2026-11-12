#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/common/endian-helper.hpp>
#include <sgrn/gateway/database/PersistenceService.hpp>
#include <sgrn/gateway/tools/sgrn-dataset.hpp>
#include <sgrn/gateway/twin/FieldTraversal.hpp>
#include <sgrn/ml/columnar-dataset.hpp>
#include <sgrn/ml/time-grid-resampler.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/json-dom.hpp>
#include <sgrn/utils/time.hpp>
#include <fstream>
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

// Transcode unit (sgrn-dataset-transcode.cpp): declared here so this TU can
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
    RETURN_IF(t_scl_path.empty() || !std::filesystem::exists(t_scl_path), fmt::format("SCL Schema file not found: {}", t_scl_path));

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
    if (t_s.size() != 29 || t_s[4] != '-' || t_s[7] != '-' || t_s[10] != ' ' || t_s[13] != ':' || t_s[16] != ':' || t_s[19] != '.')
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
    if (year < 1900 || month < 1 || month > 12 || day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
        second > 59 || ms < 0)
        return 0;
    // Howard Hinnant's days_from_civil
    int64_t y = year - (month <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153u * static_cast<unsigned>(month + (month > 2 ? -3 : 9)) + 2) / 5 + static_cast<unsigned>(day) - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;
    return ((days * 24 + hour) * 60 + minute) * 60000 + second * 1000 + ms;
}

static bool isBinaryArchiveContent(const std::string& t_content) {
    return t_content.size() >= 4 && t_content[0] == 'S' && t_content[1] == 'G' && t_content[2] == 'R' && t_content[3] == 'N';
}

sgrn::Result<DatasetSummary> DatasetProcessor::process(const DatasetConfig& t_config) {
    auto schema_res = loadSchema(t_config.scl_schema_path);
    RETURN_IF(schema_res.hasError(), schema_res.error());

    auto files = discoverFiles(t_config.input_dir);
    if (files.empty()) {
        return fmt::format("No telemetry archive files found in directory: {}", t_config.input_dir);
    }

    auto writers = openWriters(t_config);
    RETURN_IF(writers.hasError(), writers.error());

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
    if (!t_config.output_csv_path.empty() && !t_config.no_csv) {
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

    // --- Discrete time-grid resampler & columnar binary writer ---
    if (!t_config.output_bin_path.empty()) {
        // Build ResamplerConfig from the loaded SCL schema store
        std::vector<sgrn::scl::DbSchema> schemas;
        schemas.reserve(schema_store_.dbs().size());
        for (const auto& [db_num, db] : schema_store_.dbs()) {
            schemas.push_back(db);
        }

        auto resampler_cfg = sgrn::ml::makeResamplerConfig(schemas, t_config.step_override_ns, t_config.time_tag_override);

        const uint64_t step_ns = resampler_cfg.step_ns;
        const size_t n_fields = resampler_cfg.fields.size();

        // Separate feature and label field indices for the writer
        std::vector<size_t> feature_indices;
        std::vector<size_t> label_indices;
        for (size_t i = 0; i < n_fields; ++i) {
            if (resampler_cfg.fields[i].is_label)
                label_indices.push_back(i);
            else
                feature_indices.push_back(i);
        }

        // Build a manifest with the column names for .sgrnb
        sgrn::ml::ModelManifest bin_manifest;
        bin_manifest.name = "sgrn_dataset";
        bin_manifest.backend = "columnar";
        for (size_t fi : feature_indices) {
            sgrn::ml::FeatureDef fd;
            fd.name = resampler_cfg.fields[fi].name;
            fd.source_tag = resampler_cfg.fields[fi].name;
            fd.tag_index = static_cast<uint32_t>(fi);
            fd.derived = sgrn::ml::DerivedType::Raw;
            fd.norm = sgrn::ml::NormType::None;
            bin_manifest.spec.features.push_back(std::move(fd));
        }
        for (size_t li : label_indices) {
            sgrn::ml::LabelDef ld;
            ld.name = resampler_cfg.fields[li].name;
            ld.target_tag = resampler_cfg.fields[li].name;
            ld.task = sgrn::ml::TaskType::Regression;
            bin_manifest.spec.labels.push_back(std::move(ld));
        }

        // Open the columnar binary writer
        bin_writer_ = std::make_unique<sgrn::ml::ColumnarDatasetWriter>();
        auto open_res = bin_writer_->open(t_config.output_bin_path, std::move(bin_manifest), step_ns);
        if (open_res.hasError()) {
            return fmt::format("Failed to open .sgrnb output: {}", open_res.error());
        }

        // Set up the resampler with a callback that writes rows to the writer
        resampler_ = std::make_unique<sgrn::ml::TimeGridResampler>(std::move(resampler_cfg));

        // Capture feature/label indices for the row callback
        auto* writer_ptr = bin_writer_.get();
        const auto feat_idx = feature_indices;
        const auto lbl_idx = label_indices;

        resampler_->setRowCallback([writer_ptr, feat_idx, lbl_idx](const sgrn::ml::DiscreteGridRow& t_row) {
            std::vector<float> features(feat_idx.size());
            for (size_t i = 0; i < feat_idx.size(); ++i) {
                features[i] = t_row.values[feat_idx[i]];
            }
            std::vector<float> labels(lbl_idx.size());
            for (size_t i = 0; i < lbl_idx.size(); ++i) {
                labels[i] = t_row.values[lbl_idx[i]];
            }
            (void)writer_ptr->pushRow(t_row.timestamp_ns, features, labels);
        });

        fmt::print(fg(fmt::color::cyan), "[sgrn_dataset] Time-grid resampler: step={}ns, {} fields ({} features, {} labels)\n", step_ns,
            n_fields, feature_indices.size(), label_indices.size());
    }

    return {};
}

void DatasetProcessor::closeWriters(const DatasetConfig& /*t_config*/) {
    if (csv_out_.is_open())
        csv_out_.close();

    // Flush the resampler's final bucket and finalize the binary writer
    if (resampler_) {
        resampler_->flush();
        fmt::print(fg(fmt::color::cyan), "[sgrn_dataset] Resampler emitted {} discrete rows\n", resampler_->totalEmittedRows());
        resampler_.reset();
    }
    if (bin_writer_) {
        auto fin_res = bin_writer_->finalize();
        if (fin_res.hasError()) {
            fmt::print(fg(fmt::color::red), "[sgrn_dataset] Failed to finalize .sgrnb: {}\n", fin_res.error());
        } else {
            fmt::print(fg(fmt::color::green), "[sgrn_dataset] Wrote {} rows to .sgrnb\n", bin_writer_->rowCount());
        }
        bin_writer_.reset();
    }
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
    const int64_t resolved_ts = resolveRowTs(t_ts);

    // --- Feed the discrete time-grid resampler (nanoseconds) ---
    if (resampler_) {
        const uint64_t ts_ns = static_cast<uint64_t>(resolved_ts) * 1'000'000ULL;
        for (const auto& feat : features_) {
            auto st_it = current_state_.find(feat.full_name);
            if (st_it == current_state_.end())
                continue;
            const std::string& raw = st_it->second;
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
                        continue;
                } catch (...) {
                    continue;
                }
            }
            if (std::isnan(dval))
                continue;
            const std::string db_tag = fmt::format("DB{}.{}", feat.db_num, feat.field_path);
            resampler_->pushNamedSample(db_tag, dval, ts_ns);
            resampler_->pushNamedSample(feat.full_name, dval, ts_ns);
        }
    }

    // --- CSV output ---
    if (!csv_out_.is_open())
        return;
    csv_out_ << resolved_ts;
    for (size_t f_idx = 0; f_idx < summary_.features.size(); ++f_idx) {
        auto& feat = summary_.features[f_idx];
        auto st_it = current_state_.find(feat.full_name);
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
                csv_out_ << "," << raw;
            } else {
                csv_out_ << ",";
                feat.null_count++;
                feat.total_samples++;
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

            if (!std::isnan(dval)) {
                if (!feat.min_max_set || dval < feat.min_val)
                    feat.min_val = dval;
                if (!feat.min_max_set || dval > feat.max_val)
                    feat.max_val = dval;
                feat.min_max_set = true;
            }
        } else {
            csv_out_ << ",";
            feat.null_count++;
        }
    }
    csv_out_ << "\n";
}
void DatasetProcessor::storeStateValue(const std::string& t_key, const sgrn::json::dom::Value& t_v) {
    std::string val_str;
    if (t_v.is_string())
        val_str = t_v.getString();
    else if (t_v.is_number())
        val_str = std::to_string(t_v.getDouble());
    else if (t_v.is_boolean())
        val_str = t_v.getBool() ? "true" : "false";
    else if (t_v.is_null())
        val_str = "null";
    else {
        sgrn::json::dom::StringBuffer sb;
        sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> w(sb);
        t_v.accept(w);
        val_str = sb.getString();
    }
    current_state_[t_key] = val_str;
}

void DatasetProcessor::walkStateObject(const std::string& t_prefix, const sgrn::json::dom::Value& t_v) {
    if (!t_v.is_object()) {
        storeStateLeaf(t_prefix, t_v);
        return;
    }
    for (auto m = t_v.memberBegin(); m != t_v.memberEnd(); ++m)
        walkStateObject(t_prefix.empty() ? m->name.getString() : t_prefix + "." + m->name.getString(), m->value);
}

/// Store one leaf; array values fan out to the indexed schema features
/// ("a.b[i].c"), resolved against feature_index_map_ so the keys match
/// the CSV columns exactly. Unknown shapes are skipped, as before.
void DatasetProcessor::storeStateLeaf(const std::string& t_key, const sgrn::json::dom::Value& t_v) {
    if (t_v.is_object()) {
        walkStateObject(t_key, t_v);
        return;
    }
    if (!t_v.is_array()) {
        if (t_v.is_string() || t_v.is_number() || t_v.is_boolean())
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
        for (sgrn::json::dom::SizeType i = 0; i < t_v.size(); ++i) {
            std::string ikey;
            for (size_t k = 0; k <= p; ++k)
                ikey += (k ? "." : "") + segs[k];
            ikey += "[" + std::to_string(i) + "]";
            for (size_t k = p + 1; k < segs.size(); ++k)
                ikey += "." + segs[k];
            if (feature_index_map_.find(ikey) == feature_index_map_.end())
                continue;
            const auto& el = t_v[i];
            if (el.is_string() || el.is_number() || el.is_boolean() || el.is_null())
                storeStateValue(ikey, el);
            else if (el.is_object())
                walkStateObject(ikey, el);
        }
        return;
    }
}

void DatasetProcessor::ingestKeyedValues(const sgrn::json::dom::Value& t_obj, const std::vector<std::string>& t_dict) {
    for (auto it = t_obj.memberBegin(); it != t_obj.memberEnd(); ++it) {
        std::string key = it->name.getString();

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
bool DatasetProcessor::ingestJsonRecord(const sgrn::json::dom::Document& t_doc, const std::vector<std::string>& t_dict) {
    int64_t ts = 0;
    if (t_doc.hasMember("ts") && t_doc["ts"].isInt64()) {
        ts = t_doc["ts"].getInt64();
    } else if (t_doc.hasMember("timestamp") && t_doc["timestamp"].isInt64()) {
        ts = t_doc["timestamp"].getInt64();
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
    if (t_doc.hasMember("changes") && t_doc["changes"].is_object()) {
        ingestKeyedValues(t_doc["changes"], t_dict);
    }
    // 2. Anchor record — current format: {"type":"anchor","ts":123,"data":{"<id>":val,...}}
    //    Legacy format (pre-dictionary): {"data":{"DbName":{"field":val}}}
    else if (t_doc.hasMember("data") && t_doc["data"].is_object()) {
        const auto& data = t_doc["data"];
        bool looks_nested = false;
        for (auto it = data.memberBegin(); it != data.memberEnd(); ++it) {
            if (it->value.is_object()) {
                looks_nested = true;
                break;
            }
        }
        if (looks_nested) {
            for (auto db_it = data.memberBegin(); db_it != data.memberEnd(); ++db_it) {
                if (!db_it->value.is_object())
                    continue;
                walkStateObject(db_it->name.getString(), db_it->value);
            }
        } else {
            ingestKeyedValues(data, t_dict);
        }
    }
    // 3. Legacy flat record: {"timestamp":123,"db":"TankSkid","path":"tank_level","val":"30.5"}
    else if (t_doc.hasMember("db") && t_doc.hasMember("path") && t_doc.hasMember("val")) {
        std::string db = t_doc["db"].getString();
        std::string path = t_doc["path"].getString();
        std::string full_key = fmt::format("{}.{}", db, path);
        std::string val_str;
        if (t_doc["val"].is_string())
            val_str = t_doc["val"].getString();
        else if (t_doc["val"].is_number())
            val_str = std::to_string(t_doc["val"].getDouble());
        else if (t_doc["val"].is_boolean())
            val_str = t_doc["val"].getBool() ? "true" : "false";
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
            db_endianness_.count(t_db) ? db_endianness_.at(t_db) : s7codec::Endian::Little, feat.bit_index);
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
        fmt::print(fg(fmt::color::yellow), "[sgrn_dataset] Unsupported binary version {} in {}, skipping file.\n", ver, t_path.string());
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
            sgrn::json::dom::Document control_doc;
            if (!control_doc.parse(reinterpret_cast<const char*>(payload), payload_len).hasParseError() && control_doc.is_object()) {
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
                fmt::print(fg(fmt::color::yellow), "[sgrn_dataset] Anchor CRC mismatch in {}, seeking resync\n", t_path.string());
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
            if (have_image && sgrn::gateway::database::parseDeltaRuns(payload, payload_len, img_it->second.size(), delta_db, runs)) {
                frame_ok = true;
                for (const auto& run : runs)
                    std::memcpy(img_it->second.data() + run.offset, payload + run.data_pos, run.len);
            } else if (have_image) {
                corrupt = true;
            }
            if (!frame_ok) {
                if (corrupt) {
                    fmt::print(fg(fmt::color::yellow), "[sgrn_dataset] Corrupt delta frame in {}, seeking resync\n", t_path.string());
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

        if (csv_out_.is_open() || resampler_)
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

        sgrn::json::dom::Document doc;
        if (doc.parse(line.c_str()).hasParseError())
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
    sgrn::json::dom::StringBuffer sb;
    sgrn::json::dom::PrettyWriter<sgrn::json::dom::StringBuffer> writer(sb);

    writer.startObject();
    writer.key("generator");
    writer.string("sgrn_dataset");

    writer.key("created_at_ms");
    writer.int64(sgrn::utils::time::nowMilliseconds());

    writer.key("start_timestamp_ms");
    writer.int64(t_summary.start_timestamp_ms);

    writer.key("end_timestamp_ms");
    writer.int64(t_summary.end_timestamp_ms);

    writer.key("total_files_processed");
    writer.uint64(t_summary.total_files_processed);

    writer.key("total_records");
    writer.uint64(t_summary.total_records_processed);

    writer.key("features");
    writer.startArray();

    for (const auto& feat : t_summary.features) {
        writer.startObject();
        writer.key("name");
        writer.string(feat.full_name.c_str());

        writer.key("db");
        writer.string(feat.db_name.c_str());

        writer.key("path");
        writer.string(feat.field_path.c_str());

        writer.key("type");
        writer.string(feat.data_type.c_str());

        writer.key("unit");
        writer.string(feat.unit.c_str());

        writer.key("dimension");
        writer.string(feat.dimension.c_str());

        writer.key("is_label");
        writer.boolean(feat.is_label);

        if (!feat.description.empty()) {
            writer.key("description");
            writer.string(feat.description.c_str());
        }
        if (feat.precision >= 0) {
            writer.key("precision");
            writer.int32(feat.precision);
        }
        if (feat.has_nominal) {
            writer.key("nominal");
            writer.float64(feat.nominal);
        }
        if (feat.has_alarm) {
            writer.key("alarm_lo");
            writer.float64(feat.alarm_lo);
            writer.key("alarm_hi");
            writer.float64(feat.alarm_hi);
        }

        writer.key("is_categorical");
        writer.boolean(feat.is_categorical);

        if (!feat.enum_map.empty()) {
            writer.key("enum_values");
            writer.startObject();
            for (const auto& kv : feat.enum_map) {
                writer.key(std::to_string(kv.first).c_str());
                writer.string(kv.second.c_str());
            }
            writer.endObject();
        }

        writer.key("min_val");
        writer.float64(feat.min_val);

        writer.key("max_val");
        writer.float64(feat.max_val);

        writer.key("null_count");
        writer.uint64(feat.null_count);

        writer.key("total_samples");
        writer.uint64(feat.total_samples);
        writer.endObject();
    }

    writer.endArray();
    writer.endObject();

    std::ofstream out(t_manifest_path);
    if (!out.is_open()) {
        return fmt::format("Failed to write manifest: {}", t_manifest_path);
    }
    out << sb.getString();
    out.close();

    return {};
}

} // namespace sgrn::gateway::tools
