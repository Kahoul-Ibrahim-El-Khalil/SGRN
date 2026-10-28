#include <sgrn/ml/time_grid_resampler.hpp>
#include <algorithm>
#include <cmath>

namespace sgrn::ml
{

TimeGridResampler::TimeGridResampler(ResamplerConfig t_config) {
    setConfig(std::move(t_config));
}

void TimeGridResampler::setConfig(ResamplerConfig t_config) {
    config_ = std::move(t_config);
    if (config_.step_ns == 0) {
        config_.step_ns = 10'000'000ULL; // 10ms fallback
    }

    field_index_map_.clear();
    for (size_t i = 0; i < config_.fields.size(); ++i) {
        field_index_map_[config_.fields[i].name] = i;
        config_.fields[i].is_boolean = (config_.fields[i].type == s7codec::Type::Bool);
    }

    reset();
}

void TimeGridResampler::reset() {
    accumulators_.assign(config_.fields.size(), ResamplerCell{});
    last_valid_values_.assign(config_.fields.size(), 0.0f);
    has_last_valid_.assign(config_.fields.size(), false);

    start_ts_ns_ = 0;
    current_bucket_idx_ = 0;
    total_emitted_rows_ = 0;
    has_started_ = false;
}

void TimeGridResampler::resetAccumulators() {
    for (auto& acc : accumulators_) {
        acc.reset();
    }
}

bool TimeGridResampler::isBool(size_t t_field_idx) const noexcept {
    if (t_field_idx < config_.fields.size()) {
        return config_.fields[t_field_idx].is_boolean;
    }
    return false;
}

void TimeGridResampler::pushSample(size_t t_field_idx, double t_value, uint64_t t_sample_clock_ns) {
    if (t_field_idx >= config_.fields.size()) {
        return;
    }
    if (t_sample_clock_ns == 0) {
        return;
    }

    if (!has_started_) {
        // Anchor the uniform grid to a clean multiple of step_ns
        start_ts_ns_ = (t_sample_clock_ns / config_.step_ns) * config_.step_ns;
        current_bucket_idx_ = 0;
        has_started_ = true;
    }

    if (t_sample_clock_ns < start_ts_ns_) {
        accumulators_[t_field_idx].addSample(t_value, isBool(t_field_idx));
        return;
    }

    const uint64_t bucket_idx = (t_sample_clock_ns - start_ts_ns_) / config_.step_ns;

    if (bucket_idx == current_bucket_idx_) {
        accumulators_[t_field_idx].addSample(t_value, isBool(t_field_idx));
    } else if (bucket_idx > current_bucket_idx_) {
        // Advance bucket: close current bucket and emit
        closeAndEmitBucket(current_bucket_idx_);

        // Emit forward-filled rows for any gap buckets between current and target
        if (config_.forward_fill) {
            for (uint64_t gap = current_bucket_idx_ + 1; gap < bucket_idx; ++gap) {
                emitForwardFilledBucket(gap);
            }
        }

        current_bucket_idx_ = bucket_idx;
        resetAccumulators();
        accumulators_[t_field_idx].addSample(t_value, isBool(t_field_idx));
    } else {
        // Slight jitter within 1 prior bucket: accumulate into current bucket
        accumulators_[t_field_idx].addSample(t_value, isBool(t_field_idx));
    }
}

void TimeGridResampler::pushNamedSample(std::string_view t_tag, double t_value, uint64_t t_sample_clock_ns) {
    auto it = field_index_map_.find(std::string(t_tag));
    if (it != field_index_map_.end()) {
        pushSample(it->second, t_value, t_sample_clock_ns);
    }
}

void TimeGridResampler::closeAndEmitBucket(uint64_t t_bucket_idx) {
    DiscreteGridRow row;
    row.timestamp_ns = start_ts_ns_ + t_bucket_idx * config_.step_ns;
    row.values.resize(config_.fields.size(), 0.0f);
    row.valid.resize(config_.fields.size(), false);

    for (size_t i = 0; i < config_.fields.size(); ++i) {
        const auto& f = config_.fields[i];
        auto& acc = accumulators_[i];
        if (acc.count > 0) {
            row.values[i] = acc.finish(f.agg_type, f.is_boolean);
            row.valid[i] = true;
            last_valid_values_[i] = row.values[i];
            has_last_valid_[i] = true;
        } else if (config_.forward_fill && has_last_valid_[i]) {
            row.values[i] = last_valid_values_[i];
            row.valid[i] = true;
        } else {
            row.values[i] = 0.0f;
            row.valid[i] = false;
        }
    }

    if (row_callback_) {
        row_callback_(row);
    }
    total_emitted_rows_++;
}

void TimeGridResampler::emitForwardFilledBucket(uint64_t t_gap_idx) {
    DiscreteGridRow row;
    row.timestamp_ns = start_ts_ns_ + t_gap_idx * config_.step_ns;
    row.values.resize(config_.fields.size(), 0.0f);
    row.valid.resize(config_.fields.size(), false);

    for (size_t i = 0; i < config_.fields.size(); ++i) {
        if (has_last_valid_[i]) {
            row.values[i] = last_valid_values_[i];
            row.valid[i] = true;
        } else {
            row.values[i] = 0.0f;
            row.valid[i] = false;
        }
    }

    if (row_callback_) {
        row_callback_(row);
    }
    total_emitted_rows_++;
}

void TimeGridResampler::flush() {
    if (has_started_) {
        closeAndEmitBucket(current_bucket_idx_);
        has_started_ = false;
    }
}

ResamplerConfig makeResamplerConfig(
    const std::vector<sgrn::scl::DbSchema>& t_schemas, uint64_t t_step_override_ns, std::string_view t_clock_tag_override) {
    ResamplerConfig cfg;
    cfg.step_ns = t_step_override_ns;

    std::string primary_clock;
    if (!t_clock_tag_override.empty()) {
        primary_clock = std::string(t_clock_tag_override);
    }

    // Pass 1: Discover default clock and step if not provided
    for (const auto& db : t_schemas) {
        if (cfg.step_ns == 0 && db.dataset_step_ns > 0) {
            cfg.step_ns = db.dataset_step_ns;
        }

        std::function<void(const std::vector<sgrn::scl::DbField>&, const std::string&)> scan_clocks =
            [&](const std::vector<sgrn::scl::DbField>& t_fields, const std::string& t_prefix) {
                for (const auto& f : t_fields) {
                    std::string full_path = t_prefix.empty() ? f.name : t_prefix + "." + f.name;
                    if (f.is_timestamp || f.type == s7codec::Type::DTL || f.type == s7codec::Type::LDT || f.type == s7codec::Type::LDTL) {
                        if (primary_clock.empty()) {
                            primary_clock = fmt::format("DB{}.{}", db.db_number, full_path);
                        }
                    }
                    if (f.dataset_step_ns > 0 && cfg.step_ns == 0) {
                        cfg.step_ns = f.dataset_step_ns;
                    }
                    if (!f.children.empty()) {
                        scan_clocks(f.children, full_path);
                    }
                }
            };
        scan_clocks(db.fields, "");
    }

    if (cfg.step_ns == 0) {
        cfg.step_ns = 10'000'000ULL; // 10ms default
    }
    cfg.primary_clock_tag = primary_clock;

    // Pass 2: Extract columns
    for (const auto& db : t_schemas) {
        std::string db_clock = primary_clock;

        // Check if DB has its own timestamp clock
        for (const auto& f : db.fields) {
            if (f.is_timestamp) {
                db_clock = fmt::format("DB{}.{}", db.db_number, f.name);
                break;
            }
        }

        std::function<void(const std::vector<sgrn::scl::DbField>&, const std::string&, size_t)> scan_fields =
            [&](const std::vector<sgrn::scl::DbField>& t_fields, const std::string& t_prefix, size_t t_base_offset) {
                for (const auto& f : t_fields) {
                    if (f.is_transient) {
                        continue;
                    }
                    std::string full_path = t_prefix.empty() ? f.name : t_prefix + "." + f.name;

                    if (f.type == s7codec::Type::Struct) {
                        scan_fields(f.children, full_path, t_base_offset + static_cast<size_t>(f.offset));
                        continue;
                    }

                    // Skip the clock timestamp fields themselves from feature inputs
                    if (f.is_timestamp || f.type == s7codec::Type::DTL || f.type == s7codec::Type::LDT || f.type == s7codec::Type::LDTL) {
                        continue;
                    }

                    ResamplerFieldConfig fc;
                    fc.name = fmt::format("DB{}.{}", db.db_number, full_path);
                    fc.db_num = db.db_number;
                    fc.offset = t_base_offset + static_cast<size_t>(f.offset);
                    fc.bit_index = f.bit_index;
                    fc.type = f.type;
                    fc.is_boolean = (f.type == s7codec::Type::Bool);
                    fc.is_label = f.is_label;

                    // Apply default aggregation based on type if unspecified
                    if (f.agg_type != sgrn::scl::AggregationType::Default) {
                        fc.agg_type = f.agg_type;
                    } else if (fc.is_boolean) {
                        fc.agg_type = sgrn::scl::AggregationType::Or;
                    } else if (f.type == s7codec::Type::Real || f.type == s7codec::Type::LReal) {
                        fc.agg_type = sgrn::scl::AggregationType::Mean;
                    } else {
                        fc.agg_type = sgrn::scl::AggregationType::Last;
                    }

                    fc.clock_tag = db_clock;
                    cfg.fields.push_back(std::move(fc));
                }
            };

        scan_fields(db.fields, "", 0);
    }

    return cfg;
}

ResamplerConfig makeResamplerConfig(
    const sgrn::scl::DbSchema& t_schema, uint64_t t_step_override_ns, std::string_view t_clock_tag_override) {
    std::vector<sgrn::scl::DbSchema> schemas;
    schemas.push_back(t_schema);
    return makeResamplerConfig(schemas, t_step_override_ns, t_clock_tag_override);
}

} // namespace sgrn::ml
