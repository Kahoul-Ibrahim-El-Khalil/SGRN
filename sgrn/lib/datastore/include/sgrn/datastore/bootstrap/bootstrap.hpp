#pragma once
/**
 * @file  sgrn/datastore/bootstrap/bootstrap.hpp
 * @brief SGRN datastore bootstrap orchestration.
 *
 * This header is the single entry point for the bootstrap subsystem.
 * It pulls in the focused sub-headers and exposes the four top-level
 * commands that the CLI dispatcher (bootstrap/cli.hpp) calls:
 *
 *   generateConfigOnly()   – write config files + self-signed cert
 *   initDatabaseOnly()     – apply the SQL schema to PostgreSQL
 *   configureSystemd()     – deploy + restart system-level services (root)
 *   bootstrapDatabase()    – interactive --init: wizard + config + DB
 *   verifyInstall()        – health / drift report (--verify-install)
 *
 * The sub-headers below declare their helpers; the implementations live in
 * src/bootstrap/*.cpp (this file's orchestration is src/bootstrap/bootstrap.cpp).
 */

// ── Sub-system headers ────────────────────────────────────────────────────
#include "help.hpp"
#include <sgrn/datastore/bootstrap/bootstrap-helpers.hpp>
#include <sgrn/datastore/bootstrap/config-gen.hpp>
#include <sgrn/datastore/bootstrap/db-init.hpp>
#include <sgrn/datastore/bootstrap/secrets-rotate.hpp>
#include <sgrn/datastore/bootstrap/secrets-wizard.hpp>
#include <sgrn/datastore/bootstrap/secrets.hpp>
#include <sgrn/datastore/bootstrap/systemd.hpp>
#include <sgrn/datastore/bootstrap/verify.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace sgrn::datastore::bootstrap
{

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr const std::string_view kDefaultOperationDir = "~/.local/share/sgrn";

// ---------------------------------------------------------------------------
// Template rendering
// ---------------------------------------------------------------------------

/// Replace ${PLACEHOLDER} tokens in a template string with concrete values.
std::string replaceTemplateVars(std::string t_text, const std::string& t_pg_db, const std::string& t_pg_pass,
    const std::string& t_jwt_secret, const std::string& t_sgrn_user = {}, const std::string& t_sgrn_data = {},
    const std::string& t_sgrn_bin = {}, const std::string& t_sgrn_deployment_env = {}, const std::string& t_pg_user = {},
    const std::string& t_garage_access = {}, const std::string& t_garage_secret = {}, const std::string& t_garage_rpc = {},
    const std::string& t_garage_admin = {});

/// Shared helper: builds a template variable substitution lambda from .env parameters.
std::function<std::string(std::string)> buildTemplateResolver(const std::string& t_base_dir, const std::string& t_fallback_user = "");

// ---------------------------------------------------------------------------
// Orchestrating commands
// ---------------------------------------------------------------------------

bool generateConfigOnly(const std::string& t_base_dir);

void initDatabaseOnly(const std::string& t_base_dir, bool t_allow_insecure = false, bool t_force = false, bool t_non_interactive = false);

void configureSystemd();

void bootstrapDatabase(bool t_non_interactive = false, bool t_allow_insecure = false, bool t_force = false);

} // namespace sgrn::datastore::bootstrap
