// sgrn/lib/datastore/src/bootstrap/systemd.cpp
//
// systemd service management helpers — definitions for
// include/sgrn/datastore/bootstrap/systemd.hpp.

#include <sgrn/datastore/bootstrap/systemd.hpp>

#include <fmt/core.h>
#include <sgrn/debug.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <sgrn/utils/process.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <pwd.h>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>

namespace sgrn::datastore::bootstrap
{

using sgrn::utils::process::execute;

// ---------------------------------------------------------------------------
// Sudo-user resolution
// ---------------------------------------------------------------------------

std::string resolveSudoUser() {
    std::string sudo_user = sgrn::utils::process::resolveSudoUser();
    if (sudo_user.empty() || sudo_user == "root") {
        SGRN_ERROR("DatastoreInit", "Error: Could not resolve the non-root sudo user.");
        std::exit(EXIT_FAILURE);
    }
    return sudo_user;
}

// ---------------------------------------------------------------------------
// Legacy / obsolete service cleanup
// ---------------------------------------------------------------------------

void stopLegacyServices(const std::string& t_sudo_user, uid_t t_user_uid) {
    SGRN_INFO("DatastoreInit", "Cleaning up any legacy user-level services...");
    // NOTE: SGRN-postgrest.service is intentionally still stopped here: it is
    // obsolete (no longer deployed), so any lingering user-level instance
    // must be torn down, never started.
    std::string cmd = fmt::format("sudo -u {} XDG_RUNTIME_DIR=/run/user/{} systemctl --user stop "
                                  "SGRN-nginx.service sgrn.service SGRN-datastore.service "
                                  "SGRN-garage.service SGRN-minio.service SGRN-postgres.service SGRN-postgrest.service "
                                  "2>/dev/null || true",
        t_sudo_user, t_user_uid);
    execute(cmd);
}

void removeObsoleteUnits(const std::filesystem::path& t_data_dir) {
    namespace fs = std::filesystem;
    static constexpr std::string_view kObsoleteUnits[] = {
        "SGRN-postgrest.service", "SGRN-minio.service", "SGRN-redis.service", "SGRN-api.service"};
    static constexpr std::string_view kObsoleteDataFiles[] = {"systemd/SGRN-postgrest.service", "systemd/SGRN-minio.service",
        "systemd/SGRN-redis.service", "systemd/SGRN-api.service", "postgrest.conf"};

    for (auto unit : kObsoleteUnits) {
        SGRN_INFO("DatastoreInit", "Removing obsolete unit {} (if present)...", unit);
        execute(fmt::format("systemctl disable --now {} 2>/dev/null || true", unit));
        std::error_code ec;
        fs::remove(fs::path("/etc/systemd/system") / unit, ec);
        if (!ec)
            SGRN_INFO("DatastoreInit", "  -> Removed /etc/systemd/system/{}", unit);
    }
    for (auto rel : kObsoleteDataFiles) {
        std::error_code ec;
        fs::remove(fs::path(t_data_dir) / rel, ec);
        if (!ec)
            SGRN_INFO("DatastoreInit", "  -> Removed {}/{}", t_data_dir.string(), rel);
    }
}

// ---------------------------------------------------------------------------
// Service ordering (dependency tier sort)
// ---------------------------------------------------------------------------

std::vector<std::string> orderServicesForRestart(std::vector<std::string> t_services) {
    std::vector<std::string> ordered;
    for (auto& svc : t_services) {
        if (std::find(ordered.begin(), ordered.end(), svc) == ordered.end())
            ordered.push_back(std::move(svc));
    }
    auto tier = [](const std::string& t_svc) {
        if (t_svc == "SGRN-postgres.service")
            return 0;
        if (t_svc == "SGRN-garage.service")
            return 1;
        if (t_svc == "SGRN-nginx.service")
            return 3;
        if (t_svc == "sgrn.service")
            return 4;
        return 2; // SGRN-datastore.service and anything unknown
    };
    std::stable_sort(ordered.begin(), ordered.end(), [&](const std::string& t_a, const std::string& t_b) { return tier(t_a) < tier(t_b); });
    return ordered;
}

// ---------------------------------------------------------------------------
// Unit introspection helpers
// ---------------------------------------------------------------------------

std::string unitType(const std::string& t_svc) {
    std::string out = sgrn::utils::process::captureOutput(fmt::format("systemctl show -p Type --value {} 2>/dev/null", t_svc), 64);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

void logUnitStatus(const std::string& t_svc) {
    std::string out = sgrn::utils::process::captureOutput(fmt::format("systemctl status {} --no-pager -l -n 15 2>&1", t_svc), 4000);
    SGRN_ERROR("DatastoreInit", "--- systemctl status {} ---\n{}", t_svc, out);
}

// ---------------------------------------------------------------------------
// Service restart / enable
// ---------------------------------------------------------------------------

bool restartOneService(const std::string& t_svc) {
    // Clear any latched failed / rate-limited state first: without this, a
    // unit that crash-looped earlier refuses every new start ("start of the
    // service was attempted too often") even after the cause is fixed.
    execute(fmt::format("systemctl reset-failed {} 2>/dev/null || true", t_svc));
    SGRN_INFO("DatastoreInit", "Restarting service {}...", t_svc);
    if (execute(fmt::format("systemctl restart {}", t_svc)) == 0)
        return true;
    SGRN_ERROR("DatastoreInit", "systemctl restart {} failed.", t_svc);
    // `reload` is never applicable to oneshot units (e.g. the sgrn.service
    // umbrella) — attempting it only adds a confusing second error.
    if (unitType(t_svc) == "oneshot") {
        SGRN_INFO("DatastoreInit", "{} is oneshot: skipping reload fallback.", t_svc);
    } else if (execute(fmt::format("systemctl reload {}", t_svc)) == 0) {
        SGRN_INFO("DatastoreInit", "{} reloaded as fallback.", t_svc);
        return true;
    }
    logUnitStatus(t_svc);
    return false;
}

bool restartChangedServices(const std::vector<std::string>& t_services) {
    if (t_services.empty()) {
        SGRN_INFO("DatastoreInit", "No changes detected. Systemd services are up to date.");
        return true;
    }
    SGRN_INFO("DatastoreInit", "Reloading systemd daemon...");
    if (execute("systemctl daemon-reload") != 0) {
        SGRN_ERROR("DatastoreInit", "systemctl daemon-reload failed; not restarting services against a stale systemd.");
        return false;
    }
    bool ok = true;
    for (const auto& svc : t_services) {
        ok = restartOneService(svc) && ok;
    }
    // Crash-loops report success to `restart` (the job queues while the
    // process dies seconds later under Restart=on-failure). When systemd is
    // actually running, verify liveness after a grace period instead of
    // trusting the restart return code. Skipped in non-systemd environments
    // (containers), where is-active can never succeed.
    if (execute("systemctl is-system-running --quiet 2>/dev/null") == 0) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        for (const auto& svc : t_services) {
            if (execute(fmt::format("systemctl is-active --quiet {}", svc)) != 0) {
                SGRN_ERROR("DatastoreInit", "Service {} is not active after restart — it may be crash-looping.", svc);
                logUnitStatus(svc);
                ok = false;
            }
        }
    } else {
        SGRN_INFO("DatastoreInit", "systemd is not running here; skipping post-restart liveness check.");
    }
    return ok;
}

bool enableServices(const std::vector<std::string>& t_services) {
    bool ok = true;
    for (const auto& svc : t_services) {
        if (execute(fmt::format("systemctl enable {} 2>/dev/null", svc)) != 0) {
            SGRN_WARN("DatastoreInit", "Could not enable {} for boot (continuing).", svc);
            ok = false;
        } else {
            SGRN_INFO("DatastoreInit", "  -> Enabled {} for boot.", svc);
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------
// PostgreSQL readiness wait
// ---------------------------------------------------------------------------

void waitForPostgres(const std::string& t_host, const std::string& t_port, const std::string& t_pg_isready) {
    if (t_pg_isready.empty() || !std::filesystem::exists(t_pg_isready)) {
        SGRN_WARN("DatastoreInit", "pg_isready not found; skipping postgres readiness wait.");
        return;
    }
    SGRN_INFO("DatastoreInit", "Waiting for PostgreSQL at {}:{} to accept connections...", t_host, t_port);
    for (int i = 0; i < 30; ++i) {
        if (execute(fmt::format("{} -h {} -p {} -t 2 2>/dev/null", t_pg_isready, t_host, t_port)) == 0) {
            SGRN_INFO("DatastoreInit", "PostgreSQL is accepting connections.");
            return;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    SGRN_WARN("DatastoreInit", "PostgreSQL did not become ready in ~60s; continuing anyway.");
}

} // namespace sgrn::datastore::bootstrap
