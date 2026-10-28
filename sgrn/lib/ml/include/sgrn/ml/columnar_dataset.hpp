#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/ml/manifest.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sgrn::ml
{

/**
 * @brief Writer for creating binary columnar datasets (.sgrnb).
 *
 * Stores discrete time-grid data in columnar fashion (contiguous feature and
 * target columns) alongside exact 64-bit nanosecond timestamp grids.
 */
class ColumnarDatasetWriter {
public:
    ColumnarDatasetWriter() = default;
    ~ColumnarDatasetWriter();

    ColumnarDatasetWriter(const ColumnarDatasetWriter&) = delete;
    ColumnarDatasetWriter& operator=(const ColumnarDatasetWriter&) = delete;
    ColumnarDatasetWriter(ColumnarDatasetWriter&&) noexcept = default;
    ColumnarDatasetWriter& operator=(ColumnarDatasetWriter&&) noexcept = default;

    Result<void> open(const std::string& t_path, ModelManifest t_manifest, uint64_t t_step_ns);

    /// Appends one discrete time step row across all feature and label columns.
    Result<void> pushRow(uint64_t t_timestamp_ns, std::span<const float> t_features, std::span<const float> t_labels = {});

    /// Finalizes headers, computes column normalization statistics, and writes the file.
    Result<void> finalize();

    [[nodiscard]] size_t rowCount() const noexcept {
        return timestamps_.size();
    }

    [[nodiscard]] bool isOpen() const noexcept {
        return is_open_;
    }

private:
    std::string path_;
    ModelManifest manifest_;
    uint64_t step_ns_{10'000'000ULL};
    size_t num_features_{0};
    size_t num_labels_{0};

    std::vector<uint64_t> timestamps_;
    std::vector<std::vector<float>> feature_columns_;
    std::vector<std::vector<float>> label_columns_;

    bool is_open_{false};
};

/**
 * @brief Memory-mapped, zero-copy reader for columnar binary datasets (.sgrnb).
 *
 * Provides direct contiguous spans of floats suitable for torch::from_blob()
 * with zero heap allocations and sub-millisecond file loading times.
 */
class ColumnarDatasetReader {
public:
    ColumnarDatasetReader() = default;
    ~ColumnarDatasetReader();

    ColumnarDatasetReader(const ColumnarDatasetReader&) = delete;
    ColumnarDatasetReader& operator=(const ColumnarDatasetReader&) = delete;
    ColumnarDatasetReader(ColumnarDatasetReader&&) noexcept;
    ColumnarDatasetReader& operator=(ColumnarDatasetReader&&) noexcept;

    static Result<std::unique_ptr<ColumnarDatasetReader>> open(const std::string& t_path);

    [[nodiscard]] size_t rowCount() const noexcept {
        return row_count_;
    }

    [[nodiscard]] size_t featureCount() const noexcept {
        return num_features_;
    }

    [[nodiscard]] size_t labelCount() const noexcept {
        return num_labels_;
    }

    [[nodiscard]] uint64_t stepNs() const noexcept {
        return step_ns_;
    }

    [[nodiscard]] const ModelManifest& manifest() const noexcept {
        return manifest_;
    }

    /// Access raw 64-bit nanosecond timestamps column
    [[nodiscard]] std::span<const uint64_t> timestamps() const noexcept;

    /// Access contiguous float column for feature by index
    [[nodiscard]] std::span<const float> featureColumn(size_t t_idx) const noexcept;

    /// Access contiguous float column for label by index
    [[nodiscard]] std::span<const float> labelColumn(size_t t_idx) const noexcept;

    /// Returns a direct pointer to the contiguous feature matrix (if stored row/column contiguous)
    [[nodiscard]] const uint8_t* rawData() const noexcept {
        return p_data_;
    }

    [[nodiscard]] size_t dataSize() const noexcept {
        return data_size_;
    }

private:
    std::string path_;
    ModelManifest manifest_;
    size_t row_count_{0};
    size_t num_features_{0};
    size_t num_labels_{0};
    uint64_t step_ns_{0};

    int fd_{-1};
    uint8_t* p_data_{nullptr};
    size_t data_size_{0};

    const uint64_t* p_timestamps_{nullptr};
    std::vector<const float*> p_feature_cols_;
    std::vector<const float*> p_label_cols_;
};

} // namespace sgrn::ml
