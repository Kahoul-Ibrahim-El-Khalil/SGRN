#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <ankerl/unordered_dense.h>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sgrn::gateway::tools
{

struct FeatureMeta {
    std::string db_name;
    std::string field_path;
    std::string full_name; // e.g. "DB1.ReactorCore.ThermalPower"
    std::string data_type; // e.g. "REAL", "INT", "BOOL", "ENUM"
    std::string unit;      // e.g. "MW", "°C", "%", "bar"
    bool is_categorical = false;
    std::map<int, std::string> enum_map;
    double min_val = 0.0;
    double max_val = 0.0;
    uint64_t null_count = 0;
    uint64_t total_samples = 0;

    // Added for binary decoding
    uint16_t db_num = 0;
    size_t offset = 0;
    int bit_index = 0; ///< for Bool: bit within the byte (S7 packs consecutive bools)
    sgrn::scl::DataType raw_type = sgrn::scl::DataType::Real;
};

struct DatasetConfig {
    std::string input_dir;        // /tmp/sgrn-gateway-state/
    std::string scl_schema_path;  // ./sgrn/gateway/simulations/nuclear/schema.scl
    std::string output_csv_path;  // dataset.csv or dataset.csv.zst
    std::string manifest_path;    // manifest.json
    std::string convert_out_path; // Output path for archive conversion (.bin.zst or .jsonl.zst)
    std::string target_format;    // "binary" or "jsonl"
    std::string start_time;       // Optional ISO timestamp filter
    std::string end_time;         // Optional ISO timestamp filter
    int zstd_compression_level = 5;
};

struct DatasetSummary {
    uint64_t total_files_processed = 0;
    uint64_t total_records_processed = 0;
    uint64_t total_timestamps = 0;
    int64_t start_timestamp_ms = 0;
    int64_t end_timestamp_ms = 0;
    std::vector<FeatureMeta> features;
};

class DatasetProcessor {
public:
    DatasetProcessor() = default;

    /**
     * @brief Executes dataset extraction, delta reconciliation, CSV export, and ML manifest creation.
     */
    sgrn::Result<DatasetSummary> process(const DatasetConfig& t_config);

    /**
     * @brief Converts an archive file between binary (.bin.zst) and JSONL (.jsonl.zst) formats.
     */
    sgrn::Result<void> convertFormat(
        const std::filesystem::path& t_input_file, const std::filesystem::path& t_output_file, const std::string& t_target_format);

    /**
     * @brief Concatenates archives into a single archive at t_output_file.
     *
     * Each entry of t_inputs may be a file or a directory (directories
     * expand in sorted filename order; explicit entries keep CLI order).
     * Same-format inputs merge verbatim (frame/line order preserved); binary
     * inputs merge into a jsonl target via the convertFormat transcoding.
     * jsonl->binary is refused (transcoding unimplemented). Per-file headers
     * collapse to the first file's (schema drift warns; a changed dictionary
     * is kept inline so IDs keep resolving); footers are dropped and one
     * recomputed footer is appended for jsonl output.
     */
    sgrn::Result<void> mergeArchives(const std::vector<std::filesystem::path>& t_inputs, const std::filesystem::path& t_output_file,
        const std::string& t_target_format = "", int t_zstd_level = 5);

private:
    sgrn::Result<void> loadSchema(const std::string& t_scl_path);
    std::vector<std::filesystem::path> discoverFiles(const std::string& t_dir);
    sgrn::Result<void> processFile(const std::filesystem::path& t_file_path);
    sgrn::Result<void> generateManifest(const std::string& t_manifest_path, const DatasetSummary& t_summary);

    sgrn::scl::PlcSchemaStore schema_store_;
    std::vector<FeatureMeta> features_;
    ankerl::unordered_dense::map<std::string, size_t> feature_index_map_;
    ankerl::unordered_dense::map<std::string, std::string> current_state_;
    int64_t first_ts_ms_ = 0;
    int64_t last_ts_ms_ = 0;
    uint64_t record_count_ = 0;
};

} // namespace sgrn::gateway::tools
