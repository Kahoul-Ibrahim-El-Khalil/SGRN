#pragma once
/**
 * @file  sgrn/datastore/bootstrap/db-init.hpp
 * @brief PostgreSQL database initialisation helpers for the SGRN bootstrap.
 *
 * Covers:
 *   - Estimating live row counts (destructive-rebuild guard)
 *   - Decompressing the embedded SQL schema asset
 *   - Dropping and recreating the target database
 *   - Applying the schema via an external `psql` process
 *
 * All functions stay inside namespace sgrn::datastore::bootstrap and are
 * called by the initDatabaseOnly orchestrator in bootstrap.hpp.
 * Definitions live in src/bootstrap/db-init.cpp.
 */

#include <drogon/orm/DbClient.h>
#include <filesystem>
#include <functional>
#include <string>

namespace sgrn::datastore::bootstrap
{

// ---------------------------------------------------------------------------
// Live-row estimate (destructive-rebuild guard)
// ---------------------------------------------------------------------------

/// Estimated live rows across user tables in t_conn's database — the
/// destructive-rebuild guard's evidence. Reads pg_stat_all_tables (stats
/// collector) plus a direct count over known core tables when stats lag;
/// -1 when the database exists but cannot be read (treated as "unknown"
/// by the caller: refuse non-interactively, confirm interactively).
long long estimateLiveRows(const std::string& t_conn);

// ---------------------------------------------------------------------------
// SQL asset decompression
// ---------------------------------------------------------------------------

std::string decompressSqlAssets(const std::function<std::string(std::string)>& t_tmpl);

// ---------------------------------------------------------------------------
// Database drop + recreate
// ---------------------------------------------------------------------------

void recreateDatabase(drogon::orm::DbClient* t_client, const std::string& t_db_name);

// ---------------------------------------------------------------------------
// Schema application via psql
// ---------------------------------------------------------------------------

void executeSchemaViaPsql(const std::string& t_pg_host, const std::string& t_pg_port, const std::string& t_pg_superuser,
    const std::filesystem::path& t_schema_path, const std::string& t_deployment_env);

} // namespace sgrn::datastore::bootstrap
