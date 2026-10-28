#include <sgrn/gateway/tools/sgrn_dataset.hpp>

#include "man_message.hpp"
#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/utils/time.hpp>
#include <cxxopts.hpp>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
int main(int t_argc, char** tp_argv) {
    cxxopts::Options options("sgrn_dataset", "SGRN Industrial Telemetry Dataset Processor & Format Converter");

    options.add_options()("i,input", "Input file(s) or directorie(s) containing telemetry state archives (repeatable)",
        cxxopts::value<std::vector<std::string>>())("o,output", "Output file path (default: stdout or infer based on conversion)",
        cxxopts::value<std::string>())("s,scl", "Path to SCL schema file (.scl)", cxxopts::value<std::string>())(
        "c,convert", "Convert file to target format (-f jsonl|binary)", cxxopts::value<std::string>())(
        "f,format", "Target format for conversion ('binary', 'jsonl', 'csv')", cxxopts::value<std::string>()->default_value("auto"))(
        "d,decompress", "Decompress target file to raw uncompressed stdout/file")("z,compress", "Compress target file with Zstd")(
        "csv", "Output CSV file path for dataset processing", cxxopts::value<std::string>()->default_value("dataset.csv"))(
        "m,manifest", "Output ML manifest JSON file path", cxxopts::value<std::string>()->default_value("manifest.json"))("merge",
        "Merge all archives under the input path into a single archive file",
        cxxopts::value<std::string>())("bin", "Output columnar binary dataset (.sgrnb) file path", cxxopts::value<std::string>())("step",
        "Override discrete time-grid step (e.g. '100ms', '1s', '500us')",
        cxxopts::value<std::string>())("time-tag", "Override the timestamp clock tag for resampling", cxxopts::value<std::string>())(
        "no-csv", "Skip CSV output entirely (only produce .sgrnb)")("man", "Display detailed manual page")("h,help", "Print usage help");

    options.parse_positional({"input"});

    auto result = options.parse(t_argc, tp_argv);

    if (result.count("man")) {
        fmt::print(MAN_MESSAGE);
        return 0;
    }

    if (result.count("help")) {
        fmt::print("{}\n", options.help());
        return 0;
    }

    std::vector<std::string> input_paths;
    if (result.count("input")) {
        input_paths = result["input"].as<std::vector<std::string>>();
    } else if (result.count("convert")) {
        input_paths.push_back(result["convert"].as<std::string>());
    }

    // Merge mode: concatenate archives into one (takes precedence over convert).
    // Accepts any mix of files and directories, in the order given.
    if (result.count("merge")) {
        if (input_paths.empty()) {
            fmt::print(stderr, "Error: Input file(s) or directorie(s) required for merging (-i <path> [-i <path>...])\n");
            return 1;
        }
        std::string target_fmt;
        if (result.count("format") && result["format"].as<std::string>() != "auto") {
            target_fmt = result["format"].as<std::string>();
        }
        sgrn::gateway::tools::DatasetProcessor processor;
        const std::vector<std::filesystem::path> merge_inputs(input_paths.begin(), input_paths.end());
        auto res = processor.mergeArchives(merge_inputs, result["merge"].as<std::string>(), target_fmt);
        if (res.hasError()) {
            fmt::print(fg(fmt::color::red), "Merge error: {}\n", res.error());
            return 1;
        }
        return 0;
    }

    if (input_paths.size() > 1) {
        fmt::print("Error: convert/process modes take a single input; pass a directory, or merge inputs first (--merge).\n");
        return 1;
    }
    const std::string input_path = input_paths.empty() ? std::string{} : input_paths.front();

    // Conversion or Decompression mode
    if (result.count("convert") || (result.count("format") && result["format"].as<std::string>() != "auto") || result.count("decompress") ||
        result.count("compress")) {
        if (input_path.empty()) {
            fmt::print("Error: Input file required for conversion/compression (-i <file>)\n");
            return 1;
        }

        std::string target_fmt = result.count("format") ? result["format"].as<std::string>() : "auto";
        std::string out_path = result.count("output") ? result["output"].as<std::string>() : "";

        if (out_path.empty()) {
            if (target_fmt == "jsonl") {
                out_path = input_path + ".converted.jsonl.zst";
            } else if (target_fmt == "binary" || target_fmt == "bin") {
                out_path = input_path + ".converted.bin.zst";
            } else {
                out_path = input_path + ".out";
            }
        }

        sgrn::gateway::tools::DatasetProcessor processor;
        auto res = processor.convertFormat(input_path, out_path, target_fmt);
        if (res.hasError()) {
            fmt::print(fg(fmt::color::red), "Conversion error: {}\n", res.error());
            return 1;
        }
        fmt::print(fg(fmt::color::green), "[sgrn_dataset] Processed {} -> {}\n", input_path, out_path);
        return 0;
    }

    // Dataset processing mode
    if (input_path.empty() || !result.count("scl")) {
        std::cout << options.help() << std::endl;
        return 0;
    }

    sgrn::gateway::tools::DatasetConfig config;
    config.input_dir = input_path;
    config.scl_schema_path = result["scl"].as<std::string>();
    config.no_csv = result.count("no-csv") > 0;
    if (!config.no_csv) {
        config.output_csv_path = result.count("output") ? result["output"].as<std::string>() : result["csv"].as<std::string>();
        if (!config.output_csv_path.empty())
            fmt::print(fg(fmt::color::cyan), "CSV Output:      {}\n", config.output_csv_path);
    }
    config.manifest_path = result["manifest"].as<std::string>();

    // --- Columnar binary (.sgrnb) options ---
    if (result.count("bin")) {
        config.output_bin_path = result["bin"].as<std::string>();
    }
    if (result.count("step")) {
        const std::string step_str = result["step"].as<std::string>();
        auto parsed = sgrn::utils::time::parseDurationNanoseconds(step_str);
        if (!parsed.has_value()) {
            fmt::print(stderr, fg(fmt::color::red), "Error: invalid --step value '{}' (try e.g. '100ms', '1s', '500us')\n", step_str);
            return 1;
        }
        config.step_override_ns = parsed.value();
    }
    if (result.count("time-tag")) {
        config.time_tag_override = result["time-tag"].as<std::string>();
    }

    fmt::print(fg(fmt::color::cyan), "==================================================\n");
    fmt::print(fg(fmt::color::cyan), " SGRN Telemetry Dataset Processor\n");
    fmt::print(fg(fmt::color::cyan), "==================================================\n");
    fmt::print("Input directory: {}\n", config.input_dir);
    fmt::print("SCL Schema:      {}\n", config.scl_schema_path);
    if (!config.no_csv)
        fmt::print("CSV Output:      {}\n", config.output_csv_path);
    if (!config.output_bin_path.empty())
        fmt::print("Binary Output:   {}\n", config.output_bin_path);
    if (config.step_override_ns > 0)
        fmt::print("Step Override:   {} ns\n", config.step_override_ns);
    fmt::print("Manifest Output: {}\n", config.manifest_path);

    sgrn::gateway::tools::DatasetProcessor processor;
    auto proc_res = processor.process(config);

    if (proc_res.hasError()) {
        fmt::print(fg(fmt::color::red), "Error: {}\n", proc_res.error());
        return 1;
    }

    const auto& summary = proc_res.value();
    fmt::print(fg(fmt::color::green), "\n[DatasetProcessor] Success!\n");
    fmt::print("Files processed:   {}\n", summary.total_files_processed);
    fmt::print("Total records:     {}\n", summary.total_records_processed);
    fmt::print("Total timestamps:  {}\n", summary.total_timestamps);
    fmt::print("Features extracted:{}\n", summary.features.size());

    return 0;
}
