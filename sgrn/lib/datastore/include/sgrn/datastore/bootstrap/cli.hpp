#pragma once
/**
 * @file  sgrn/datastore/bootstrap/cli.hpp
 * @brief Top-level CLI argument parsing and command dispatch for sgrn_datastore.
 *
 * Keeps main.cpp as a thin entry point: all flag scanning, option extraction,
 * and subcommand routing live here so they can be unit-tested independently.
 * Definitions live in src/bootstrap/cli.cpp.
 *
 * Usage:
 *   int main(int argc, char** argv) {
 *       if (auto rc = bootstrap::cli::dispatch(argc, argv))
 *           return *rc;          // subcommand ran — exit with its code
 *       // no subcommand → start the server
 *       ...
 *   }
 */

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sgrn::datastore::bootstrap::cli
{

// ---------------------------------------------------------------------------
// GlobalFlags — alignment flags that modify behaviour of any subcommand.
// ---------------------------------------------------------------------------
struct GlobalFlags {
    bool non_interactive = false;
    bool allow_insecure = false;
    bool force = false;
    std::vector<std::pair<std::string, std::string>> rotate_requests;
};

/**
 * @brief Scan argv for alignment flags that apply to every subcommand.
 *
 * Recognised flags (may appear anywhere in the argument list):
 *   --non-interactive   suppress all interactive prompts; CI-safe
 *   --allow-insecure    bypass the change_me_* placeholder gate
 *   --force             overwrite existing artefacts without asking
 *   --rotate KEY[=VAL]  secret rotation request (repeatable)
 *
 * @return Populated GlobalFlags, or std::nullopt on a flag parse error
 *         (error message already printed to stderr).
 */
std::optional<GlobalFlags> parseGlobalFlags(int t_argc, char** tp_argv);

/**
 * @brief Parse flags then dispatch to the appropriate subcommand.
 *
 * Returns std::nullopt when no subcommand was matched (caller should start
 * the normal server).  Otherwise returns the exit code the process should use.
 *
 * Deployment runbook order (see --help for the full narrative):
 *   1. --init               interactive wizard + config + DB
 *   2. --generate-config    write config file (no DB, no sudo)
 *   3. --init-db            apply schema (needs PostgreSQL)
 *   4. --config-systemd     install + enable systemd units (root)
 *   --rotate                live secret rotation without redeployment
 *   --backup-db / --backup-garage  independent atomic archives
 *   --verify-backup [path]  cross-check archive against PostgreSQL catalog
 */
std::optional<int> dispatch(int t_argc, char** tp_argv);

} // namespace sgrn::datastore::bootstrap::cli
