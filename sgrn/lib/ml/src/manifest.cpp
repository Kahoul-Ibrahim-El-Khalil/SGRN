#include <sgrn/ml/manifest.hpp>
#include <sgrn/utils/hashing.hpp>
#include <sgrn/utils/json.hpp>
#include <fstream>

namespace sgrn::ml
{

std::string ModelManifest::toJson(bool t_pretty) const {
    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();

    doc.AddMember("name", rapidjson::Value(name.c_str(), alloc), alloc);
    doc.AddMember("version", rapidjson::Value(version.c_str(), alloc), alloc);
    doc.AddMember("backend", rapidjson::Value(backend.c_str(), alloc), alloc);
    doc.AddMember("weights_file", rapidjson::Value(weights_file.c_str(), alloc), alloc);
    doc.AddMember("weights_sha256", rapidjson::Value(weights_sha256.c_str(), alloc), alloc);

    rapidjson::Value spec_val(rapidjson::kObjectType);
    spec.toJsonValue(spec_val, alloc);
    doc.AddMember("spec", spec_val, alloc);

    // MLP config
    rapidjson::Value mlp_val(rapidjson::kObjectType);
    mlp_val.AddMember("use_batch_norm", mlp.use_batch_norm, alloc);
    rapidjson::Value layers_arr(rapidjson::kArrayType);
    for (const auto& layer : mlp.layers) {
        rapidjson::Value l_val(rapidjson::kObjectType);
        l_val.AddMember("units", static_cast<uint64_t>(layer.units), alloc);
        l_val.AddMember("activation", rapidjson::Value(layer.activation.c_str(), alloc), alloc);
        l_val.AddMember("dropout", layer.dropout, alloc);
        layers_arr.PushBack(l_val, alloc);
    }
    mlp_val.AddMember("layers", layers_arr, alloc);
    doc.AddMember("mlp", mlp_val, alloc);

    // Train info
    rapidjson::Value train_val(rapidjson::kObjectType);
    train_val.AddMember("timestamp_ns", train.timestamp_ns, alloc);
    train_val.AddMember("epochs_trained", train.epochs_trained, alloc);
    train_val.AddMember("final_loss", train.final_loss, alloc);
    train_val.AddMember("dataset_sha256", rapidjson::Value(train.dataset_sha256.c_str(), alloc), alloc);
    rapidjson::Value metrics_obj(rapidjson::kObjectType);
    for (const auto& [k, v] : train.metrics) {
        rapidjson::Value key(k.c_str(), alloc);
        rapidjson::Value val(v);
        metrics_obj.AddMember(key, val, alloc);
    }
    train_val.AddMember("metrics", metrics_obj, alloc);
    doc.AddMember("train", train_val, alloc);

    // Thresholds
    rapidjson::Value thresh_val(rapidjson::kObjectType);
    thresh_val.AddMember("warning", thresholds.warning, alloc);
    thresh_val.AddMember("alarm", thresholds.alarm, alloc);
    thresh_val.AddMember("anomaly", thresholds.anomaly, alloc);
    doc.AddMember("thresholds", thresh_val, alloc);

    return sgrn::utils::json::serialize(doc, t_pretty);
}

Result<ModelManifest> ModelManifest::fromJson(std::string_view t_json_str) {
    auto doc_res = sgrn::utils::json::deserialize(t_json_str);
    if (doc_res.hasError()) {
        return Result<ModelManifest>::Error("Failed to parse ModelManifest JSON: " + doc_res.error());
    }

    const auto& doc = doc_res.value();
    if (!doc.IsObject()) {
        return Result<ModelManifest>::Error("ModelManifest JSON must be an object");
    }

    ModelManifest manifest;
    if (doc.HasMember("name") && doc["name"].IsString())
        manifest.name = doc["name"].GetString();
    if (doc.HasMember("version") && doc["version"].IsString())
        manifest.version = doc["version"].GetString();
    if (doc.HasMember("backend") && doc["backend"].IsString())
        manifest.backend = doc["backend"].GetString();
    if (doc.HasMember("weights_file") && doc["weights_file"].IsString())
        manifest.weights_file = doc["weights_file"].GetString();
    if (doc.HasMember("weights_sha256") && doc["weights_sha256"].IsString())
        manifest.weights_sha256 = doc["weights_sha256"].GetString();

    if (doc.HasMember("spec") && doc["spec"].IsObject()) {
        auto spec_res = FeatureSpec::fromJsonValue(doc["spec"]);
        if (spec_res.hasError()) {
            return Result<ModelManifest>::Error("Failed to parse FeatureSpec inside manifest: " + spec_res.error());
        }
        manifest.spec = std::move(spec_res.value());
    }

    if (doc.HasMember("mlp") && doc["mlp"].IsObject()) {
        const auto& m = doc["mlp"];
        if (m.HasMember("use_batch_norm") && m["use_batch_norm"].IsBool()) {
            manifest.mlp.use_batch_norm = m["use_batch_norm"].GetBool();
        }
        if (m.HasMember("layers") && m["layers"].IsArray()) {
            for (const auto& l : m["layers"].GetArray()) {
                if (!l.IsObject())
                    continue;
                MlpLayerConfig lc;
                if (l.HasMember("units") && l["units"].IsUint64())
                    lc.units = l["units"].GetUint64();
                if (l.HasMember("activation") && l["activation"].IsString())
                    lc.activation = l["activation"].GetString();
                if (l.HasMember("dropout") && l["dropout"].IsNumber())
                    lc.dropout = static_cast<float>(l["dropout"].GetDouble());
                manifest.mlp.layers.push_back(std::move(lc));
            }
        }
    }

    if (doc.HasMember("train") && doc["train"].IsObject()) {
        const auto& t = doc["train"];
        if (t.HasMember("timestamp_ns") && t["timestamp_ns"].IsUint64())
            manifest.train.timestamp_ns = t["timestamp_ns"].GetUint64();
        if (t.HasMember("epochs_trained") && t["epochs_trained"].IsUint())
            manifest.train.epochs_trained = t["epochs_trained"].GetUint();
        if (t.HasMember("final_loss") && t["final_loss"].IsNumber())
            manifest.train.final_loss = static_cast<float>(t["final_loss"].GetDouble());
        if (t.HasMember("dataset_sha256") && t["dataset_sha256"].IsString())
            manifest.train.dataset_sha256 = t["dataset_sha256"].GetString();
        if (t.HasMember("metrics") && t["metrics"].IsObject()) {
            for (auto it = t["metrics"].MemberBegin(); it != t["metrics"].MemberEnd(); ++it) {
                if (it->name.IsString() && it->value.IsNumber()) {
                    manifest.train.metrics[it->name.GetString()] = static_cast<float>(it->value.GetDouble());
                }
            }
        }
    }

    if (doc.HasMember("thresholds") && doc["thresholds"].IsObject()) {
        const auto& th = doc["thresholds"];
        if (th.HasMember("warning") && th["warning"].IsNumber())
            manifest.thresholds.warning = static_cast<float>(th["warning"].GetDouble());
        if (th.HasMember("alarm") && th["alarm"].IsNumber())
            manifest.thresholds.alarm = static_cast<float>(th["alarm"].GetDouble());
        if (th.HasMember("anomaly") && th["anomaly"].IsNumber())
            manifest.thresholds.anomaly = static_cast<float>(th["anomaly"].GetDouble());
    }

    return manifest;
}

Result<ModelManifest> ModelManifest::fromFile(const std::filesystem::path& t_file_path) {
    std::ifstream ifs(t_file_path, std::ios::binary);
    if (!ifs.is_open()) {
        return Result<ModelManifest>::Error("Failed to open file: " + t_file_path.string());
    }
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return fromJson(content);
}

Result<void> ModelManifest::validateWeights(std::string_view t_bytes) const {
    if (weights_sha256.empty()) {
        return {}; // No checksum specified
    }
    auto hash_res = sgrn::utils::computeSha256Data(t_bytes, sgrn::utils::HashEncoding::hex);
    if (hash_res.hasError()) {
        return Result<void>::Error("Failed to compute SHA256 of weights: " + hash_res.error());
    }
    if (hash_res.value() != weights_sha256) {
        return Result<void>::Error("Weights SHA256 mismatch: expected " + weights_sha256 + ", got " + hash_res.value());
    }
    return {};
}

Result<void> ModelManifest::validateWeightsFile(const std::filesystem::path& t_file_path) const {
    if (weights_sha256.empty()) {
        return {};
    }
    auto hash_res = sgrn::utils::computeSha256File(t_file_path, sgrn::utils::HashEncoding::hex);
    if (hash_res.hasError()) {
        return Result<void>::Error("Failed to compute SHA256 of file " + t_file_path.string() + ": " + hash_res.error());
    }
    if (hash_res.value() != weights_sha256) {
        return Result<void>::Error(
            "Weights file SHA256 mismatch for " + t_file_path.string() + ": expected " + weights_sha256 + ", got " + hash_res.value());
    }
    return {};
}

} // namespace sgrn::ml
