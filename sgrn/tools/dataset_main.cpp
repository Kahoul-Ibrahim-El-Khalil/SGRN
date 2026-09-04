#include <sgrn/gateway/tools/sgrn_dataset.hpp>

#include <fmt/color.h>
#include <fmt/core.h>
#include <cxxopts.hpp>
#include <iostream>

int main(int argc, char** argv) {
    cxxopts::Options options("sgrn_dataset", "SGRN Industrial Telemetry Dataset Processor & ML Manifest Generator");

    options.add_options()("i,input", "Input directory containing .jsonl / .jsonl.zst state files", cxxopts::value<std::string>())(
        "s,scl", "Path to SCL schema file (.scl)", cxxopts::value<std::string>())("c,csv", "Output CSV file path",
        cxxopts::value<std::string>()->default_value("dataset.csv"))("m,manifest", "Output ML manifest JSON file path",
        cxxopts::value<std::string>()->default_value("manifest.json"))("man", "Display detailed manual page")("h,help", "Print usage help");

    auto result = options.parse(argc, argv);

    if (result.count("man")) {
        std::cout << R"(SGRN_DATASET(1)                  User Commands                  SGRN_DATASET(1)

NAME
       sgrn_dataset - SGRN Industrial Telemetry Dataset Processor & Data Canonicalizer

SYNOPSIS
       sgrn_dataset -i DIR -s SCHEMA.scl [-c DATASET.csv] [-m MANIFEST.json]

DESCRIPTION
       sgrn_dataset is the C++ data canonicalization engine for the SGRN platform.
       It reads Zstd-compressed JSON Lines (.jsonl.zst) telemetry archives generated
       by the SGRN Gateway persistence service, reconciles state deltas, applies SCL
       schema rules (including #UNIT directives and categorical enum mappings), and
       exports a normalized CSV dataset alongside a structured Machine Learning
       manifest JSON file.

OPTIONS
       -i, --input DIR
              Path to input directory containing Zstd historian archives (.jsonl.zst). Required.

       -s, --scl FILE.scl
              Path to SCL schema file (.scl) defining PLC data blocks and #UNIT annotations. Required.

       -c, --csv FILE.csv
              Path to output CSV file for aligned time-series feature rows. Default: dataset.csv.

       -m, --manifest FILE.json
              Path to output ML Manifest JSON file containing feature taxonomy & stats. Default: manifest.json.

       --man
              Display this manual page and exit.

       -h, --help
              Display short command usage help and exit.

SCHEMA ANNOTATIONS & #UNIT CONVENTIONS
       Tag names in SCL schemas must be unit-agnostic. Physical units are annotated
       directly on variables using the #UNIT("...") directive:

       DATA_BLOCK "Reactor"
       {
           VERSION: '0.1'
       }
       STRUCT
           thermal_power   #UNIT("MW")   : REAL := 3400.0;
           przr_pressure   #UNIT("bar")  : REAL := 155.0;
       END_STRUCT
       END_DATA_BLOCK

SYSTEM ARCHITECTURE ROLES
       * SGRN Gateway: Live operational state twin executing protocol translation.
       * sgrn_dataset: C++ data canonicalizer producing row-aligned datasets.
       * SGRN Datastore: Persistent cloud substrate storing manifests, weights, & schemas.
)" << std::endl;
        return 0;
    }

    if (result.count("help") || !result.count("input") || !result.count("scl")) {
        std::cout << options.help() << std::endl;
        return 0;
    }

    sgrn::gateway::tools::DatasetConfig config;
    config.input_dir = result["input"].as<std::string>();
    config.scl_schema_path = result["scl"].as<std::string>();
    config.output_csv_path = result["csv"].as<std::string>();
    config.manifest_path = result["manifest"].as<std::string>();

    fmt::print(fg(fmt::color::cyan), "==================================================\n");
    fmt::print(fg(fmt::color::cyan), " SGRN Telemetry Dataset Processor\n");
    fmt::print(fg(fmt::color::cyan), "==================================================\n");
    fmt::print("Input directory: {}\n", config.input_dir);
    fmt::print("SCL Schema:      {}\n", config.scl_schema_path);
    fmt::print("CSV Output:      {}\n", config.output_csv_path);
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
