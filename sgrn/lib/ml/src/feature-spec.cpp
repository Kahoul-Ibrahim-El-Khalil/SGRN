#include "spec-json.hpp"
#include <sgrn/ml/feature-spec.hpp>
#include <sgrn/utils/glaze.hpp>
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

std::string FeatureSpec::toJson(bool t_pretty) const {
    const auto res = t_pretty ? sgrn::json::writePretty(detail::specToJson(*this)) : sgrn::json::write(detail::specToJson(*this));
    // Plain mirror structs cannot fail to write; degrade to an empty payload.
    return res.value_or(std::string{});
}

Result<FeatureSpec> FeatureSpec::fromJson(std::string_view t_json_str) {
    auto spec_res = sgrn::json::read<detail::FeatureSpecJson>(t_json_str);
    RETURN_ERROR_IF(spec_res.hasError(), "Invalid JSON for FeatureSpec: " + spec_res.error().what());
    return detail::specFromJson(spec_res.value());
}

} // namespace sgrn::ml
