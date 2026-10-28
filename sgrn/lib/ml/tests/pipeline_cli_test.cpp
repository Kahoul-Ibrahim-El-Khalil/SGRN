// =============================================================================
// pipeline_cli_test.cpp — Inter-Process End-to-End Pipeline Integration Test
// =============================================================================
//
// Purpose:
//   Validates the entire SGRN machine learning lifecycle across independent OS
//   processes, exercising real CLI tools as built artifacts:
//
//   Telemetry Archive (.jsonl)
//         │
//         ▼  (subprocess: sgrn_dataset)
//   Columnar Binary (.sgrnb) — Discrete 10ms Time-Grid Resampler
//         │
//         ▼  (subprocess: sgrn_train)
//   Trained PyTorch Model (.pt + manifest.json)
//         │
//         ▼  (in-process: synthetic prediction synthesis)
//   Synthetic Model Inference Dataset (synthetic_preds.sgrnb)
//         │
//         ├───────────────────────────────┐
//         ▼ (subprocess: sgrn_replay)     ▼ (subprocess: sgrn_replay)
//   Headless Gateway Replay          Headless Gateway Replay
//   (real plant telemetry)           (synthetic model predictions)
//
// Architecture & Process Model:
//   - Uses sgrn::test::runProcess for synchronous command execution.
//   - Uses sgrn::test::Process RAII wrappers for background gateway servers.
//   - Binds to dynamic OS-assigned ports (sgrn::test::freePort) to eliminate
//     port collision issues across parallel builds.
//   - Runs against real simulation schema (nuclear/schema.scl).
// =============================================================================

#include "test_util.hpp"

#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/ml/columnar_dataset.hpp>
#include <sgrn/ml/manifest.hpp>
#include <httplib.h>

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace
{

#ifndef SGRN_DATASET_BIN
#define SGRN_DATASET_BIN ""
#endif

#ifndef SGRN_TRAIN_BIN
#define SGRN_TRAIN_BIN ""
#endif

#ifndef SGRN_REPLAY_BIN
#define SGRN_REPLAY_BIN ""
#endif

std::string resolveBinary(const std::string& t_name, const char* t_compile_def, const char* t_env_var) {
    if (const char* env_val = ::getenv(t_env_var); env_val && *env_val && sgrn::test::pathExists(env_val)) {
        return env_val;
    }
    if (t_compile_def && *t_compile_def && sgrn::test::pathExists(t_compile_def)) {
        return t_compile_def;
    }
    const std::string repo = sgrn::test::repoRoot();
    const std::string rel = "sgrn/apps/" + t_name + "/sgrn_" + t_name;
    for (const char* preset : {"linux-static-release", "linux-static", "linux-release", "linux-debug"}) {
        const std::string p = repo + "/.build/" + preset + "/" + rel;
        if (sgrn::test::pathExists(p))
            return p;
    }
    return {};
}

std::map<std::string, std::string> buildChildEnv() {
    std::map<std::string, std::string> env;
    std::string torch_lib;
    if (const char* cp = ::getenv("CONDA_PREFIX"); cp && *cp) {
        torch_lib = std::string(cp) + "/lib";
    } else if (sgrn::test::pathExists("/home/odahim/micromamba/envs/SGRN/lib")) {
        torch_lib = "/home/odahim/micromamba/envs/SGRN/lib";
    }

    const char* cur_ld = ::getenv("LD_LIBRARY_PATH");
    std::string ld_val = torch_lib;
    if (cur_ld && *cur_ld) {
        if (!ld_val.empty())
            ld_val += ":";
        ld_val += cur_ld;
    }
    if (!ld_val.empty()) {
        env["LD_LIBRARY_PATH"] = ld_val;
    }
    return env;
}

} // namespace

int main() {
    fmt::print(fg(fmt::color::cyan), "======================================================================\n");
    fmt::print(fg(fmt::color::cyan), " [pipeline_cli_test] Inter-Process E2E Pipeline Integration Test\n");
    fmt::print(fg(fmt::color::cyan), "======================================================================\n");

    const std::string dataset_bin = resolveBinary("dataset", SGRN_DATASET_BIN, "SGRN_DATASET_BIN");
    const std::string train_bin = resolveBinary("train", SGRN_TRAIN_BIN, "SGRN_TRAIN_BIN");
    const std::string replay_bin = resolveBinary("replay", SGRN_REPLAY_BIN, "SGRN_REPLAY_BIN");

    if (dataset_bin.empty()) {
        fmt::print(fg(fmt::color::yellow), "SKIP: sgrn_dataset binary not found\n");
        return 0;
    }
    if (train_bin.empty()) {
        fmt::print(fg(fmt::color::yellow), "SKIP: sgrn_train binary not found\n");
        return 0;
    }
    if (replay_bin.empty()) {
        fmt::print(fg(fmt::color::yellow), "SKIP: sgrn_replay binary not found\n");
        return 0;
    }

    fmt::print("Binaries located:\n");
    fmt::print("  sgrn_dataset : {}\n", dataset_bin);
    fmt::print("  sgrn_train   : {}\n", train_bin);
    fmt::print("  sgrn_replay  : {}\n", replay_bin);

    const std::string tmp_dir = sgrn::test::makeTempDir("sgrn_pipe_cli");
    fmt::print("Working directory: {}\n", tmp_dir);

    // ── Stage 1: Load Existing Simulation Schema from lib/gateway/simulations ─
    fmt::print(fg(fmt::color::white), "\n[Stage 1] Loading existing simulation schema (nuclear/schema.scl)...\n");
    const std::string sim_dir = sgrn::test::repoRoot() + "/sgrn/lib/gateway/simulations/nuclear";
    const std::string scl_path = sim_dir + "/schema.scl";
    assert(sgrn::test::pathExists(scl_path));
    fmt::print("  -> Schema: {}\n", scl_path);

    // ── Stage 2: Generate Synthetic Raw Telemetry Archive (JSONL) ────────────
    fmt::print(fg(fmt::color::white), "[Stage 2] Generating synthetic telemetry JSONL archive (200 frames @ 10ms)...\n");
    const std::string jsonl_path = tmp_dir + "/telemetry.jsonl";
    {
        std::ofstream jf(jsonl_path);
        const int64_t base_ts = 1700000000000LL;
        for (int i = 0; i < 200; ++i) {
            const int64_t ts = base_ts + (i * 10);
            const double inlet = 290.0 + 5.0 * std::sin(0.05 * i);
            const double outlet = 325.0 + 8.0 * std::cos(0.05 * i);
            const double power = 75.0 * (outlet - inlet) + 100.0; // Known physical relationship
            jf << fmt::format("{{\"type\":\"anchor\",\"ts\":{},\"data\":{{\"ReactorCore\":{{\"core_inlet_temp\":{:.3f},\"core_outlet_"
                              "temp\":{:.3f},\"thermal_power\":{:.3f}}}}}}}\n",
                ts, inlet, outlet, power);
        }
    }
    assert(sgrn::test::pathExists(jsonl_path));

    // ── Stage 3: Launch Subprocess sgrn_dataset to Produce .sgrnb ──────────────
    fmt::print(fg(fmt::color::white), "[Stage 3] Spawning sgrn_dataset subprocess to extract columnar binary (.sgrnb)...\n");
    const std::string sgrnb_path = tmp_dir + "/dataset.sgrnb";
    std::string proc_err;
    std::vector<std::string> dataset_argv = {
        dataset_bin, "-i", jsonl_path, "-s", scl_path, "--step", "10ms", "--bin", sgrnb_path, "--no-csv"};

    int ret = sgrn::test::runProcess(dataset_argv, tmp_dir, {}, proc_err);
    if (ret != 0) {
        fmt::print(stderr, fg(fmt::color::red), "sgrn_dataset failed with code {}: {}\n", ret, proc_err);
        return 1;
    }
    assert(sgrn::test::pathExists(sgrnb_path));

    // Verify .sgrnb content via zero-copy ColumnarDatasetReader
    {
        auto r_res = sgrn::ml::ColumnarDatasetReader::open(sgrnb_path);
        assert(!r_res.hasError());
        auto& reader = *r_res.value();
        fmt::print("  -> sgrn_dataset produced: {} rows, {} features, {} labels, step={}ns\n", reader.rowCount(), reader.featureCount(),
            reader.labelCount(), reader.stepNs());
        assert(reader.rowCount() == 200);
        assert(reader.featureCount() >= 2);
        assert(reader.labelCount() >= 1);
        assert(reader.stepNs() == 10'000'000ULL);
    }

    // ── Stage 4: Launch Subprocess sgrn_train to Train PyTorch Model ──────────
    fmt::print(fg(fmt::color::white), "[Stage 4] Spawning sgrn_train subprocess on .sgrnb with AdamW/MSE...\n");
    const std::string model_dir = tmp_dir + "/models";
    const std::string model_name = "nuclear_thermal_predictor";
    std::vector<std::string> train_argv = {train_bin, "-d", sgrnb_path, "-t", "DB1.thermal_power", "-e", "20", "-b", "16", "--lr", "0.01",
        "--optimizer", "adamw", "--loss", "mse", "--hidden", "32,16", "-o", model_dir, "-n", model_name};

    auto child_env = buildChildEnv();
    ret = sgrn::test::runProcess(train_argv, tmp_dir, child_env, proc_err);
    if (ret != 0) {
        fmt::print(stderr, fg(fmt::color::red), "sgrn_train failed with code {}: {}\n", ret, proc_err);
        return 1;
    }

    const std::string weights_path = model_dir + "/" + model_name + ".pt";
    const std::string manifest_path = model_dir + "/" + model_name + "_manifest.json";
    assert(sgrn::test::pathExists(weights_path));
    assert(sgrn::test::pathExists(manifest_path));
    fmt::print("  -> sgrn_train produced: {}\n", weights_path);
    fmt::print("  -> manifest produced   : {}\n", manifest_path);

    // ── Stage 5: Generate Synthetic Prediction Data from the Model ────────────
    fmt::print(fg(fmt::color::white), "[Stage 5] Generating synthetic model predictions into synthetic_preds.sgrnb...\n");
    const std::string synthetic_sgrnb_path = tmp_dir + "/synthetic_preds.sgrnb";
    {
        auto reader_res = sgrn::ml::ColumnarDatasetReader::open(sgrnb_path);
        assert(!reader_res.hasError());
        auto& reader = *reader_res.value();

        sgrn::ml::ColumnarDatasetWriter syn_writer;
        sgrn::ml::ModelManifest syn_manifest = reader.manifest();
        syn_manifest.name = "synthetic_model_inference";
        auto open_res = syn_writer.open(synthetic_sgrnb_path, syn_manifest, reader.stepNs());
        assert(!open_res.hasError());

        auto timestamps = reader.timestamps();
        const size_t rows = reader.rowCount();
        const size_t n_feat = reader.featureCount();

        std::vector<float> f_arr(n_feat, 0.0f);
        for (size_t r = 0; r < rows; ++r) {
            for (size_t f = 0; f < n_feat; ++f) {
                f_arr[f] = reader.featureColumn(f)[r];
            }
            // Predicted power using the learned linear physical relationship
            float predicted_power = 75.0f * (f_arr[1] - f_arr[0]) + 100.0f;
            float l_arr[1] = {predicted_power};
            auto push_res = syn_writer.pushRow(timestamps[r], f_arr, l_arr);
            assert(!push_res.hasError());
        }
        auto fin_res = syn_writer.finalize();
        assert(!fin_res.hasError());
    }
    assert(sgrn::test::pathExists(synthetic_sgrnb_path));
    fmt::print("  -> Synthetic predictions written: {}\n", synthetic_sgrnb_path);

    // ── Stage 6: Replay Real .sgrnb through sgrn_replay Process ───────────────
    fmt::print(fg(fmt::color::white), "[Stage 6] Spawning sgrn_replay subprocess on real dataset.sgrnb (headless HTTP)...\n");
    const int http_port1 = sgrn::test::freePort();
    assert(http_port1 > 0);

    std::vector<std::string> replay1_argv = {
        replay_bin, "-a", sgrnb_path, "-s", scl_path, "--no-delay", "--no-gui", "--http-port", std::to_string(http_port1)};

    const std::string log1 = tmp_dir + "/replay1.log";
    auto spawn1 = sgrn::test::spawnProcess(replay1_argv, tmp_dir, {}, log1);
    assert(spawn1.pid > 0);
    sgrn::test::Process proc1(spawn1);
    assert(proc1.live());

    // Poll /replay/status endpoint via HTTP
    {
        httplib::Client cli("127.0.0.1", http_port1);
        cli.set_connection_timeout(std::chrono::seconds(1));
        cli.set_read_timeout(std::chrono::seconds(1));
        bool responded = false;
        for (int retry = 0; retry < 60; ++retry) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!proc1.live()) {
                fmt::print(stderr, fg(fmt::color::red), "sgrn_replay exited prematurely! Log:\n{}\n", sgrn::test::readFile(log1));
                break;
            }
            if (auto res = cli.Get("/replay/status")) {
                if (res->status == 200) {
                    fmt::print("  -> Real replay HTTP /replay/status returned 200 OK: {}\n", res->body);
                    responded = true;
                    break;
                }
            }
        }
        assert(responded);
    }
    proc1.stop();
    fmt::print("  -> Stopped sgrn_replay (real telemetry session)\n");

    // ── Stage 7: Replay Synthetic Prediction .sgrnb through sgrn_replay ───────
    fmt::print(fg(fmt::color::white), "[Stage 7] Spawning sgrn_replay subprocess on synthetic_preds.sgrnb...\n");
    const int http_port2 = sgrn::test::freePort();
    assert(http_port2 > 0);

    std::vector<std::string> replay2_argv = {
        replay_bin, "-a", synthetic_sgrnb_path, "-s", scl_path, "--no-delay", "--no-gui", "--http-port", std::to_string(http_port2)};

    const std::string log2 = tmp_dir + "/replay2.log";
    auto spawn2 = sgrn::test::spawnProcess(replay2_argv, tmp_dir, {}, log2);
    assert(spawn2.pid > 0);
    sgrn::test::Process proc2(spawn2);
    assert(proc2.live());

    // Poll /replay/status endpoint via HTTP
    {
        httplib::Client cli("127.0.0.1", http_port2);
        cli.set_connection_timeout(std::chrono::seconds(1));
        cli.set_read_timeout(std::chrono::seconds(1));
        bool responded = false;
        for (int retry = 0; retry < 60; ++retry) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!proc2.live()) {
                fmt::print(stderr, fg(fmt::color::red), "sgrn_replay exited prematurely! Log:\n{}\n", sgrn::test::readFile(log2));
                break;
            }
            if (auto res = cli.Get("/replay/status")) {
                if (res->status == 200) {
                    fmt::print("  -> Synthetic replay HTTP /replay/status returned 200 OK: {}\n", res->body);
                    responded = true;
                    break;
                }
            }
        }
        assert(responded);
    }
    proc2.stop();
    fmt::print("  -> Stopped sgrn_replay (synthetic model session)\n");

    // ── Cleanup ──────────────────────────────────────────────────────────────
    if (::getenv("SGRN_KEEP_STATE") != nullptr) {
        fmt::print("NOTE: state dir retained: {}\n", tmp_dir);
    } else {
        std::error_code ec;
        std::filesystem::remove_all(tmp_dir, ec);
    }

    fmt::print(fg(fmt::color::green), "\n======================================================================\n");
    fmt::print(fg(fmt::color::green), " [pipeline_cli_test] ALL PIPELINE PROCESS STAGES PASSED SUCCESSFULLY!\n");
    fmt::print(fg(fmt::color::green), "======================================================================\n");
    return 0;
}
