#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <ankerl/unordered_dense.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <rapidjson/document.h>
#include <s7codec/codec.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace sgrn::gateway::tools
{

struct FeatureMeta {
    std::string db_name;
    std::string field_path;
    std::string full_name; // e.g. "DB1.ReactorCore.ThermalPower"
    std::string data_type; // e.g. "REAL", "INT", "BOOL", "ENUM"
    std::string unit;      // e.g. "MW", "°C", "%", "bar" (HOW it is measured)
    std::string dimension; // e.g. "power", "temperature", "flow" (WHAT is measured)
    std::string description;
    int precision = -1; // display decimals, -1 = unset
    double nominal = 0.0;
    bool has_nominal = false;
    double alarm_lo = 0.0;
    double alarm_hi = 0.0;
    bool has_alarm = false;
    bool is_label = false; // #LABEL: collected for reference, never a feature
    bool is_categorical = false;
    std::map<int, std::string> enum_map;
    double min_val = 0.0;
    double max_val = 0.0;
    uint64_t null_count = 0;
    uint64_t total_samples = 0;
    bool min_max_set = false; ///< min/max track the first *valid* (non-NaN) sample

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
    std::string output_cache_prefix; ///< Streaming float32 cache: <prefix>.f32 (rows × features),
                                     ///< <prefix>.ts.i64 (row timestamps, INT64_MIN = unset) and
                                     ///< <prefix>.json (feature name/order metadata). Empty = off.
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
    sgrn::Result<void> generateManifest(const std::string& t_manifest_path, const DatasetSummary& t_summary);

    // --- file ingest (one pass per file, dispatched by format) ---
    void processBinaryArchive(const std::filesystem::path& t_path, const std::string& t_content);
    void processJsonlArchive(const std::filesystem::path& t_path, const std::string& t_content);

    // --- carried-forward-state ingestion (shared by both branches) ---
    /// Ingests one parsed JSONL-shape document (anchor / delta / legacy flat
    /// / control anchor). Returns false for timestamp-less lines (schema /
    /// dictionary / manifest / footer), which carry no data.
    bool ingestJsonRecord(const rapidjson::Document& t_doc, const std::vector<std::string>& t_dict);
    /// Merge one binary DB image's decodable leaves into the current state
    /// (same semantics as the JSONL ingest; undecodable leaves stay absent).
    void ingestBinaryImage(uint16_t t_db, const uint8_t* t_image, size_t t_len);
    void ingestKeyedValues(const rapidjson::Value& t_obj, const std::vector<std::string>& t_dict);
    void storeStateValue(const std::string& t_key, const rapidjson::Value& t_v);
    void storeStateLeaf(const std::string& t_key, const rapidjson::Value& t_v);
    void walkStateObject(const std::string& t_prefix, const rapidjson::Value& t_v);

    // --- row emission (CSV + streaming cache stay row-identical) ---
    /// Batch every co-timestamp frame into one complete row: defer until
    /// the row timestamp changes, then emit.
    void deferRow(int64_t t_ts);
    void flushPendingRow();
    /// Row timestamp: datablock DTL clock (see features ending in
    /// ".timestamp"), WAL frame stamp only as fallback.
    int64_t resolveRowTs(int64_t t_frame_ts) const;
    /// Emit one full row from the carried-forward state.
    void emitRow(int64_t t_ts);

    // --- output writers (CSV + streaming float32 cache) ---
    sgrn::Result<void> openWriters(const DatasetConfig& t_config);
    void closeWriters(const DatasetConfig& t_config);

    sgrn::scl::PlcSchemaStore schema_store_;
    std::vector<FeatureMeta> features_;
    ankerl::unordered_dense::map<std::string, size_t> feature_index_map_;
    ankerl::unordered_dense::map<std::string, std::string> current_state_;
    /// Per-DB byte order used by the binary-image decode (from the compiled
    /// schema; little-endian for typical S7-1500 emulation captures).
    ankerl::unordered_dense::map<uint16_t, s7codec::Endian> db_endianness_;
    int64_t first_ts_ms_ = 0;
    int64_t last_ts_ms_ = 0;
    uint64_t record_count_ = 0;

    // --- process() pass state, initialised by openWriters()/process() ---
    std::ofstream csv_out_;
    std::ofstream cache_f32_;
    std::ofstream cache_ts_;
    uint64_t cache_rows_ = 0;
    DatasetSummary summary_;
    std::vector<size_t> dtl_ts_feature_idx_; ///< features_*index of "…timestamp" DTL leaves
    int64_t deferred_row_ts_ = 0;            ///< ts of the not-yet-emitted batched row
    bool row_pending_ = false;
    /// Last full DB image per DB, carried across files like the JSONL
    /// current_state_: delta frames patch these in place.
    std::unordered_map<uint16_t, std::vector<uint8_t>> last_images_;
};

} // namespace sgrn::gateway::tools
