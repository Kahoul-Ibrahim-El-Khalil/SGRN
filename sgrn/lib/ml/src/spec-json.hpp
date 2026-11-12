#pragma once
// spec-json.hpp — internal JSON mirror structs for FeatureSpec (Glaze).
//
// The spec JSON layout is a compatibility contract (specs and manifests are
// committed alongside trained models). Each on-disk shape is mirrored by a
// Json-suffixed struct whose member names match the JSON keys, so plain Glaze
// reflection reads and writes it. Mirrors live in sgrn::ml::detail; see
// MbProxyConfigJson for the naming precedent.
//
// Member declaration order matches the historical key order byte-for-byte.
// The enums ("derived", "norm", "task") were historically written as ints, so
// the mirror members are ints with converters; everything else is pure
// reflection. Absent keys keep the domain struct's defaults on read — for that
// to hold, the mirror defaults must equal the domain defaults.
//
// See sgrn/utils/glaze.hpp for the sgrn::json wrapper; direct glz:: calls are
// not needed here.

#include <sgrn/ml/feature-spec.hpp>
#include <cstdint>
#include <string>
#include <vector>

namespace sgrn::ml::detail
{

struct FeatureDefJson {
    std::string name;
    std::string source_tag;
    uint32_t tag_index = 0;
    int derived = 0; ///< DerivedType as int (historical wire form)
    double param = 0.0;
    int norm = 0; ///< NormType as int (historical wire form)
    double min_val = 0.0;
    double max_val = 1.0;
    double mean_val = 0.0;
    double std_val = 1.0;
};

struct LabelDefJson {
    std::string name;
    std::string target_tag;
    int task = 0; ///< TaskType as int (historical wire form)
    double threshold = 0.5;
    std::vector<std::string> classes;
};

struct FeatureSpecJson {
    std::string name;
    std::string version = "1.0.0";
    std::vector<FeatureDefJson> features;
    std::vector<LabelDefJson> labels;
};

[[nodiscard]] inline FeatureDefJson featureDefToJson(const FeatureDef& t_f) {
    FeatureDefJson w;
    w.name = t_f.name;
    w.source_tag = t_f.source_tag;
    w.tag_index = t_f.tag_index;
    w.derived = static_cast<int>(t_f.derived);
    w.param = t_f.param;
    w.norm = static_cast<int>(t_f.norm);
    w.min_val = t_f.min_val;
    w.max_val = t_f.max_val;
    w.mean_val = t_f.mean_val;
    w.std_val = t_f.std_val;
    return w;
}

[[nodiscard]] inline FeatureDef featureDefFromJson(const FeatureDefJson& t_w) {
    FeatureDef f;
    f.name = t_w.name;
    f.source_tag = t_w.source_tag;
    f.tag_index = t_w.tag_index;
    f.derived = static_cast<DerivedType>(t_w.derived);
    f.param = t_w.param;
    f.norm = static_cast<NormType>(t_w.norm);
    f.min_val = t_w.min_val;
    f.max_val = t_w.max_val;
    f.mean_val = t_w.mean_val;
    f.std_val = t_w.std_val;
    return f;
}

[[nodiscard]] inline LabelDefJson labelDefToJson(const LabelDef& t_l) {
    LabelDefJson w;
    w.name = t_l.name;
    w.target_tag = t_l.target_tag;
    w.task = static_cast<int>(t_l.task);
    w.threshold = t_l.threshold;
    w.classes = t_l.classes;
    return w;
}

[[nodiscard]] inline LabelDef labelDefFromJson(const LabelDefJson& t_w) {
    LabelDef l;
    l.name = t_w.name;
    l.target_tag = t_w.target_tag;
    l.task = static_cast<TaskType>(t_w.task);
    l.threshold = t_w.threshold;
    l.classes = t_w.classes;
    return l;
}

[[nodiscard]] inline FeatureSpecJson specToJson(const FeatureSpec& t_spec) {
    FeatureSpecJson w;
    w.name = t_spec.name;
    w.version = t_spec.version;
    w.features.reserve(t_spec.features.size());
    for (const auto& f : t_spec.features) {
        w.features.push_back(featureDefToJson(f));
    }
    w.labels.reserve(t_spec.labels.size());
    for (const auto& l : t_spec.labels) {
        w.labels.push_back(labelDefToJson(l));
    }
    return w;
}

[[nodiscard]] inline FeatureSpec specFromJson(const FeatureSpecJson& t_w) {
    FeatureSpec spec;
    spec.name = t_w.name;
    spec.version = t_w.version;
    spec.features.reserve(t_w.features.size());
    for (const auto& fw : t_w.features) {
        spec.features.push_back(featureDefFromJson(fw));
    }
    spec.labels.reserve(t_w.labels.size());
    for (const auto& lw : t_w.labels) {
        spec.labels.push_back(labelDefFromJson(lw));
    }
    return spec;
}

} // namespace sgrn::ml::detail
