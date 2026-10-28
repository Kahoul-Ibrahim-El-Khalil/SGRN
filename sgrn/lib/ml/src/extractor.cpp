#include <sgrn/ml/extractor.hpp>
#include <numeric>

namespace sgrn::ml
{

FeatureExtractor::FeatureExtractor(FeatureSpec t_spec) {
    (void)setSpec(std::move(t_spec));
}

Result<void> FeatureExtractor::setSpec(FeatureSpec t_spec) {
    spec_ = std::move(t_spec);
    states_.clear();
    states_.resize(spec_.features.size());
    return {};
}

void FeatureExtractor::reset() {
    for (auto& s : states_) {
        s.last_ts_ns = 0;
        s.last_value = 0.0;
        s.has_last = false;
        s.window.clear();
    }
}

Result<void> FeatureExtractor::push(uint64_t t_ts_ns, std::span<const double> t_raw_inputs, std::span<float> t_out_features) {
    if (t_out_features.size() != spec_.features.size()) {
        return Result<void>::Error(
            fmt::format("FeatureExtractor output span size {} != expected feature count {}", t_out_features.size(), spec_.features.size()));
    }

    for (size_t i = 0; i < spec_.features.size(); ++i) {
        const auto& f = spec_.features[i];
        if (f.tag_index >= t_raw_inputs.size()) {
            return Result<void>::Error(
                fmt::format("Feature '{}' requests tag_index {}, but raw_inputs size is {}", f.name, f.tag_index, t_raw_inputs.size()));
        }

        const double raw_val = t_raw_inputs[f.tag_index];
        auto& st = states_[i];

        double computed_val = raw_val;

        switch (f.derived) {
            case DerivedType::Raw: {
                computed_val = raw_val;
                break;
            }
            case DerivedType::RateOfChange: {
                if (st.has_last && t_ts_ns > st.last_ts_ns) {
                    const double dt = static_cast<double>(t_ts_ns - st.last_ts_ns) * 1e-9;
                    if (dt > 1e-9) {
                        computed_val = (raw_val - st.last_value) / dt;
                    } else {
                        computed_val = 0.0;
                    }
                } else {
                    computed_val = 0.0;
                }
                break;
            }
            case DerivedType::Delta: {
                if (st.has_last) {
                    computed_val = raw_val - st.last_value;
                } else {
                    computed_val = 0.0;
                }
                break;
            }
            case DerivedType::MovingAverage: {
                const size_t max_win = f.param > 1.0 ? static_cast<size_t>(f.param) : 1;
                st.window.push_back(raw_val);
                while (st.window.size() > max_win) {
                    st.window.pop_front();
                }
                const double sum = std::accumulate(st.window.begin(), st.window.end(), 0.0);
                computed_val = sum / static_cast<double>(st.window.size());
                break;
            }
            case DerivedType::Lag: {
                const size_t lag_steps = f.param >= 1.0 ? static_cast<size_t>(f.param) : 1;
                st.window.push_back(raw_val);
                while (st.window.size() > lag_steps + 1) {
                    st.window.pop_front();
                }
                if (st.window.size() > lag_steps) {
                    computed_val = st.window.front();
                } else {
                    computed_val = raw_val;
                }
                break;
            }
        }

        st.last_value = raw_val;
        st.last_ts_ns = t_ts_ns;
        st.has_last = true;

        t_out_features[i] = f.normalize(static_cast<float>(computed_val));
    }

    return {};
}

Result<std::vector<float>> FeatureExtractor::extract(uint64_t t_ts_ns, std::span<const double> t_raw_inputs) {
    std::vector<float> features(spec_.features.size());
    auto res = push(t_ts_ns, t_raw_inputs, features);
    if (res.hasError()) {
        return Result<std::vector<float>>::Error(res.error());
    }
    return features;
}

} // namespace sgrn::ml
