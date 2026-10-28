#include <sgrn/ml/columnar_dataset.hpp>
#include <sgrn/utils/hashing.hpp>
#include <sgrn/utils/json.hpp>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <windows.h>
#endif

namespace sgrn::ml
{

namespace
{
constexpr char kMagic[8] = {'S', 'G', 'R', 'N', 'B', '0', '1', '\0'};

struct FileHeader {
    char magic[8];
    uint32_t header_len{0};
    uint32_t reserved{0};
    uint64_t row_count{0};
    uint64_t step_ns{0};
    uint32_t num_features{0};
    uint32_t num_labels{0};
    uint64_t data_offset{0};
};
} // namespace

// -----------------------------------------------------------------------------
// ColumnarDatasetWriter
// -----------------------------------------------------------------------------

ColumnarDatasetWriter::~ColumnarDatasetWriter() {
    if (is_open_) {
        (void)finalize();
    }
}

Result<void> ColumnarDatasetWriter::open(const std::string& t_path, ModelManifest t_manifest, uint64_t t_step_ns) {
    path_ = t_path;
    manifest_ = std::move(t_manifest);
    step_ns_ = t_step_ns;
    num_features_ = manifest_.spec.features.size();
    num_labels_ = manifest_.spec.labels.size();

    timestamps_.clear();
    feature_columns_.assign(num_features_, std::vector<float>{});
    label_columns_.assign(num_labels_, std::vector<float>{});

    is_open_ = true;
    return {};
}

Result<void> ColumnarDatasetWriter::pushRow(uint64_t t_timestamp_ns, std::span<const float> t_features, std::span<const float> t_labels) {
    if (!is_open_) {
        return Result<void>::Error("ColumnarDatasetWriter is not open");
    }
    if (t_features.size() != num_features_) {
        return Result<void>::Error(fmt::format("Feature span size {} does not match expected {}", t_features.size(), num_features_));
    }
    if (!t_labels.empty() && t_labels.size() != num_labels_) {
        return Result<void>::Error(fmt::format("Label span size {} does not match expected {}", t_labels.size(), num_labels_));
    }

    timestamps_.push_back(t_timestamp_ns);

    for (size_t i = 0; i < num_features_; ++i) {
        feature_columns_[i].push_back(t_features[i]);
    }

    for (size_t i = 0; i < num_labels_; ++i) {
        const float val = i < t_labels.size() ? t_labels[i] : 0.0f;
        label_columns_[i].push_back(val);
    }

    return {};
}

Result<void> ColumnarDatasetWriter::finalize() {
    if (!is_open_) {
        return {};
    }

    const size_t rows = timestamps_.size();

    // 1. Calculate column statistics and update manifest
    for (size_t i = 0; i < num_features_; ++i) {
        const auto& col = feature_columns_[i];
        if (col.empty())
            continue;

        double sum = 0.0;
        double min_v = col[0];
        double max_v = col[0];
        for (float v : col) {
            sum += v;
            if (v < min_v)
                min_v = v;
            if (v > max_v)
                max_v = v;
        }

        const double mean = sum / static_cast<double>(col.size());
        double sq_sum = 0.0;
        for (float v : col) {
            const double diff = v - mean;
            sq_sum += diff * diff;
        }
        const double std_dev = std::sqrt(sq_sum / static_cast<double>(col.size()));

        manifest_.spec.features[i].min_val = min_v;
        manifest_.spec.features[i].max_val = (max_v > min_v) ? max_v : (min_v + 1.0);
        manifest_.spec.features[i].mean_val = mean;
        manifest_.spec.features[i].std_val = (std_dev > 1e-9) ? std_dev : 1.0;
    }

    // 2. Serialize manifest JSON
    std::string manifest_json = manifest_.toJson(false);

    // 3. Prepare FileHeader with 64-byte aligned data_offset
    FileHeader hdr;
    std::memcpy(hdr.magic, kMagic, 8);
    hdr.header_len = static_cast<uint32_t>(manifest_json.size());
    hdr.reserved = 0;
    hdr.row_count = static_cast<uint64_t>(rows);
    hdr.step_ns = step_ns_;
    hdr.num_features = static_cast<uint32_t>(num_features_);
    hdr.num_labels = static_cast<uint32_t>(num_labels_);

    const size_t total_hdr_len = sizeof(FileHeader) + hdr.header_len;
    const size_t aligned_data_offset = (total_hdr_len + 63) & ~size_t(63);
    hdr.data_offset = static_cast<uint64_t>(aligned_data_offset);

    // 4. Write to disk
    std::filesystem::path out_path(path_);
    if (out_path.has_parent_path()) {
        std::filesystem::create_directories(out_path.parent_path());
    }

    std::ofstream ofs(path_, std::ios::binary);
    if (!ofs.is_open()) {
        return Result<void>::Error("Failed to create dataset file: " + path_);
    }

    ofs.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    ofs.write(manifest_json.data(), manifest_json.size());

    // Write alignment padding
    const size_t pad_bytes = aligned_data_offset - total_hdr_len;
    if (pad_bytes > 0) {
        std::vector<char> pad(pad_bytes, '\0');
        ofs.write(pad.data(), pad.size());
    }

    // Write Column 0: Timestamps
    if (!timestamps_.empty()) {
        ofs.write(reinterpret_cast<const char*>(timestamps_.data()), sizeof(uint64_t) * rows);
    }

    // Write Columns 1..N: Features
    for (size_t i = 0; i < num_features_; ++i) {
        if (!feature_columns_[i].empty()) {
            ofs.write(reinterpret_cast<const char*>(feature_columns_[i].data()), sizeof(float) * rows);
        }
    }

    // Write Columns N+1..M: Labels
    for (size_t i = 0; i < num_labels_; ++i) {
        if (!label_columns_[i].empty()) {
            ofs.write(reinterpret_cast<const char*>(label_columns_[i].data()), sizeof(float) * rows);
        }
    }

    ofs.close();
    is_open_ = false;

    // Also write companion manifest.json if path ends with .sgrnb
    if (path_.size() > 6 && path_.compare(path_.size() - 6, 6, ".sgrnb") == 0) {
        std::string json_manifest_path = path_.substr(0, path_.size() - 6) + "_manifest.json";
        std::ofstream m_file(json_manifest_path);
        if (m_file.is_open()) {
            m_file << manifest_.toJson(true);
            m_file.close();
        }
    }

    return {};
}

// -----------------------------------------------------------------------------
// ColumnarDatasetReader
// -----------------------------------------------------------------------------

ColumnarDatasetReader::ColumnarDatasetReader(ColumnarDatasetReader&& t_other) noexcept
    : path_(std::move(t_other.path_))
    , manifest_(std::move(t_other.manifest_))
    , row_count_(t_other.row_count_)
    , num_features_(t_other.num_features_)
    , num_labels_(t_other.num_labels_)
    , step_ns_(t_other.step_ns_)
    , fd_(t_other.fd_)
    , p_data_(t_other.p_data_)
    , data_size_(t_other.data_size_)
    , p_timestamps_(t_other.p_timestamps_)
    , p_feature_cols_(std::move(t_other.p_feature_cols_))
    , p_label_cols_(std::move(t_other.p_label_cols_)) {
    t_other.fd_ = -1;
    t_other.p_data_ = nullptr;
    t_other.data_size_ = 0;
    t_other.p_timestamps_ = nullptr;
}

ColumnarDatasetReader& ColumnarDatasetReader::operator=(ColumnarDatasetReader&& t_other) noexcept {
    if (this != &t_other) {
        if (p_data_) {
#ifndef _WIN32
            munmap(p_data_, data_size_);
#else
            UnmapViewOfFile(p_data_);
#endif
        }
        if (fd_ >= 0) {
#ifndef _WIN32
            close(fd_);
#else
            CloseHandle(reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd_)));
#endif
        }

        path_ = std::move(t_other.path_);
        manifest_ = std::move(t_other.manifest_);
        row_count_ = t_other.row_count_;
        num_features_ = t_other.num_features_;
        num_labels_ = t_other.num_labels_;
        step_ns_ = t_other.step_ns_;
        fd_ = t_other.fd_;
        p_data_ = t_other.p_data_;
        data_size_ = t_other.data_size_;
        p_timestamps_ = t_other.p_timestamps_;
        p_feature_cols_ = std::move(t_other.p_feature_cols_);
        p_label_cols_ = std::move(t_other.p_label_cols_);

        t_other.fd_ = -1;
        t_other.p_data_ = nullptr;
        t_other.data_size_ = 0;
        t_other.p_timestamps_ = nullptr;
    }
    return *this;
}

ColumnarDatasetReader::~ColumnarDatasetReader() {
    if (p_data_) {
#ifndef _WIN32
        munmap(p_data_, data_size_);
#else
        UnmapViewOfFile(p_data_);
#endif
        p_data_ = nullptr;
    }
    if (fd_ >= 0) {
#ifndef _WIN32
        close(fd_);
#else
        CloseHandle(reinterpret_cast<HANDLE>(static_cast<intptr_t>(fd_)));
#endif
        fd_ = -1;
    }
}

Result<std::unique_ptr<ColumnarDatasetReader>> ColumnarDatasetReader::open(const std::string& t_path) {
    if (!std::filesystem::exists(t_path)) {
        return Result<std::unique_ptr<ColumnarDatasetReader>>::Error("Dataset file not found: " + t_path);
    }

    auto reader = std::make_unique<ColumnarDatasetReader>();
    reader->path_ = t_path;

#ifndef _WIN32
    int fd = ::open(t_path.c_str(), O_RDONLY);
    if (fd < 0) {
        return Result<std::unique_ptr<ColumnarDatasetReader>>::Error("Failed to open file: " + t_path);
    }

    struct stat st{};
    if (fstat(fd, &st) < 0) {
        close(fd);
        return Result<std::unique_ptr<ColumnarDatasetReader>>::Error("Failed to stat file: " + t_path);
    }

    size_t file_size = static_cast<size_t>(st.st_size);
    if (file_size < sizeof(FileHeader)) {
        close(fd);
        return Result<std::unique_ptr<ColumnarDatasetReader>>::Error("Dataset file too small: " + t_path);
    }

    void* mapped = mmap(nullptr, file_size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        close(fd);
        return Result<std::unique_ptr<ColumnarDatasetReader>>::Error("Failed to mmap file: " + t_path);
    }

    reader->fd_ = fd;
    reader->p_data_ = static_cast<uint8_t*>(mapped);
    reader->data_size_ = file_size;
#else
    HANDLE hFile = CreateFileA(t_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return Result<std::unique_ptr<ColumnarDatasetReader>>::Error("Failed to open file: " + t_path);
    }
    LARGE_INTEGER liSize;
    if (!GetFileSizeEx(hFile, &liSize)) {
        CloseHandle(hFile);
        return Result<std::unique_ptr<ColumnarDatasetReader>>::Error("Failed to get file size: " + t_path);
    }
    size_t file_size = static_cast<size_t>(liSize.QuadPart);
    HANDLE hMap = CreateFileMappingA(hFile, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!hMap) {
        CloseHandle(hFile);
        return Result<std::unique_ptr<ColumnarDatasetReader>>::Error("Failed to create file mapping: " + t_path);
    }
    void* mapped = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(hMap);
    if (!mapped) {
        CloseHandle(hFile);
        return Result<std::unique_ptr<ColumnarDatasetReader>>::Error("Failed to MapViewOfFile: " + t_path);
    }
    reader->fd_ = static_cast<int>(reinterpret_cast<intptr_t>(hFile));
    reader->p_data_ = static_cast<uint8_t*>(mapped);
    reader->data_size_ = file_size;
#endif

    // Validate Header
    const auto* pHdr = reinterpret_cast<const FileHeader*>(reader->p_data_);
    if (std::memcmp(pHdr->magic, kMagic, 8) != 0) {
        return Result<std::unique_ptr<ColumnarDatasetReader>>::Error("Invalid .sgrnb magic signature in: " + t_path);
    }

    reader->row_count_ = static_cast<size_t>(pHdr->row_count);
    reader->step_ns_ = pHdr->step_ns;
    reader->num_features_ = pHdr->num_features;
    reader->num_labels_ = pHdr->num_labels;

    // Parse embedded manifest JSON
    if (pHdr->header_len > 0) {
        std::string_view json_view(reinterpret_cast<const char*>(reader->p_data_ + sizeof(FileHeader)), pHdr->header_len);
        auto m_res = ModelManifest::fromJson(json_view);
        if (m_res.hasError()) {
            return Result<std::unique_ptr<ColumnarDatasetReader>>::Error("Corrupt embedded manifest JSON: " + m_res.error());
        }
        reader->manifest_ = std::move(m_res.value());
    }

    // Set column pointers
    const uint8_t* p_curr = reader->p_data_ + pHdr->data_offset;

    // Timestamps
    reader->p_timestamps_ = reinterpret_cast<const uint64_t*>(p_curr);
    p_curr += reader->row_count_ * sizeof(uint64_t);

    // Feature columns
    reader->p_feature_cols_.resize(reader->num_features_);
    for (size_t i = 0; i < reader->num_features_; ++i) {
        reader->p_feature_cols_[i] = reinterpret_cast<const float*>(p_curr);
        p_curr += reader->row_count_ * sizeof(float);
    }

    // Label columns
    reader->p_label_cols_.resize(reader->num_labels_);
    for (size_t i = 0; i < reader->num_labels_; ++i) {
        reader->p_label_cols_[i] = reinterpret_cast<const float*>(p_curr);
        p_curr += reader->row_count_ * sizeof(float);
    }

    return reader;
}

std::span<const uint64_t> ColumnarDatasetReader::timestamps() const noexcept {
    if (!p_timestamps_ || row_count_ == 0)
        return {};
    return std::span<const uint64_t>(p_timestamps_, row_count_);
}

std::span<const float> ColumnarDatasetReader::featureColumn(size_t t_idx) const noexcept {
    if (t_idx >= p_feature_cols_.size() || !p_feature_cols_[t_idx] || row_count_ == 0)
        return {};
    return std::span<const float>(p_feature_cols_[t_idx], row_count_);
}

std::span<const float> ColumnarDatasetReader::labelColumn(size_t t_idx) const noexcept {
    if (t_idx >= p_label_cols_.size() || !p_label_cols_[t_idx] || row_count_ == 0)
        return {};
    return std::span<const float>(p_label_cols_[t_idx], row_count_);
}

} // namespace sgrn::ml
