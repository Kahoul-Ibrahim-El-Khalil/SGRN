#include <sgrn/ml/feature_spec.hpp>
#include <sgrn/utils/json.hpp>
#include <cmath>
#include <limits>

namespace sgrn::ml
{

float FeatureDef::normalize(float t_val) const noexcept {
    switch (norm) {
        case NormType::MinMax: {
            const double denom = max_val - min_val;
            if (std::abs(denom) > 1e-9) {
                return static_cast<float>((t_val - min_val) / denom);
            }
            return t_val;
        }
        case NormType::ZScore: {
            if (std::abs(std_val) > 1e-9) {
                return static_cast<float>((t_val - mean_val) / std_val);
            }
            return t_val;
        }
        case NormType::None:
        default:
            return t_val;
    }
}

int FeatureSpec::findFeatureIndex(std::string_view t_feat_name) const noexcept {
    for (size_t i = 0; i < features.size(); ++i) {
        if (features[i].name == t_feat_name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int FeatureSpec::findLabelIndex(std::string_view t_label_name) const noexcept {
    for (size_t i = 0; i < labels.size(); ++i) {
        if (labels[i].name == t_label_name) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void FeatureSpec::toJsonValue(rapidjson::Value& t_out, rapidjson::Document::AllocatorType& t_alloc) const {
    t_out.SetObject();
    t_out.AddMember("name", rapidjson::Value(name.c_str(), t_alloc), t_alloc);
    t_out.AddMember("version", rapidjson::Value(version.c_str(), t_alloc), t_alloc);

    rapidjson::Value feats(rapidjson::kArrayType);
    for (const auto& f : features) {
        rapidjson::Value f_val(rapidjson::kObjectType);
        f_val.AddMember("name", rapidjson::Value(f.name.c_str(), t_alloc), t_alloc);
        f_val.AddMember("source_tag", rapidjson::Value(f.source_tag.c_str(), t_alloc), t_alloc);
        f_val.AddMember("tag_index", f.tag_index, t_alloc);
        f_val.AddMember("derived", static_cast<int>(f.derived), t_alloc);
        f_val.AddMember("param", f.param, t_alloc);
        f_val.AddMember("norm", static_cast<int>(f.norm), t_alloc);
        f_val.AddMember("min_val", f.min_val, t_alloc);
        f_val.AddMember("max_val", f.max_val, t_alloc);
        f_val.AddMember("mean_val", f.mean_val, t_alloc);
        f_val.AddMember("std_val", f.std_val, t_alloc);
        feats.PushBack(f_val, t_alloc);
    }
    t_out.AddMember("features", feats, t_alloc);

    rapidjson::Value labs(rapidjson::kArrayType);
    for (const auto& l : labels) {
        rapidjson::Value l_val(rapidjson::kObjectType);
        l_val.AddMember("name", rapidjson::Value(l.name.c_str(), t_alloc), t_alloc);
        l_val.AddMember("target_tag", rapidjson::Value(l.target_tag.c_str(), t_alloc), t_alloc);
        l_val.AddMember("task", static_cast<int>(l.task), t_alloc);
        l_val.AddMember("threshold", l.threshold, t_alloc);

        rapidjson::Value classes_arr(rapidjson::kArrayType);
        for (const auto& c : l.classes) {
            classes_arr.PushBack(rapidjson::Value(c.c_str(), t_alloc), t_alloc);
        }
        l_val.AddMember("classes", classes_arr, t_alloc);
        labs.PushBack(l_val, t_alloc);
    }
    t_out.AddMember("labels", labs, t_alloc);
}

std::string FeatureSpec::toJson(bool t_pretty) const {
    rapidjson::Document doc;
    doc.SetObject();
    toJsonValue(doc, doc.GetAllocator());
    return sgrn::utils::json::serialize(doc, t_pretty);
}

Result<FeatureSpec> FeatureSpec::fromJsonValue(const rapidjson::Value& t_val) {
    if (!t_val.IsObject()) {
        return Result<FeatureSpec>::Error("FeatureSpec JSON root must be an object");
    }

    FeatureSpec spec;
    if (t_val.HasMember("name") && t_val["name"].IsString()) {
        spec.name = t_val["name"].GetString();
    }
    if (t_val.HasMember("version") && t_val["version"].IsString()) {
        spec.version = t_val["version"].GetString();
    }

    if (t_val.HasMember("features") && t_val["features"].IsArray()) {
        const auto& f_arr = t_val["features"].GetArray();
        spec.features.reserve(f_arr.Size());
        for (const auto& f_val : f_arr) {
            if (!f_val.IsObject())
                continue;
            FeatureDef f;
            if (f_val.HasMember("name") && f_val["name"].IsString())
                f.name = f_val["name"].GetString();
            if (f_val.HasMember("source_tag") && f_val["source_tag"].IsString())
                f.source_tag = f_val["source_tag"].GetString();
            if (f_val.HasMember("tag_index") && f_val["tag_index"].IsUint())
                f.tag_index = f_val["tag_index"].GetUint();
            if (f_val.HasMember("derived") && f_val["derived"].IsInt())
                f.derived = static_cast<DerivedType>(f_val["derived"].GetInt());
            if (f_val.HasMember("param") && f_val["param"].IsNumber())
                f.param = f_val["param"].GetDouble();
            if (f_val.HasMember("norm") && f_val["norm"].IsInt())
                f.norm = static_cast<NormType>(f_val["norm"].GetInt());
            if (f_val.HasMember("min_val") && f_val["min_val"].IsNumber())
                f.min_val = f_val["min_val"].GetDouble();
            if (f_val.HasMember("max_val") && f_val["max_val"].IsNumber())
                f.max_val = f_val["max_val"].GetDouble();
            if (f_val.HasMember("mean_val") && f_val["mean_val"].IsNumber())
                f.mean_val = f_val["mean_val"].GetDouble();
            if (f_val.HasMember("std_val") && f_val["std_val"].IsNumber())
                f.std_val = f_val["std_val"].GetDouble();
            spec.features.push_back(std::move(f));
        }
    }

    if (t_val.HasMember("labels") && t_val["labels"].IsArray()) {
        const auto& l_arr = t_val["labels"].GetArray();
        spec.labels.reserve(l_arr.Size());
        for (const auto& l_val : l_arr) {
            if (!l_val.IsObject())
                continue;
            LabelDef l;
            if (l_val.HasMember("name") && l_val["name"].IsString())
                l.name = l_val["name"].GetString();
            if (l_val.HasMember("target_tag") && l_val["target_tag"].IsString())
                l.target_tag = l_val["target_tag"].GetString();
            if (l_val.HasMember("task") && l_val["task"].IsInt())
                l.task = static_cast<TaskType>(l_val["task"].GetInt());
            if (l_val.HasMember("threshold") && l_val["threshold"].IsNumber())
                l.threshold = l_val["threshold"].GetDouble();
            if (l_val.HasMember("classes") && l_val["classes"].IsArray()) {
                for (const auto& c_val : l_val["classes"].GetArray()) {
                    if (c_val.IsString())
                        l.classes.emplace_back(c_val.GetString());
                }
            }
            spec.labels.push_back(std::move(l));
        }
    }

    return spec;
}

Result<FeatureSpec> FeatureSpec::fromJson(std::string_view t_json_str) {
    auto doc_res = sgrn::utils::json::deserialize(t_json_str);
    if (doc_res.hasError()) {
        return Result<FeatureSpec>::Error("Invalid JSON for FeatureSpec: " + doc_res.error());
    }
    return fromJsonValue(doc_res.value());
}

} // namespace sgrn::ml
