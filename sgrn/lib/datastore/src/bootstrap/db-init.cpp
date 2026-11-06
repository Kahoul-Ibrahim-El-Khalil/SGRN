// sgrn/lib/datastore/src/bootstrap/db-init.cpp
//
// PostgreSQL database initialisation helpers — definitions for
// include/sgrn/datastore/bootstrap/db-init.hpp.

#include <sgrn/datastore/bootstrap/db-init.hpp>

#include <fmt/core.h>
#include <sgrn/debug.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/process.hpp>
#include <algorithm>
#include <cstdlib>
#include <exception>
#include <sql_assets.hpp>
#include <trantor/net/EventLoopThread.h>

namespace sgrn::datastore::bootstrap
{

// ---------------------------------------------------------------------------
// Live-row estimate (destructive-rebuild guard)
// ---------------------------------------------------------------------------

long long estimateLiveRows(const std::string& t_conn) {
    trantor::EventLoopThread loop_thread;
    loop_thread.run();
    try {
        auto client = drogon::orm::DbClient::newPgClient(t_conn, 1, loop_thread.getLoop());
        auto result = client->execSqlSync("SELECT COALESCE(SUM(n_live_tup), 0)::bigint AS live FROM pg_stat_all_tables "
                                          "WHERE schemaname NOT IN ('pg_catalog', 'information_schema')");
        const long long stats_estimate = result.empty() ? -1 : result[0]["live"].as<long long>();
        try {
            auto core_total =
                client->execSqlSync("SELECT (SELECT count(*) FROM core.users) + (SELECT count(*) FROM core.organisations) AS core_rows");
            const long long core_rows = core_total[0]["core_rows"].as<long long>();
            return std::max(stats_estimate, core_rows);
        } catch (const std::exception&) {
            // No core schema yet (fresh cluster, DB created empty): the stats
            // estimate is the only evidence available.
            return stats_estimate;
        }
    } catch (const std::exception&) {
        return -1;
    }
}

// ---------------------------------------------------------------------------
// SQL asset decompression
// ---------------------------------------------------------------------------

std::string decompressSqlAssets(const std::function<std::string(std::string)>& t_tmpl) {
    if (sgrn::datastore::assets::sql::ASSET_COUNT == 0) {
        SGRN_ERROR("DatastoreInit", "No SQL assets found in binary!");
        std::exit(EXIT_FAILURE);
    }
    const auto& sql_asset = sgrn::datastore::assets::sql::ASSETS[0];
    auto dec_result = sgrn::utils::compression::decompressStringZstd(sql_asset.compressedView());
    if (dec_result.hasError()) {
        SGRN_ERROR("DatastoreInit", "Failed to decompress SQL assets: {}", dec_result.error());
        std::exit(EXIT_FAILURE);
    }
    return t_tmpl(dec_result.value());
}

// ---------------------------------------------------------------------------
// Database drop + recreate
// ---------------------------------------------------------------------------

void recreateDatabase(drogon::orm::DbClient* t_client, const std::string& t_db_name) {
    try {
        SGRN_INFO("DatastoreInit", "Dropping existing '{}' database if present...", t_db_name);
        t_client->execSqlSync(fmt::format("DROP DATABASE IF EXISTS {} WITH (FORCE);", t_db_name));
        SGRN_INFO("DatastoreInit", "Creating fresh '{}' database...", t_db_name);
        t_client->execSqlSync(fmt::format("CREATE DATABASE {} ENCODING 'UTF8';", t_db_name));
    } catch (const std::exception& e) {
        SGRN_ERROR("DatastoreInit", "Failed to recreate database: {}", e.what());
        std::exit(EXIT_FAILURE);
    }
}

// ---------------------------------------------------------------------------
// Schema application via psql
// ---------------------------------------------------------------------------

void executeSchemaViaPsql(const std::string& t_pg_host, const std::string& t_pg_port, const std::string& t_pg_superuser,
    const std::filesystem::path& t_schema_path, const std::string& t_deployment_env) {

    std::string pwd_prefix;
    if (const char* p_su_pass = std::getenv("POSTGRES_SUPERUSER_PASSWORD")) {
        pwd_prefix = fmt::format("PGPASSWORD='{}' ", p_su_pass);
    }

    std::string psql_cmd = fmt::format("{}{}psql -h {} -p {} -d postgres -U {} -f {} -q -v ON_ERROR_STOP=1", pwd_prefix,
        t_deployment_env.empty() ? "" : (t_deployment_env + "/bin/"), t_pg_host, t_pg_port, t_pg_superuser, t_schema_path.string());

    SGRN_INFO("DatastoreInit", "Executing flattened schema via psql...");
    int ret = sgrn::utils::process::execute(psql_cmd);
    if (ret != 0) {
        SGRN_ERROR("DatastoreInit", "Failed to execute schema (psql returned {})", ret);
        std::exit(EXIT_FAILURE);
    }
    SGRN_INFO("DatastoreInit", "Database schema applied.");
}

} // namespace sgrn::datastore::bootstrap
