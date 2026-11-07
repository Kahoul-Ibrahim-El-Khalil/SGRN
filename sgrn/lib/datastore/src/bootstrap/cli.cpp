// sgrn/lib/datastore/src/bootstrap/cli.cpp
//
// Top-level CLI argument parsing and command dispatch for sgrn_datastore —
// definitions for include/sgrn/datastore/bootstrap/cli.hpp.

#include <sgrn/datastore/bootstrap/cli.hpp>

#include <fmt/core.h>
#include <sgrn/datastore/audit/AuditLogger.hpp>
#include <sgrn/datastore/bootstrap/backup.hpp>
#include <sgrn/datastore/bootstrap/bootstrap.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace sgrn::datastore::bootstrap::cli
{

std::optional<GlobalFlags> parseGlobalFlags(int t_argc, char** tp_argv) {
    GlobalFlags flags;

    for (int i = 1; i < t_argc; ++i) {
        const std::string_view arg = tp_argv[i];
        if (arg == "--non-interactive") {
            flags.non_interactive = true;
        } else if (arg == "--allow-insecure") {
            flags.allow_insecure = true;
        } else if (arg == "--force") {
            flags.force = true;
        } else if (arg == "--rotate") {
            if (i + 1 >= t_argc || std::string_view(tp_argv[i + 1]).starts_with("--")) {
                fmt::print(stderr, "error: --rotate needs a rotatable KEY (or KEY=VALUE);\n"
                                   "       e.g. --rotate POSTGRES_PASSWORD --rotate JWT_SECRET=some_value\n");
                return std::nullopt;
            }
            std::string spec = tp_argv[++i];
            const size_t eq = spec.find('=');
            flags.rotate_requests.emplace_back(spec.substr(0, eq), eq == std::string::npos ? "" : spec.substr(eq + 1));
        }
    }

    return flags;
}

std::optional<int> dispatch(int t_argc, char** tp_argv) {
    if (t_argc <= 1)
        return std::nullopt; // no subcommand → server mode

    const std::string_view first_arg(tp_argv[1]);

    if (first_arg == "--help" || first_arg == "-h") {
        bootstrap::printHelp(tp_argv[0]);
        return EXIT_SUCCESS;
    }

    // Parse alignment flags before dispatching any subcommand.
    auto maybe_flags = parseGlobalFlags(t_argc, tp_argv);
    if (!maybe_flags)
        return EXIT_FAILURE;
    const GlobalFlags& flags = *maybe_flags;

    // Helper: expand the well-known operation directory once.
    auto base_dir = [&]() { return sgrn::utils::filesystem::expandUserPath(std::string(bootstrap::kDefaultOperationDir)); };

    // --rotate (or any --rotate flag in the full argv)
    if (first_arg == "--rotate" || !flags.rotate_requests.empty()) {
        return bootstrap::rotate::runRotation(base_dir(), flags.rotate_requests, flags.non_interactive) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (first_arg == "--init") {
        bootstrap::bootstrapDatabase(flags.non_interactive, flags.allow_insecure, flags.force);
        return EXIT_SUCCESS;
    }

    if (first_arg == "--generate-config") {
        bootstrap::generateConfigOnly(base_dir());
        return EXIT_SUCCESS;
    }

    if (first_arg == "--init-db") {
        bootstrap::initDatabaseOnly(base_dir(), flags.allow_insecure, flags.force, flags.non_interactive);
        return EXIT_SUCCESS;
    }

    if (first_arg == "--config-systemd") {
        bootstrap::configureSystemd();
        return EXIT_SUCCESS;
    }

    if (first_arg == "--verify-install" || first_arg == "--verify") {
        return bootstrap::verifyInstall(base_dir(), flags.allow_insecure) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (first_arg == "--backup-db") {
        return bootstrap::backup::backupDatabase(base_dir()) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (first_arg == "--backup-garage") {
        return bootstrap::backup::backupGarage(base_dir()) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (first_arg == "--verify-backup") {
        const std::string target = t_argc > 2 ? tp_argv[2] : "";
        return bootstrap::backup::verifyBackup(base_dir(), target) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (first_arg == "--verify-audit") {
        const std::string org = t_argc > 2 ? tp_argv[2] : "";
        return audit::verifyAuditTrail(org) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (first_arg == "--backup-keys") {
        const std::string target = t_argc > 2 ? tp_argv[2] : "";
        return bootstrap::backup::backupKeyDb(base_dir(), target) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (first_arg == "--verify-keys") {
        return bootstrap::backup::verifyKeys(base_dir()) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    return std::nullopt; // unrecognised argument → fall through to server mode
}

} // namespace sgrn::datastore::bootstrap::cli
