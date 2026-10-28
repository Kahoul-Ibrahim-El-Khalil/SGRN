#include <sgrn/ml/engine.hpp>
#include <chrono>

namespace sgrn::ml
{

Result<void> PredictionEngine::loadModel(ModelManifest t_manifest, std::unique_ptr<IModelBackend> t_backend) {
    if (!t_backend) {
        return Result<void>::Error("Cannot load null backend into PredictionEngine");
    }

    manifest_ = std::move(t_manifest);
    backend_ = std::move(t_backend);

    auto spec_res = extractor_.setSpec(manifest_.spec);
    if (spec_res.hasError()) {
        backend_.reset();
        return spec_res;
    }

    feature_buffer_.resize(manifest_.spec.features.size(), 0.0f);
    return {};
}

void PredictionEngine::reset() {
    extractor_.reset();
    std::fill(feature_buffer_.begin(), feature_buffer_.end(), 0.0f);
}

Result<Prediction> PredictionEngine::step(uint64_t t_ts_ns, std::span<const double> t_raw_inputs) {
    if (!backend_) {
        return Result<Prediction>::Error("PredictionEngine has no active model backend");
    }

    const auto t_start = std::chrono::steady_clock::now();

    // 1. Extract features
    auto ext_res = extractor_.push(t_ts_ns, t_raw_inputs, feature_buffer_);
    if (ext_res.hasError()) {
        return Result<Prediction>::Error("Feature extraction error: " + ext_res.error());
    }

    // 2. Perform inference via backend
    auto pred_res = backend_->predict(feature_buffer_);
    if (pred_res.hasError()) {
        return Result<Prediction>::Error("Inference error: " + pred_res.error());
    }

    const auto t_end = std::chrono::steady_clock::now();
    const auto latency_us = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::microseconds>(t_end - t_start).count());

    Prediction pred;
    pred.timestamp_ns = t_ts_ns;
    pred.outputs = std::move(pred_res.value());
    pred.features = feature_buffer_;
    pred.latency_us = latency_us;

    // 3. Evaluate verdict against thresholds
    pred.verdict.status = VerdictStatus::Normal;
    pred.verdict.confidence = 1.0f;

    // Use primary output as anomaly / score if available
    if (!pred.outputs.empty()) {
        const float primary_score = pred.outputs[0];
        pred.verdict.anomaly_score = primary_score;

        if (primary_score >= manifest_.thresholds.anomaly) {
            pred.verdict.status = VerdictStatus::Anomaly;
            pred.verdict.message =
                fmt::format("Anomaly detected: score {:.4f} >= threshold {:.4f}", primary_score, manifest_.thresholds.anomaly);
        } else if (primary_score >= manifest_.thresholds.alarm) {
            pred.verdict.status = VerdictStatus::Alarm;
            pred.verdict.message = fmt::format("Alarm: score {:.4f} >= threshold {:.4f}", primary_score, manifest_.thresholds.alarm);
        } else if (primary_score >= manifest_.thresholds.warning) {
            pred.verdict.status = VerdictStatus::Warning;
            pred.verdict.message = fmt::format("Warning: score {:.4f} >= threshold {:.4f}", primary_score, manifest_.thresholds.warning);
        } else {
            pred.verdict.message = "Normal";
        }
    }

    return pred;
}

} // namespace sgrn::ml
