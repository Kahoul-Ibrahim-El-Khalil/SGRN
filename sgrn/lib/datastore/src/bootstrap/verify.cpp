// sgrn/lib/datastore/src/bootstrap/verify.cpp
//
// Install health and drift verification (--verify-install) — definitions for
// include/sgrn/datastore/bootstrap/verify.hpp.

#include <sgrn/datastore/bootstrap/verify.hpp>

#include <fmt/core.h>
#include <sgrn/datastore/bootstrap/bootstrap-helpers.hpp>
#include <sgrn/datastore/bootstrap/secrets.hpp>
#include <sgrn/datastore/bootstrap/systemd.hpp>
#include <sgrn/utils/env.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <sgrn/utils/process.hpp>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace sgrn::datastore::bootstrap
{

using sgrn::utils::filesystem::whichInPaths;
using sgrn::utils::process::captureOutput;
using sgrn::utils::process::execute;

namespace
{

// ---------------------------------------------------------------------------
// Unit-file / path helpers
// ---------------------------------------------------------------------------

/// First Environment="PATH=..." value in a systemd unit file; empty when absent.
std::string unitEnvPath(const std::string& t_unit_text) {
    size_t pos = 0;
    while ((pos = t_unit_text.find("Environment=", pos)) != std::string::npos) {
        size_t val_start = t_unit_text.find('"', pos);
        size_t line_end = t_unit_text.find('\n', pos);
        if (val_start == std::string::npos || (line_end != std::string::npos && val_start > line_end))
            break;
        size_t val_end = t_unit_text.find('"', val_start + 1);
        if (val_end == std::string::npos)
            break;
        std::string kv = t_unit_text.substr(val_start + 1, val_end - val_start - 1);
        if (kv.starts_with("PATH="))
            return kv.substr(5);
        pos = val_end + 1;
    }
    return {};
}

/// First integer behind "port": in a sgrn.json (0 when absent) — used for the
/// live port check of the datastore's own listener.
int firstConfiguredPort(const std::string& t_sgrn_json_text) {
    size_t key = t_sgrn_json_text.find("\"port\"");
    if (key == std::string::npos)
        return 0;
    const auto is_digit = [](char t_c) { return t_c >= '0' && t_c <= '9'; };
    for (size_t i = key + 6; i < t_sgrn_json_text.size(); ++i) {
        char c = t_sgrn_json_text[i];
        if (is_digit(c))
            return std::atoi(t_sgrn_json_text.c_str() + i);
        if (c == ':' || c == '"' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
            continue;
        return 0; // unexpected structure before the number
    }
    return 0;
}

} // namespace

// ---------------------------------------------------------------------------
// verifyInstall orchestrator
// ---------------------------------------------------------------------------

int verifyInstall(const std::string& t_base_dir) {
    namespace fs = std::filesystem;
    VerifyReport report;

    SGRN_INFO("DatastoreInit", "Verifying SGRN datastore install under {}...", t_base_dir);

    // 0. .env present + parseable
    fs::path env_path = fs::path(t_base_dir) / ".env";
    if (auto env_result = sgrn::utils::env::loadFile(env_path); env_result.hasError()) {
        report.fail(fmt::format(".env not loadable ({}): run --generate-config first", env_path.string()));
    } else if (envHasPlaceholderSecret()) {
        report.fail(".env still contains change_me_* placeholder secrets — run --init or --rotate KEY=VALUE");
    } else {
        report.pass(".env loaded");
    }

    // 1. staged unit templates exist and are non-empty.
    std::vector<std::string> staged_units;
    fs::path systemd_stage = fs::path(t_base_dir) / "systemd";
    std::error_code iter_ec;
    if (fs::exists(systemd_stage, iter_ec)) {
        for (const auto& entry : fs::directory_iterator(systemd_stage, iter_ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".service" && fs::file_size(entry.path()) > 0)
                staged_units.push_back(entry.path().filename().string());
        }
    }
    std::sort(staged_units.begin(), staged_units.end());
    if (staged_units.empty()) {
        report.fail(fmt::format("no staged systemd units at {} — run --generate-config", systemd_stage.string()));
    } else {
        report.pass(fmt::format("{} staged systemd unit(s)", staged_units.size()));
    }

    // 2. deployed units match staged templates (byte-level drift check).
    const fs::path systemd_dst_path = "/etc/systemd/system";
    for (const auto& unit : staged_units) {
        fs::path live = systemd_dst_path / unit;
        if (!fs::exists(live)) {
            report.fail(fmt::format("{} staged but NOT deployed — sudo sgrn_datastore --config-systemd", unit));
        } else if (!sgrn::utils::filesystem::fileContentsEqual(fs::path(systemd_stage) / unit, live)) {
            report.fail(fmt::format("{} drifted from staged template — sudo sgrn_datastore --config-systemd", unit));
        } else {
            report.pass(fmt::format("{} deployed == staged", unit));
        }
    }

    // 3. deployed SGRN units without a staged counterpart (retired service
    // lingering; --config-systemd's obsolete-unit sweep only clears its own
    // known list, so keep eyes on anything else here).
    for (const auto& entry : fs::directory_iterator(systemd_dst_path, iter_ec)) {
        std::string name = entry.path().filename().string();
        if (entry.is_regular_file() && name.starts_with("SGRN-") && name.ends_with(".service") &&
            std::find(staged_units.begin(), staged_units.end(), name) == staged_units.end()) {
            report.fail(fmt::format("deployed unit {} has no staged counterpart (obsolete/unmanaged)", name));
        }
    }

    // 4. staged nginx tree health.
    std::string nginx_text = sgrn::utils::filesystem::readTextFile(fs::path(t_base_dir) / "nginx" / "nginx.conf");
    if (nginx_text.empty()) {
        report.fail(fmt::format("no staged nginx.conf at {}/nginx/nginx.conf — run --generate-config", t_base_dir));
    } else if (nginx_text.find("${") != std::string::npos) {
        report.fail("staged nginx.conf still contains ${...} template placeholders — regenerate it");
    } else if (nginx_text.find("daemon off;") == std::string::npos) {
        report.fail("staged nginx.conf lacks 'daemon off;' — systemd MAINPID tracking breaks");
    } else {
        report.pass("staged nginx.conf template healthy");
    }

    {
        bool has_sites = false;
        fs::path sites_dir = fs::path(t_base_dir) / "nginx" / "sites-enabled";
        std::error_code sites_ec;
        if (fs::exists(sites_dir, sites_ec)) {
            for (const auto& entry : fs::directory_iterator(sites_dir, sites_ec)) {
                if (entry.is_regular_file() && entry.path().filename() != "nginx.service") {
                    has_sites = true;
                    break;
                }
            }
        }
        if (has_sites) {
            report.pass("staged nginx sites-enabled non-empty");
        } else {
            report.fail("staged nginx sites-enabled empty — nginx would run with zero server blocks");
        }
    }

    // 5. datastore binary + TLS certs.
    {
        std::string bin_dir = sgrn::utils::env::get("SGRN_BIN_DIR", "/usr/local/bin");
        if (hasDatastoreBinary(bin_dir)) {
            report.pass(fmt::format("sgrn_datastore binary at {}/sgrn_datastore", bin_dir));
        } else {
            report.fail(fmt::format("no sgrn_datastore in SGRN_BIN_DIR={} — rebuild/install, re-run --generate-config", bin_dir));
        }

        std::string data_dir = sgrn::utils::env::get("SGRN_DATA_DIR", t_base_dir);
        fs::path cert_dir = fs::path(data_dir) / "certs";
        if (fs::exists(cert_dir / "server.crt") && fs::exists(cert_dir / "server.key")) {
            report.pass("TLS certs present (certs/server.crt + server.key)");
        } else {
            report.fail(fmt::format("TLS certs missing at {} — --generate-config generates them", cert_dir.string()));
        }

        // 6. nginx buffer dirs under the -p prefix (mkdir() not recursive).
        for (const char* kTempSub : {"client", "proxy", "fastcgi", "uwsgi", "scgi"}) {
            fs::path temp_dir = fs::path(data_dir) / "var" / "tmp" / "nginx" / kTempSub;
            if (fs::is_directory(temp_dir)) {
                report.pass(fmt::format("nginx buffer dir {}", temp_dir.string()));
            } else {
                report.fail(fmt::format("missing {}: starts die with '[emerg] mkdir() ... failed'", temp_dir.string()));
            }
        }
    }

    // 7. deployment env prerequisites: nginx binary + mime.types.
    {
        std::string deployment_env = sgrn::utils::env::get("SGRN_DEPLOYMENT_ENV", "");
        if (deployment_env.empty() || deployment_env.find("$ENV{") != std::string::npos) {
            report.fail(
                fmt::format("SGRN_DEPLOYMENT_ENV unresolved in .env (got '{}') — set it and re-run --generate-config", deployment_env));
        } else {
            std::string nginx_bin;
            for (const char* kSub : {"bin", "sbin"}) {
                fs::path candidate = fs::path(deployment_env) / kSub / "nginx";
                std::error_code ec;
                auto st = fs::status(candidate, ec);
                if (!ec && fs::is_regular_file(st)) {
                    nginx_bin = candidate.string();
                    break;
                }
            }
            if (nginx_bin.empty()) {
                report.fail(fmt::format("nginx binary not in {}/bin or {}/sbin", deployment_env, deployment_env));
            } else {
                report.pass(fmt::format("nginx binary at {}", nginx_bin));
            }

            if (fs::exists(fs::path(deployment_env) / "etc" / "nginx" / "mime.types")) {
                report.pass("mime.types available in deployment env");
            } else {
                report.fail(fmt::format("mime.types missing in {}/etc/nginx — staged conf includes it", deployment_env));
            }
        }
    }

    // 8. garage binary resolvable on the DEPLOYED unit's PATH.
    {
        std::string garage_unit = sgrn::utils::filesystem::readTextFile(fs::path("/etc/systemd/system") / "SGRN-garage.service");
        std::string unit_paths = unitEnvPath(garage_unit);
        if (unit_paths.empty() || garage_unit.empty()) {
            report.fail("SGRN-garage.service not deployed or has no PATH environment");
        } else if (whichInPaths(unit_paths, "garage").empty()) {
            report.fail(
                fmt::format("garage NOT found on SGRN-garage unit PATH '{}' — place binary/symlink in one of these dirs", unit_paths));
        } else {
            report.pass("garage binary resolvable on deployed unit PATH");
        }
    }

    // 9. live services + listening ports (skipped outside systemd).
    {
        std::string sys_state = captureOutput("systemctl is-system-running 2>/dev/null");
        const bool systemd_up = sys_state.rfind("running", 0) == 0 || sys_state.rfind("degraded", 0) == 0;
        if (systemd_up) {
            for (const auto& unit : staged_units) {
                const bool oneshot = unitType(unit) == "oneshot";
                const std::string state = captureOutput(fmt::format("systemctl show -p ActiveState --value {} 2>/dev/null", unit));
                const std::string trimmed = state.substr(0, state.find_first_of("\r\n"));
                if (oneshot) {
                    report.pass(fmt::format("service {} oneshot (state: {})", unit, trimmed.empty() ? "?" : trimmed));
                } else if (execute(fmt::format("systemctl is-active --quiet {} 2>/dev/null", unit)) == 0) {
                    report.pass(fmt::format("service {} active", unit));
                } else {
                    report.fail(fmt::format("service {} NOT active", unit));
                }
            }

            std::string data_dir = sgrn::utils::env::get("SGRN_DATA_DIR", t_base_dir);
            int ds_port = firstConfiguredPort(sgrn::utils::filesystem::readTextFile(fs::path(data_dir) / "sgrn.json"));
            if (ds_port <= 0) {
                report.fail("sgrn.json listeners: no \"port\": entry found (datastore cannot bind)");
                ds_port = 0;
            }
            std::vector<std::pair<int, const char*>> ports = {
                {80, "nginx HTTP (redirect)"}, {443, "nginx TLS"}, {3900, "garage S3"}, {3903, "garage admin"}};
            if (ds_port > 0)
                ports.push_back({ds_port, "datastore (sgrn.json listener)"});
            for (const auto& [port, label] : ports) {
                if (execute(fmt::format("ss -ltn 2>/dev/null | grep -qE ':{:d}[[:space:]]'", port)) == 0) {
                    report.pass(fmt::format("port {} ({}): listening", port, label));
                } else {
                    report.fail(fmt::format("port {} ({}): not listening", port, label));
                }
            }
        } else {
            SGRN_WARN("DatastoreInit", "systemd not running here; skipping live service and port checks.");
        }
    }

    SGRN_INFO("DatastoreInit", "");
    SGRN_INFO("DatastoreInit", "Verification: {} ok, {} failed", report.passed, report.failed);
    if (report.failed == 0) {
        SGRN_INFO("DatastoreInit", "Install healthy.");
        return 0;
    }
    SGRN_ERROR("DatastoreInit", "Drift detected. Standard repair sequence:");
    SGRN_INFO("DatastoreInit", "  1. sgrn_datastore --generate-config        (user: refresh staged templates)");
    SGRN_INFO("DatastoreInit", "  2. sudo sgrn_datastore --config-systemd   (root: deploy + restart services)");
    return report.failed;
}

} // namespace sgrn::datastore::bootstrap
