#include <sgrn/ml/twin_bridge.hpp>

#include <type_traits>

namespace sgrn::ml
{

TwinBridge::TwinBridge(TwinBridgeConfig t_config)
    : config_(std::move(t_config)) {
}

Result<void> TwinBridge::loadModel(ModelManifest t_manifest, std::unique_ptr<IModelBackend> t_backend) {
    // Ensure 1:1 input ordering if tag_index was left default (0)
    for (size_t i = 0; i < t_manifest.spec.features.size(); ++i) {
        if (t_manifest.spec.features[i].tag_index == 0 && i > 0) {
            t_manifest.spec.features[i].tag_index = static_cast<uint32_t>(i);
        }
    }

    auto res = engine_.loadModel(std::move(t_manifest), std::move(t_backend));
    if (res.hasError()) {
        return res;
    }

    if (p_state_) {
        return bindState(*p_state_);
    }

    return {};
}

Result<void> TwinBridge::bindState(PlcState& t_state) {
    p_state_ = &t_state;

    if (!engine_.hasModel()) {
        return Result<void>::Error("Cannot bind TwinBridge to state without a loaded model manifest");
    }

    const auto* p_manifest = engine_.manifest();
    if (!p_manifest) {
        return Result<void>::Error("Null manifest in PredictionEngine");
    }

    bound_inputs_.clear();
    bound_outputs_.clear();

    // 1. Resolve and bind input features
    const auto& features = p_manifest->spec.features;
    bound_inputs_.reserve(features.size());

    for (size_t i = 0; i < features.size(); ++i) {
        const auto& feat = features[i];
        const PlcNode* p_node = t_state.find(feat.source_tag);
        if (!p_node) {
            return Result<void>::Error("Feature source tag not found in PlcState: " + feat.source_tag);
        }

        DbEntry* p_entry = t_state.findSegmentById(p_node->db_number_);
        if (!p_entry) {
            return Result<void>::Error("DB segment not registered in PlcState for DB: " + std::to_string(p_node->db_number_));
        }

        BoundFeatureNode bf;
        bf.p_node = p_node;
        bf.p_entry = p_entry;
        bf.feature_index = i;
        bf.db_number = p_node->db_number_;
        bf.byte_offset = p_node->offset_;
        bf.bit_index = p_node->bit_index_;
        bf.type = p_node->type_;
        bf.endian = p_node->endian_;
        bf.valid = true;

        bound_inputs_.push_back(bf);
    }

    raw_feature_scratch_.resize(bound_inputs_.size(), 0.0);

    // 2. Resolve and bind output targets
    for (const auto& binding : config_.output_bindings) {
        const PlcNode* p_node = t_state.find(binding.tag_path);
        if (!p_node) {
            return Result<void>::Error("Output tag path not found in PlcState: " + binding.tag_path);
        }

        DbEntry* p_entry = t_state.findSegmentById(p_node->db_number_);
        if (!p_entry) {
            return Result<void>::Error("DB segment not registered in PlcState for output DB: " + std::to_string(p_node->db_number_));
        }

        BoundOutputNode bo;
        bo.p_node = p_node;
        bo.p_entry = p_entry;
        bo.target = binding.target;
        bo.output_index = binding.output_index;
        bo.db_number = p_node->db_number_;
        bo.byte_offset = p_node->offset_;
        bo.bit_index = p_node->bit_index_;
        bo.type = p_node->type_;
        bo.endian = p_node->endian_;
        bo.valid = true;

        bound_outputs_.push_back(bo);
    }

    return {};
}

void TwinBridge::unbindState() noexcept {
    p_state_ = nullptr;
    bound_inputs_.clear();
    bound_outputs_.clear();
    raw_feature_scratch_.clear();
}

double TwinBridge::extractScalarValue(const uint8_t* tp_arena, const BoundFeatureNode& t_node) const {
    if (!tp_arena || !t_node.p_entry || t_node.byte_offset >= t_node.p_entry->size) {
        return 0.0;
    }

    const uint8_t* p_field = tp_arena + t_node.p_entry->offset + t_node.byte_offset;
    const size_t remaining = t_node.p_entry->size - t_node.byte_offset;

    // Fast-path for DTL: decode directly to nanoseconds without string allocation
    if (t_node.type == s7codec::Type::DTL) {
        return static_cast<double>(
            s7codec::decodeDtlToNanoseconds(s7codec::ReadBuffer{p_field, remaining}, s7codec::DecodeOptions{.endian = t_node.endian}));
    }

    auto dv = s7codec::decodeScalar(t_node.type, s7codec::ReadBuffer{p_field, remaining},
        s7codec::DecodeOptions{.endian = t_node.endian, .bitOffset = s7codec::BitOffset{t_node.bit_index}});

    if (!dv.valid()) {
        return 0.0;
    }

    return s7codec::visit(dv, [](const auto& val) -> double {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, bool>) {
            return val ? 1.0 : 0.0;
        } else if constexpr (std::is_arithmetic_v<T>) {
            return static_cast<double>(val);
        } else if constexpr (std::is_same_v<T, std::string>) {
            try {
                return std::stod(val);
            } catch (...) {
                return 0.0;
            }
        } else {
            return 0.0;
        }
    });
}

Result<Prediction> TwinBridge::step(uint64_t t_ts_ns) {
    if (!isBound() || !p_state_) {
        return Result<Prediction>::Error("TwinBridge is not bound to a PlcState");
    }

    const uint8_t* p_arena = p_state_->arenaData();
    if (!p_arena && !bound_inputs_.empty()) {
        return Result<Prediction>::Error("PlcState arena data is null");
    }

    for (size_t i = 0; i < bound_inputs_.size(); ++i) {
        const auto& bf = bound_inputs_[i];
        if (!bf.valid || !bf.p_entry) {
            raw_feature_scratch_[i] = 0.0;
            continue;
        }

        std::shared_lock<std::shared_mutex> lock(bf.p_entry->mutex_);
        raw_feature_scratch_[i] = extractScalarValue(p_arena, bf);
    }

    auto pred_res = engine_.step(t_ts_ns, raw_feature_scratch_);
    if (pred_res.hasError()) {
        return pred_res;
    }

    if (config_.write_outputs_to_twin) {
        auto write_res = writeOutputsToTwin(pred_res.value());
        if (write_res.hasError()) {
            return Result<Prediction>::Error("Failed to write prediction outputs to twin: " + write_res.error());
        }
    }

    return pred_res;
}

Result<Prediction> TwinBridge::stepSnapshot(uint64_t t_ts_ns, uint16_t t_db_number, std::span<const uint8_t> t_raw_db) {
    if (!engine_.hasModel()) {
        return Result<Prediction>::Error("Model is not loaded in TwinBridge");
    }

    if (raw_feature_scratch_.size() != bound_inputs_.size()) {
        raw_feature_scratch_.resize(bound_inputs_.size(), 0.0);
    }

    for (size_t i = 0; i < bound_inputs_.size(); ++i) {
        const auto& bf = bound_inputs_[i];
        if (bf.db_number != t_db_number || bf.byte_offset >= t_raw_db.size()) {
            continue;
        }

        const uint8_t* p_field = t_raw_db.data() + bf.byte_offset;
        const size_t remaining = t_raw_db.size() - bf.byte_offset;

        if (bf.type == s7codec::Type::DTL) {
            raw_feature_scratch_[i] = static_cast<double>(
                s7codec::decodeDtlToNanoseconds(s7codec::ReadBuffer{p_field, remaining}, s7codec::DecodeOptions{.endian = bf.endian}));
            continue;
        }

        auto dv = s7codec::decodeScalar(bf.type, s7codec::ReadBuffer{p_field, remaining},
            s7codec::DecodeOptions{.endian = bf.endian, .bitOffset = s7codec::BitOffset{bf.bit_index}});

        if (dv.valid()) {
            raw_feature_scratch_[i] = s7codec::visit(dv, [](const auto& val) -> double {
                using T = std::decay_t<decltype(val)>;
                if constexpr (std::is_same_v<T, bool>) {
                    return val ? 1.0 : 0.0;
                } else if constexpr (std::is_arithmetic_v<T>) {
                    return static_cast<double>(val);
                } else if constexpr (std::is_same_v<T, std::string>) {
                    try {
                        return std::stod(val);
                    } catch (...) {
                        return 0.0;
                    }
                } else {
                    return 0.0;
                }
            });
        }
    }

    return engine_.step(t_ts_ns, raw_feature_scratch_);
}

Result<void> TwinBridge::writeOutputsToTwin(const Prediction& t_pred) {
    if (!isBound() || !p_state_) {
        return Result<void>::Error("TwinBridge is not bound to a PlcState");
    }

    uint8_t* p_arena = p_state_->arenaData();
    if (!p_arena) {
        return Result<void>::Error("PlcState arena data is null");
    }

    for (const auto& bo : bound_outputs_) {
        if (!bo.valid || !bo.p_entry) {
            continue;
        }

        double val = 0.0;
        int64_t int_val = 0;
        switch (bo.target) {
            case TwinOutputTarget::PredictionValue:
                if (bo.output_index < t_pred.outputs.size()) {
                    val = static_cast<double>(t_pred.outputs[bo.output_index]);
                    int_val = static_cast<int64_t>(t_pred.outputs[bo.output_index]);
                }
                break;
            case TwinOutputTarget::AnomalyScore:
                val = static_cast<double>(t_pred.verdict.anomaly_score);
                int_val = static_cast<int64_t>(val);
                break;
            case TwinOutputTarget::VerdictStatus:
                val = static_cast<double>(static_cast<uint8_t>(t_pred.verdict.status));
                int_val = static_cast<int64_t>(static_cast<uint8_t>(t_pred.verdict.status));
                break;
            case TwinOutputTarget::AlarmBit:
                val = (t_pred.verdict.status == VerdictStatus::Alarm || t_pred.verdict.status == VerdictStatus::Anomaly) ? 1.0 : 0.0;
                int_val = val != 0.0 ? 1 : 0;
                break;
            case TwinOutputTarget::Timestamp:
                int_val = static_cast<int64_t>(t_pred.timestamp_ns);
                val = static_cast<double>(t_pred.timestamp_ns);
                break;
        }

        s7codec::DecodedValue dv;
        if (bo.target == TwinOutputTarget::AlarmBit || bo.type == s7codec::Type::Bool) {
            dv = s7codec::DecodedValue::makeBool(int_val != 0);
        } else if (bo.type == s7codec::Type::Real) {
            dv = s7codec::DecodedValue::makeFloat(static_cast<float>(val));
        } else if (bo.type == s7codec::Type::LReal) {
            dv = s7codec::DecodedValue::makeDouble(val);
        } else if (bo.type == s7codec::Type::DTL || bo.type == s7codec::Type::LTime || bo.type == s7codec::Type::LDT ||
                   bo.type == s7codec::Type::LDTL) {
            dv = s7codec::DecodedValue::makeSigned(int_val);
        } else if (bo.type == s7codec::Type::SInt || bo.type == s7codec::Type::Int || bo.type == s7codec::Type::DInt ||
                   bo.type == s7codec::Type::LInt) {
            dv = s7codec::DecodedValue::makeSigned(int_val);
        } else {
            dv = s7codec::DecodedValue::makeUnsigned(static_cast<uint64_t>(int_val));
        }

        std::unique_lock<std::shared_mutex> lock(bo.p_entry->mutex_);
        if (bo.byte_offset >= bo.p_entry->size) {
            continue;
        }

        uint8_t* p_field = p_arena + bo.p_entry->offset + bo.byte_offset;
        const size_t remaining = bo.p_entry->size - bo.byte_offset;

        auto encode_st = s7codec::encodeScalar(dv, bo.type, s7codec::WriteBuffer{p_field, remaining},
            s7codec::EncodeOptions{.endian = bo.endian, .bitOffset = s7codec::BitOffset{bo.bit_index}});

        if (encode_st.has_value()) {
            if (config_.bump_versions && bo.p_node) {
                const_cast<PlcNode*>(bo.p_node)->bumpVersionChain();
            }
            if (config_.mark_dirty && bo.p_entry) {
                bo.p_entry->markDirty();
            }
        }
    }

    return {};
}

void TwinBridge::reset() {
    engine_.reset();
    std::fill(raw_feature_scratch_.begin(), raw_feature_scratch_.end(), 0.0);
}

} // namespace sgrn::ml
