#include <sgrn/gateway/tools/sgrn_dataset.hpp>

#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/time.hpp>
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
namespace sgrn::gateway::tools
{

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
    if (t_scl_path.empty() || !std::filesystem::exists(t_scl_path)) {
        return fmt::format("SCL Schema file not found: {}", t_scl_path);
    }

    auto res = sgrn::scl::PlcSchemaStore::loadFromFile(t_scl_path);
    if (res.hasError()) {
        return fmt::format("Failed to parse SCL schema: {}", toString(res.error()));
    }
    schema_store_ = std::move(res.value());

    features_.clear();
    feature_index_map_.clear();

    for (const auto& db_pair : schema_store_.dbs()) {
        const auto& db = db_pair.second;
        for (const auto& field : db.fields) {
            FeatureMeta meta;
            meta.db_name = db.db_name;
            meta.field_path = field.name;
            meta.full_name = fmt::format("{}.{}", db.db_name, field.name);
            meta.data_type = s7codec::s7TypeToString(field.type);
            meta.unit = field.unit.value_or("");
            meta.is_categorical = isCategoricalType(field.type);

            if (!field.enum_map.empty() || sgrn::scl::kind_of(field) == sgrn::scl::FieldKind::Enum) {
                meta.is_categorical = true;
                meta.data_type = "ENUM";
                meta.enum_map = field.enum_map;
            }

            feature_index_map_[meta.full_name] = features_.size();
            features_.push_back(std::move(meta));
        }
    }

    return {};
}

std::vector<std::filesystem::path> DatasetProcessor::discoverFiles(const std::string& t_dir) {
    std::vector<std::filesystem::path> files;
    if (!std::filesystem::exists(t_dir))
        return files;

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

        std::istringstream stream(decompressed);
        std::string line;

        while (std::getline(stream, line)) {
            if (line.empty())
                continue;

            rapidjson::Document doc;
            if (doc.Parse(line.c_str()).HasParseError())
                continue;

            int64_t ts = 0;
            if (doc.HasMember("ts") && doc["ts"].IsInt64()) {
                ts = doc["ts"].GetInt64();
            } else if (doc.HasMember("timestamp") && doc["timestamp"].IsInt64()) {
                ts = doc["timestamp"].GetInt64();
            }

            if (ts == 0)
                continue;

            if (summary.start_timestamp_ms == 0 || ts < summary.start_timestamp_ms) {
                summary.start_timestamp_ms = ts;
            }
            if (ts > summary.end_timestamp_ms) {
                summary.end_timestamp_ms = ts;
            }

            // 1. Delta record: {"type":"delta","ts":123,"changes":{"TankSkid.tank_level":30.5}}
            if (doc.HasMember("changes") && doc["changes"].IsObject()) {
                const auto& changes = doc["changes"];
                for (auto it = changes.MemberBegin(); it != changes.MemberEnd(); ++it) {
                    std::string key = it->name.GetString();
                    std::string val_str;
                    if (it->value.IsString())
                        val_str = it->value.GetString();
                    else if (it->value.IsNumber())
                        val_str = std::to_string(it->value.GetDouble());
                    else if (it->value.IsBool())
                        val_str = it->value.GetBool() ? "1" : "0";
                    current_state_[key] = val_str;
                }
            }
            // 2. Anchor record: {"type":"anchor","ts":123,"data":{"TankSkid":{"tank_level":30.5}}}
            else if (doc.HasMember("data") && doc["data"].IsObject()) {
                const auto& data = doc["data"];
                for (auto db_it = data.MemberBegin(); db_it != data.MemberEnd(); ++db_it) {
                    std::string db_name = db_it->name.GetString();
                    if (db_it->value.IsObject()) {
                        for (auto f_it = db_it->value.MemberBegin(); f_it != db_it->value.MemberEnd(); ++f_it) {
                            std::string full_key = fmt::format("{}.{}", db_name, f_it->name.GetString());
                            std::string val_str;
                            if (f_it->value.IsString())
                                val_str = f_it->value.GetString();
                            else if (f_it->value.IsNumber())
                                val_str = std::to_string(f_it->value.GetDouble());
                            else if (f_it->value.IsBool())
                                val_str = f_it->value.GetBool() ? "1" : "0";
                            current_state_[full_key] = val_str;
                        }
                    }
                }
            }
            // 3. Legacy flat record: {"timestamp":123,"db":"TankSkid","path":"tank_level","val":"30.5"}
            else if (doc.HasMember("db") && doc.HasMember("path") && doc.HasMember("val")) {
                std::string db = doc["db"].GetString();
                std::string path = doc["path"].GetString();
                std::string full_key = fmt::format("{}.{}", db, path);
                std::string val_str;
                if (doc["val"].IsString())
                    val_str = doc["val"].GetString();
                else if (doc["val"].IsNumber())
                    val_str = std::to_string(doc["val"].GetDouble());
                else if (doc["val"].IsBool())
                    val_str = doc["val"].GetBool() ? "1" : "0";
                current_state_[full_key] = val_str;
            }

            // Write row to CSV and update feature stats
            if (csv_out.is_open()) {
                csv_out << ts;
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
            }

            summary.total_timestamps++;
            summary.total_records_processed++;
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

} // namespace sgrn::gateway::tools
