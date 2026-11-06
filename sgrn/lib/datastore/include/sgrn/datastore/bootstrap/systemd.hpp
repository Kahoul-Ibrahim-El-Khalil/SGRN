#pragma once
/**
 * @file  sgrn/datastore/bootstrap/systemd.hpp
 * @brief systemd service management helpers for the SGRN bootstrap.
 *
 * Covers:
 *   - Resolving the non-root sudo user
 *   - Stopping legacy / obsolete services
 *   - Ordering, enabling, restarting and verifying systemd units
 *   - Waiting for PostgreSQL readiness after a postgres unit restart
 *   - Parsing unit Environment= PATH lines and searching PATH directories
 *   - Logging unit status on failure
 *
 * All functions stay inside namespace sgrn::datastore::bootstrap and are
 * called by the configureSystemd / verifyInstall orchestrators in
 * bootstrap.hpp. Definitions live in src/bootstrap/systemd.cpp.
 */

#include <filesystem>
#include <string>
#include <sys/types.h>
#include <vector>

namespace sgrn::datastore::bootstrap
{

// ---------------------------------------------------------------------------
// Sudo-user resolution
// ---------------------------------------------------------------------------

std::string resolveSudoUser();

// ---------------------------------------------------------------------------
// Legacy / obsolete service cleanup
// ---------------------------------------------------------------------------

void stopLegacyServices(const std::string& t_sudo_user, uid_t t_user_uid);

// Units removed from the deployment. If a previous release installed them,
// --config-systemd disables and deletes them so they can neither start at
// boot nor linger as confusion. System-level paths need root (guaranteed by
// the configureSystemd() euid check); every command tolerates absence.
void removeObsoleteUnits(const std::filesystem::path& t_data_dir);

// ---------------------------------------------------------------------------
// Service ordering (dependency tier sort)
// ---------------------------------------------------------------------------

// Dedupe (the nginx sync pushes one entry per changed file) and order by
// dependency tier — infrastructure first, the sgrn.service umbrella last —
// so a restart run never bounces dependents before their dependencies.
std::vector<std::string> orderServicesForRestart(std::vector<std::string> t_services);

// ---------------------------------------------------------------------------
// Unit introspection helpers
// ---------------------------------------------------------------------------

// Best-effort query of a unit's Type (e.g. "oneshot", "simple").
// Empty string when systemctl is unavailable.
std::string unitType(const std::string& t_svc);

// Logs a `systemctl status` snapshot so a failed unit's cause is visible
// inline in the deploy log instead of requiring a follow-up journalctl run.
void logUnitStatus(const std::string& t_svc);

// ---------------------------------------------------------------------------
// Service restart / enable
// ---------------------------------------------------------------------------

bool restartOneService(const std::string& t_svc);

bool restartChangedServices(const std::vector<std::string>& t_services);

// Enables units for boot persistence. Best-effort: `enable` only writes
// symlinks (works without a running systemd), but report failures loudly.
bool enableServices(const std::vector<std::string>& t_services);

// ---------------------------------------------------------------------------
// PostgreSQL readiness wait
// ---------------------------------------------------------------------------

// Blocks until PostgreSQL accepts connections (or a ~60s timeout), so
// dependents restarted right after postgres don't boot against a cold
// server. Skipped with a warning when pg_isready is unavailable.
void waitForPostgres(const std::string& t_host, const std::string& t_port, const std::string& t_pg_isready);

} // namespace sgrn::datastore::bootstrap
