// sgrn/lib/datastore/src/bootstrap/bootstrap.cpp
//
// SGRN datastore bootstrap orchestration — definitions for
// include/sgrn/datastore/bootstrap/bootstrap.hpp.

#include <sgrn/datastore/bootstrap/bootstrap.hpp>

#include <drogon/orm/DbClient.h>
#include <fmt/core.h>
#include <sgrn/debug.hpp>
#include <sgrn/utils/env.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <sgrn/utils/process.hpp>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <pwd.h>
#include <string>
#include <sys/types.h>
#include <trantor/net/EventLoopThread.h>
#include <unistd.h>

namespace sgrn::datastore::bootstrap
{

namespace
{

void chownDirToSudoUser(const std::string& t_dir) {
    if (::geteuid() != 0)
        return;
    std::string sudo_user = sgrn::utils::process::resolveSudoUser();
    if (sudo_user.empty() || sudo_user == "root")
        return;
    if (struct passwd* p_pw = ::getpwnam(sudo_user.c_str())) {
        [[maybe_unused]] int rc = std::system(fmt::format("chown -R '{}:{}' '{}' 2>/dev/null", p_pw->pw_uid, p_pw->pw_gid, t_dir).c_str());
    }
}

bool isLocalPgHost(const std::string& t_host) {
    return t_host.empty() || t_host == "127.0.0.1" || t_host == "localhost" || t_host == "::1" || t_host.starts_with("/");
}

int runPsqlAsPostgres(const std::string& t_db, const std::string& t_sql_cmd, const std::string& t_stdin_file = {}) {
    std::string cmd = t_stdin_file.empty()
                          ? fmt::format("su -s /bin/sh postgres -c \"psql -d {} -v ON_ERROR_STOP=1 -tAc \\\"{}\\\"\"", t_db, t_sql_cmd)
                          : fmt::format("su -s /bin/sh postgres -c \"psql -d {} -q -v ON_ERROR_STOP=1\" < '{}'", t_db, t_stdin_file);
    return sgrn::utils::process::execute(cmd);
}

std::string capturePsqlAsPostgres(const std::string& t_db, const std::string& t_sql_cmd) {
    std::string cmd = fmt::format("su -s /bin/sh postgres -c \"psql -d {} -tAc \\\"{}\\\"\" 2>/dev/null", t_db, t_sql_cmd);
    FILE* pipe = ::popen(cmd.c_str(), "r");
    if (!pipe)
        return {};
    char buf[256];
    std::string out;
    while (::fgets(buf, sizeof(buf), pipe) != nullptr)
        out += buf;
    ::pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Template rendering
// ---------------------------------------------------------------------------

std::string replaceTemplateVars(std::string t_text, const std::string& t_pg_db, const std::string& t_pg_pass,
    const std::string& t_jwt_secret, const std::string& t_sgrn_user, const std::string& t_sgrn_data, const std::string& t_sgrn_bin,
    const std::string& t_sgrn_deployment_env, const std::string& t_pg_user, const std::string& t_garage_access,
    const std::string& t_garage_secret, const std::string& t_garage_rpc, const std::string& t_garage_admin) {
    auto replace_all = [](std::string& t_s, const std::string& t_key, const std::string& t_val) {
        if (t_val.empty())
            return;
        size_t pos = 0;
        while ((pos = t_s.find(t_key, pos)) != std::string::npos) {
            t_s.replace(pos, t_key.length(), t_val);
            pos += t_val.length();
        }
    };
    replace_all(t_text, "${POSTGRES_DB}", t_pg_db);
    replace_all(t_text, "${POSTGRES_USER}", t_pg_user);
    replace_all(t_text, "${POSTGRES_PASSWORD}", t_pg_pass);
    replace_all(t_text, "${JWT_SECRET}", t_jwt_secret);
    replace_all(t_text, "${SGRN_USER}", t_sgrn_user);
    replace_all(t_text, "${SGRN_DATA_DIR}", t_sgrn_data);
    replace_all(t_text, "${SGRN_BIN_DIR}", t_sgrn_bin);
    replace_all(t_text, "${SGRN_DEPLOYMENT_ENV}", t_sgrn_deployment_env);
    replace_all(t_text, "${GARAGE_ACCESS_KEY}", t_garage_access);
    replace_all(t_text, "${GARAGE_SECRET_KEY}", t_garage_secret);
    replace_all(t_text, "${GARAGE_RPC_SECRET}", t_garage_rpc);
    replace_all(t_text, "${GARAGE_ADMIN_TOKEN}", t_garage_admin);
    return t_text;
}

std::function<std::string(std::string)> buildTemplateResolver(const std::string& t_base_dir, const std::string& t_fallback_user) {
    std::string sgrn_user = envOrDefault("USER", t_fallback_user.empty() ? "admin" : t_fallback_user);
    sgrn_user = envOrDefault("SGRN_USER", sgrn_user);
    std::string pg_db = envOrDefault("POSTGRES_DB", "sgrn");
    std::string pg_user = envOrDefault("POSTGRES_USER", "sgrn_datastore");
    std::string pg_pass = envOrDefault("POSTGRES_PASSWORD", "change_me_secure_db_password");
    std::string jwt_secret = envOrDefault("JWT_SECRET", "change_me_at_least_32_chars_jwt_secret");
    std::string sgrn_data = envOrDefault("SGRN_DATA_DIR", t_base_dir);
    std::string sgrn_bin = envOrDefault("SGRN_BIN_DIR", "/usr/local/bin");
    std::string sgrn_deployment_env = envOrDefault("SGRN_DEPLOYMENT_ENV", "$ENV{SGRN_DEPLOYMENT_ENV}");
    std::string garage_access = envOrDefault("GARAGE_ACCESS_KEY", "change_me_garage_access_key");
    std::string garage_secret = envOrDefault("GARAGE_SECRET_KEY", "change_me_garage_secret_key");
    std::string garage_rpc = envOrDefault("GARAGE_RPC_SECRET", "change_me_garage_rpc_secret_64_hex_chars");
    std::string garage_admin = envOrDefault("GARAGE_ADMIN_TOKEN", "change_me_garage_admin_token_64_hex_chars");

    return [=](std::string text) -> std::string {
        return replaceTemplateVars(std::move(text), pg_db, pg_pass, jwt_secret, sgrn_user, sgrn_data, sgrn_bin, sgrn_deployment_env,
            pg_user, garage_access, garage_secret, garage_rpc, garage_admin);
    };
}

// ---------------------------------------------------------------------------
// Orchestrating commands
// ---------------------------------------------------------------------------

bool generateConfigOnly(const std::string& t_base_dir) {
    SGRN_INFO("DatastoreInit", "Starting Configuration Generation...");

    namespace fs = std::filesystem;
    createDirectoryStructure(t_base_dir);

    fs::path env_path = fs::path(t_base_dir) / ".env";
    bool env_exists = fs::exists(env_path);
    std::string sgrn_user = sgrn::utils::process::resolveSudoUser();
    if (sgrn_user.empty())
        sgrn_user = envOrDefault("USER", "admin");

    if (!env_exists)
        generateDefaultEnvFile(env_path, sgrn_user, t_base_dir);

    if (auto env_result = loadEnvFile(env_path); env_result.hasError()) {
        SGRN_ERROR("DatastoreInit", "Failed to load .env: {}", env_result.error());
        std::exit(EXIT_FAILURE);
    }

    // Loud, non-fatal here (--generate-config must keep working as a template
    // step); --init-db holds the hard gate against placeholder secrets.
    if (envHasPlaceholderSecret()) {
        SGRN_WARN("DatastoreInit", "change_me_* placeholder secret(s) still active — rendered configs will carry them.\n"
                                   "Run --init (interactive wizard) or --rotate KEY=VALUE before deploying.");
    }

    {
        std::string recorded = envOrDefault("SGRN_BIN_DIR", "");
        std::string self_dir = currentExeDir();
        if (!hasDatastoreBinary(recorded) && !self_dir.empty() && hasDatastoreBinary(self_dir)) {
            SGRN_INFO("DatastoreInit", "SGRN_BIN_DIR '{}' no longer holds sgrn_datastore; updating it to '{}'.", recorded, self_dir);
            if (upsertEnvVar(env_path, "SGRN_BIN_DIR", self_dir)) {
                ::setenv("SGRN_BIN_DIR", self_dir.c_str(), 1);
            } else {
                SGRN_WARN("DatastoreInit", "Could not rewrite {}; deployed units may reference a stale binary path.", env_path.string());
            }
        }
    }

    auto tmpl = buildTemplateResolver(t_base_dir);
    generateSelfSignedCert(fs::path(t_base_dir) / "certs");
    extractConfigAssets(t_base_dir, tmpl);
    generateSystemdServices(t_base_dir, tmpl);
    chownDirToSudoUser(t_base_dir);

    return env_exists;
}

void initDatabaseOnly(const std::string& t_base_dir, bool t_allow_insecure, bool t_force, bool t_non_interactive) {
    SGRN_INFO("DatastoreInit", "Starting Database Initialization...");

    namespace fs = std::filesystem;
    fs::path env_path = fs::path(t_base_dir) / ".env";
    if (!fs::exists(env_path)) {
        SGRN_ERROR("DatastoreInit", "Configuration file .env not found. Please run --generate-config first.");
        std::exit(EXIT_FAILURE);
    }

    if (auto env_result = loadEnvFile(env_path); env_result.hasError()) {
        SGRN_ERROR("DatastoreInit", "Failed to load .env: {}", env_result.error());
        std::exit(EXIT_FAILURE);
    }

    resolveEnvPlaceholderKey(env_path, "SGRN_DEPLOYMENT_ENV");

    if (!t_allow_insecure && envHasPlaceholderSecret()) {
        SGRN_ERROR("DatastoreInit", "--init-db refuses to run with change_me_* placeholder secrets in .env.");
        SGRN_ERROR("DatastoreInit", "  a) re-run --init — the wizard generates them (or fixes existing .env)");
        SGRN_ERROR("DatastoreInit", "  b) rotate in place:  sgrn_datastore --rotate POSTGRES_PASSWORD --rotate JWT_SECRET");
        SGRN_ERROR("DatastoreInit", "  c) edit .env by hand, then re-run --init-db (--allow-insecure skips this gate)");
        std::exit(EXIT_FAILURE);
    }

    std::string pg_host = envOrDefault("POSTGRES_HOST", "127.0.0.1");
    std::string pg_port = envOrDefault("POSTGRES_PORT", "5432");
    std::string pg_db = envOrDefault("POSTGRES_DB", "sgrn");
    std::string pg_superuser = envOrDefault("POSTGRES_SUPERUSER", "postgres");
    std::string sgrn_user = envOrDefault("SGRN_USER", envOrDefault("USER", "admin"));
    std::string sgrn_deployment_env = envOrDefault("SGRN_DEPLOYMENT_ENV", "$ENV{SGRN_DEPLOYMENT_ENV}");

    auto tmpl = buildTemplateResolver(t_base_dir);
    std::string sql = decompressSqlAssets(tmpl);

    const char* p_su_pass = std::getenv("POSTGRES_SUPERUSER_PASSWORD");
    const bool use_root_peer = (::geteuid() == 0) && (p_su_pass == nullptr || *p_su_pass == '\0') && isLocalPgHost(pg_host);

    std::string bootstrap_conn = fmt::format("host={} port={} dbname=postgres user={}", pg_host, pg_port, pg_superuser);
    if (p_su_pass != nullptr && *p_su_pass != '\0')
        bootstrap_conn += fmt::format(" password={}", p_su_pass);

    trantor::EventLoopThread loop_thread;
    std::shared_ptr<drogon::orm::DbClient> client;
    if (use_root_peer) {
        SGRN_INFO("DatastoreInit", "Bootstrapping via system postgres user (Unix socket peer auth)");
    } else {
        loop_thread.run();
        SGRN_INFO("DatastoreInit", "Bootstrapping via connection: host={} port={} dbname=postgres user={}", pg_host, pg_port, pg_superuser);
        client = drogon::orm::DbClient::newPgClient(bootstrap_conn, 1, loop_thread.getLoop());
    }

    SGRN_INFO("DatastoreInit", "Init target: {}:{}/{} (drop + recreate)", pg_host, pg_port, pg_db);

    bool db_exists = false;
    if (use_root_peer) {
        db_exists =
            capturePsqlAsPostgres("postgres", fmt::format("SELECT 1 FROM pg_database WHERE datname = '{}'", sqlEscape(pg_db))) == "1";
    } else {
        try {
            auto exists = client->execSqlSync(fmt::format("SELECT 1 AS e FROM pg_database WHERE datname = '{}'", sqlEscape(pg_db)));
            db_exists = !exists.empty();
        } catch (const std::exception& e) {
            SGRN_WARN("DatastoreInit", "Could not list databases ({}): treating target as absent.", e.what());
        }
    }

    if (db_exists) {
        long long rows = -1;
        if (use_root_peer) {
            std::string stats_s = capturePsqlAsPostgres(pg_db, "SELECT COALESCE(SUM(n_live_tup), 0)::bigint FROM pg_stat_all_tables "
                                                               "WHERE schemaname NOT IN ('pg_catalog', 'information_schema')");
            if (!stats_s.empty()) {
                try {
                    rows = std::stoll(stats_s);
                } catch (...) {
                    rows = -1;
                }
            }
            std::string core_s =
                capturePsqlAsPostgres(pg_db, "SELECT (SELECT count(*) FROM core.users) + (SELECT count(*) FROM core.organisations)");
            if (!core_s.empty()) {
                try {
                    rows = std::max(rows, std::stoll(core_s));
                } catch (...) {
                }
            }
        } else {
            const std::string target_conn = fmt::format("host={} port={} dbname={} user={}", pg_host, pg_port, pg_db, pg_superuser);
            rows = estimateLiveRows(target_conn);
        }
        if (rows == 0) {
            SGRN_INFO("DatastoreInit", "Target is empty (no user rows) — safe to rebuild.");
        } else if (rows > 0) {
            SGRN_WARN("DatastoreInit", "Target database holds ~{} row(s) — a rebuild DROPS them permanently.", rows);
        } else {
            SGRN_WARN("DatastoreInit", "Target database could not be inspected — row count unknown.");
        }
        if ((rows > 0 || rows < 0) && !t_force) {
            if (t_non_interactive) {
                SGRN_ERROR("DatastoreInit", "Refusing destructive rebuild of {}:{}/{} in non-interactive mode.", pg_host, pg_port, pg_db);
                SGRN_ERROR("DatastoreInit", "Target is non-empty or uninspectable; pass --force to confirm the drop.");
                std::exit(EXIT_FAILURE);
            }
            const std::string answer =
                promptLine(fmt::format("Rebuild {}:{}/{} and DROP its data permanently? [y/N]: ", pg_host, pg_port, pg_db));
            if (!(answer == "y" || answer == "Y" || answer == "yes")) {
                SGRN_ERROR("DatastoreInit", "Aborted — no changes were made. (--force skips this confirmation)");
                std::exit(EXIT_FAILURE);
            }
            SGRN_INFO("DatastoreInit", "Confirmed by operator — rebuilding.");
        }
    }

    if (use_root_peer) {
        SGRN_INFO("DatastoreInit", "Dropping existing '{}' database if present...", pg_db);
        if (runPsqlAsPostgres("postgres", fmt::format("DROP DATABASE IF EXISTS {} WITH (FORCE);", pg_db)) != 0) {
            SGRN_ERROR("DatastoreInit", "Failed to drop existing '{}' database via postgres user.", pg_db);
            std::exit(EXIT_FAILURE);
        }
        SGRN_INFO("DatastoreInit", "Creating fresh '{}' database...", pg_db);
        if (runPsqlAsPostgres("postgres", fmt::format("CREATE DATABASE {} ENCODING 'UTF8';", pg_db)) != 0) {
            SGRN_ERROR("DatastoreInit", "Failed to create '{}' database via postgres user.", pg_db);
            std::exit(EXIT_FAILURE);
        }
    } else {
        recreateDatabase(client.get(), pg_db);
    }

    fs::path schema_path = fs::path(t_base_dir) / "schema.sql";
    std::string admin_fragment;
    bool warn_seed_password = false;
    {
        const fs::path admin_sql = wizard::adminBootstrapPath(t_base_dir);
        if (fs::exists(admin_sql))
            admin_fragment = sgrn::utils::filesystem::readTextFile(admin_sql);
        warn_seed_password = admin_fragment.empty();
        std::ofstream sql_out(schema_path, std::ios::binary);
        sql_out << sql;
        if (!admin_fragment.empty())
            sql_out << "\n-- -- admin account bootstrap (staged by --init wizard) --\n" << admin_fragment << "\n";
    }

    if (use_root_peer) {
        SGRN_INFO("DatastoreInit", "Executing flattened schema via psql (postgres peer auth)...");
        if (runPsqlAsPostgres("postgres", {}, schema_path.string()) != 0) {
            SGRN_ERROR("DatastoreInit", "Failed to execute schema via postgres user.");
            std::exit(EXIT_FAILURE);
        }
        SGRN_INFO("DatastoreInit", "Database schema applied.");
    } else {
        executeSchemaViaPsql(pg_host, pg_port, pg_superuser, schema_path, sgrn_deployment_env);
    }

    if (!admin_fragment.empty()) {
        std::error_code rm_ec;
        fs::remove(wizard::adminBootstrapPath(t_base_dir), rm_ec);
        if (upsertEnvVar(env_path, "SGRN_ADMIN_BOOTSTRAP", "1"))
            ::setenv("SGRN_ADMIN_BOOTSTRAP", "1", 1);
        SGRN_INFO("DatastoreInit", "Admin account password applied (bootstrap consumed and removed).");
    } else if (warn_seed_password && !t_allow_insecure) {
        SGRN_WARN("DatastoreInit", "The seeded admin account keeps its default password (postgres/seeding.sql). "
                                   "Change it immediately from the dashboard, or re-run --init to let the wizard master one.");
    }
    chownDirToSudoUser(t_base_dir);
}

void configureSystemd() {
    if (::geteuid() != 0) {
        SGRN_ERROR("DatastoreInit", "Error: This command must be run with root privileges (e.g., using sudo).");
        std::exit(EXIT_FAILURE);
    }

    std::string sudo_user = resolveSudoUser();

    struct passwd* p_pw = ::getpwnam(sudo_user.c_str());
    if (!p_pw) {
        SGRN_ERROR("DatastoreInit", "Error: Could not retrieve user info for user '{}'", sudo_user);
        std::exit(EXIT_FAILURE);
    }

    std::string user_home = p_pw->pw_dir;
    uid_t user_uid = p_pw->pw_uid;
    gid_t user_gid = p_pw->pw_gid;

    namespace fs = std::filesystem;
    fs::path base_dir = fs::path(user_home) / ".local" / "share" / "sgrn";
    fs::path env_path = base_dir / ".env";
    if (!fs::exists(env_path)) {
        SGRN_ERROR("DatastoreInit", "Error: Environment configuration file not found at {}. Please run --generate-config first.",
            env_path.string());
        std::exit(EXIT_FAILURE);
    }

    if (auto env_result = loadEnvFile(env_path); env_result.hasError()) {
        SGRN_ERROR("DatastoreInit", "Failed to load .env: {}", env_result.error());
        std::exit(EXIT_FAILURE);
    }

    std::string sgrn_data = envOrDefault("SGRN_DATA_DIR", base_dir.string());
    std::string sgrn_deployment_env = envOrDefault("SGRN_DEPLOYMENT_ENV", "$ENV{SGRN_DEPLOYMENT_ENV}");

    // Preflight: refuse to deploy units that point to a missing binary.
    {
        std::string bin = (fs::path(envOrDefault("SGRN_BIN_DIR", "/usr/local/bin")) / "sgrn_datastore").string();
        if (!hasDatastoreBinary(envOrDefault("SGRN_BIN_DIR", "/usr/local/bin"))) {
            SGRN_ERROR("DatastoreInit", "Error: no sgrn_datastore binary at {}.", bin);
            SGRN_ERROR("DatastoreInit", "Rebuild/reinstall the binary, then re-run --generate-config to refresh SGRN_BIN_DIR.");
            std::exit(EXIT_FAILURE);
        }
    }

    stopLegacyServices(sudo_user, user_uid);

    fs::path systemd_src = fs::path(sgrn_data) / "systemd";
    fs::path nginx_src = fs::path(sgrn_data) / "nginx";
    fs::path systemd_dst = "/etc/systemd/system";

    bool has_units = false;
    if (fs::exists(systemd_src)) {
        for (const auto& entry : fs::directory_iterator(systemd_src)) {
            if (entry.is_regular_file() && entry.path().extension() == ".service") {
                has_units = true;
                break;
            }
        }
    }
    if (!has_units) {
        SGRN_ERROR("DatastoreInit", "Error: no systemd units staged under {}. Run --generate-config first.", systemd_src.string());
        std::exit(EXIT_FAILURE);
    }

    auto tmpl = buildTemplateResolver(sgrn_data, sudo_user);
    generateSystemdServices(sgrn_data, tmpl);
    removeObsoleteUnits(sgrn_data);

    std::vector<std::string> changed_services;
    bool copy_failed = false;

    if (fs::exists(systemd_src)) {
        for (const auto& entry : fs::directory_iterator(systemd_src)) {
            if (entry.is_regular_file() && entry.path().extension() == ".service") {
                if (entry.path().filename() == "nginx.service" && fs::file_size(entry.path()) == 0)
                    continue;
                fs::path dest = systemd_dst / entry.path().filename();
                // Use content-aware copy (not size-only) to catch template substitution changes.
                if (!sgrn::utils::filesystem::fileContentsEqual(entry.path(), dest)) {
                    SGRN_INFO("DatastoreInit", "  Copying {} to {}...", entry.path().filename().string(), dest.string());
                    std::error_code ec;
                    fs::create_directories(dest.parent_path(), ec);
                    fs::copy_file(entry.path(), dest, fs::copy_options::overwrite_existing, ec);
                    if (ec) {
                        SGRN_ERROR("DatastoreInit", "Failed to copy {} to {}: {}", entry.path().string(), dest.string(), ec.message());
                        copy_failed = true;
                    } else {
                        changed_services.push_back(dest.filename().string());
                    }
                }
            }
        }
    }

    if (!fs::exists(nginx_src / "nginx.conf")) {
        SGRN_ERROR("DatastoreInit", "Error: no staged nginx config at {}.", (nginx_src / "nginx.conf").string());
        SGRN_ERROR("DatastoreInit", "Run 'sgrn_datastore --generate-config' as user, then re-run this command.");
        std::exit(EXIT_FAILURE);
    }

    {
        bool any_created = false;
        for (const char* kRelDir : {"var/log/nginx", "var/run", "var/tmp/nginx/client", "var/tmp/nginx/proxy", "var/tmp/nginx/fastcgi",
                 "var/tmp/nginx/uwsgi", "var/tmp/nginx/scgi"}) {
            if (fs::create_directories(fs::path(sgrn_data) / kRelDir))
                any_created = true;
        }
        if (any_created) {
            if (std::system(fmt::format("chown -R '{}:{}' '{}/var' 2>/dev/null", user_uid, user_gid, sgrn_data).c_str()) != 0) {
                SGRN_WARN("DatastoreInit", "Failed to chown {}/var to {}:{}", sgrn_data, user_uid, user_gid);
            }
            SGRN_WARN("DatastoreInit", "Created missing nginx runtime dirs under {}/var (root-owned entries chowned).", sgrn_data);
        }
    }

    if (copy_failed) {
        SGRN_ERROR("DatastoreInit", "Error: one or more service files could not be deployed. Nothing was restarted.");
        std::exit(EXIT_FAILURE);
    }

    changed_services = orderServicesForRestart(std::move(changed_services));
    enableServices(changed_services);

    if (!restartChangedServices(changed_services))
        std::exit(EXIT_FAILURE);

    // Verify PostgreSQL is accepting connections when SGRN-datastore.service was
    // restarted (system postgresql.service is expected to be running).
    // Use system pg_isready directly; fall back to SGRN_DEPLOYMENT_ENV/bin for
    // custom installs where SGRN_DEPLOYMENT_ENV != /usr.
    if (std::find(changed_services.begin(), changed_services.end(), "SGRN-datastore.service") != changed_services.end()) {
        auto pg_isready_path = [&]() -> std::string {
            for (const char* p : {"/usr/bin/pg_isready", "/usr/lib/postgresql/17/bin/pg_isready"}) {
                if (std::filesystem::exists(p))
                    return p;
            }
            std::string candidate = (fs::path(sgrn_deployment_env) / "bin" / "pg_isready").string();
            if (std::filesystem::exists(candidate))
                return candidate;
            return {};
        }();
        waitForPostgres(envOrDefault("POSTGRES_HOST", "127.0.0.1"), envOrDefault("POSTGRES_PORT", "5432"), pg_isready_path);
    }
}

void bootstrapDatabase(bool t_non_interactive, bool t_allow_insecure, bool t_force) {
    // Path resolved by cli.cpp's base_dir() lambda (SUDO_USER-aware) and
    // forwarded here via bootstrapDatabase(base_dir, ...). This zero-arg overload
    // is kept for other internal call-sites that don't go through the CLI.
    const std::string base_dir = sgrn::utils::filesystem::expandUserPath(std::string(kDefaultOperationDir));

    wizard::runSecretWizard(base_dir, t_non_interactive);

    bool env_exists = generateConfigOnly(base_dir);
    if (!env_exists)
        return; // stop and let user configure secrets

    initDatabaseOnly(base_dir, t_allow_insecure, t_force, t_non_interactive);

    SGRN_INFO("DatastoreInit", "");
    SGRN_INFO("DatastoreInit", "╔══════════════════════════════════════════════╗");
    SGRN_INFO("DatastoreInit", "║     SGRN Platform Ready!                     ║");
    SGRN_INFO("DatastoreInit", "╚══════════════════════════════════════════════╝");
    SGRN_INFO("DatastoreInit", "To install and activate system-wide systemd services:");
    SGRN_INFO("DatastoreInit", "  sudo <bin> --config-systemd");
    SGRN_INFO("DatastoreInit", "");
}

void bootstrapDatabase(const std::string& t_base_dir, bool t_non_interactive, bool t_allow_insecure, bool t_force) {
    wizard::runSecretWizard(t_base_dir, t_non_interactive);

    bool env_exists = generateConfigOnly(t_base_dir);
    if (!env_exists)
        return; // stop and let user configure secrets

    initDatabaseOnly(t_base_dir, t_allow_insecure, t_force, t_non_interactive);

    SGRN_INFO("DatastoreInit", "");
    SGRN_INFO("DatastoreInit", "╔══════════════════════════════════════════════╗");
    SGRN_INFO("DatastoreInit", "║     SGRN Platform Ready!                     ║");
    SGRN_INFO("DatastoreInit", "╚══════════════════════════════════════════════╝");
    SGRN_INFO("DatastoreInit", "To install and activate system-wide systemd services:");
    SGRN_INFO("DatastoreInit", "  sudo <bin> --config-systemd");
    SGRN_INFO("DatastoreInit", "");
}

} // namespace sgrn::datastore::bootstrap
