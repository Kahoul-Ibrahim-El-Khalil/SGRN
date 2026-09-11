#pragma once
#include <drogon/orm/DbClient.h>
#include <fmt/core.h>
#include <sgrn/assets/EmbeddedAsset.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/app.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/env.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <config_assets.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <pwd.h>
#include <sql_assets.hpp>
#include <sys/types.h>
#include <trantor/net/EventLoopThread.h>
#include <unistd.h>
#include <vector>

#include "help.hpp"
#include <string_view>

namespace sgrn::datastore::bootstrap
{
constexpr const std::string_view kDefaultOperationDir = "~/.local/share/sgrn";

// ---------------------------------------------------------------------------
// Shared Helpers
// ---------------------------------------------------------------------------

/// Parse a .env file and setenv() each KEY=VALUE pair.
/// Delegates to sgrn::utils::env::loadFile().
/// Returns a Result to propagate file-open errors to the caller.
inline sgrn::Result<void, std::string> loadEnvFile(const std::filesystem::path& t_env_path) {
    return sgrn::utils::env::loadFile(t_env_path);
}

/// Read an env var or return a default if unset.
/// Delegates to sgrn::utils::env::get().
inline std::string envOrDefault(const char* t_key, const std::string& t_default) {
    return sgrn::utils::env::get(t_key, t_default);
}

inline std::string replaceTemplateVars(std::string t_text, const std::string& t_pg_db, const std::string& t_pg_pass,
    const std::string& t_jwt_secret, const std::string& t_sgrn_user = {}, const std::string& t_sgrn_data = {},
    const std::string& t_sgrn_bin = {}, const std::string& t_sgrn_deployment_env = {}, const std::string& t_minio_user = {},
    const std::string& t_minio_pass = {}, const std::string& t_pg_user = {}) {
    auto replace_all = [](std::string& t_s, const std::string& t_ey, const std::string& t_val) {
        if (t_val.empty())
            return;
        size_t pos = 0;
        while ((pos = t_s.find(t_ey, pos)) != std::string::npos) {
            t_s.replace(pos, t_ey.length(), t_val);
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
    replace_all(t_text, "${MINIO_ROOT_USER}", t_minio_user);
    replace_all(t_text, "${MINIO_ROOT_PASSWORD}", t_minio_pass);
    return t_text;
}

// ---------------------------------------------------------------------------
// generateConfigOnly Helpers
// ---------------------------------------------------------------------------

inline void createDirectoryStructure(const std::string& t_base_dir) {
    namespace fs = std::filesystem;
    fs::create_directories(t_base_dir);
    fs::create_directories(fs::path(t_base_dir) / "Postgres" / "data");
    fs::create_directories(fs::path(t_base_dir) / "minio");
    fs::create_directories(fs::path(t_base_dir) / "var" / "log" / "nginx");
    fs::create_directories(fs::path(t_base_dir) / "var" / "run");
}

inline void generateDefaultEnvFile(const std::filesystem::path& t_env_path, const std::string& t_user, const std::string& t_base_dir) {
    namespace fs = std::filesystem;

    // Resolve runtime paths so the .env is portable
    char exe_buf[4096] = {};
    ssize_t exe_len = ::readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    std::string bin_dir = (exe_len > 0) ? fs::path(exe_buf).parent_path().string() : "/usr/local/bin";
    std::string conda_prefix = envOrDefault("CONDA_PREFIX", "/home/" + t_user + "/micromamba/envs/SGRN");

    std::ofstream env_out(t_env_path);
    env_out << "# ==========================================\n";
    env_out << "# SGRN Platform — Environment Configuration\n";
    env_out << "# Edit secrets, then re-run with --init-db or --init\n";
    env_out << "# ==========================================\n\n";

    env_out << "# ── Deployment paths ──────────────────────\n";
    env_out << "SGRN_USER=" << t_user << "\n";
    env_out << "SGRN_DATA_DIR=" << t_base_dir << "\n";
    env_out << "SGRN_BIN_DIR=" << bin_dir << "\n";
    env_out << "SGRN_DEPLOYMENT_ENV=" << conda_prefix << "\n\n";

    env_out << "# ── PostgreSQL ─────────────────────────────\n";
    env_out << "POSTGRES_HOST=127.0.0.1\n";
    env_out << "POSTGRES_PORT=5432\n";
    env_out << "POSTGRES_DB=sgrn\n";
    env_out << "# Superuser for bootstrapping (peer auth — must match your Linux username)\n";
    env_out << "POSTGRES_SUPERUSER=" << t_user << "\n";
    env_out << "# App-level role created by the schema (used by datastore + postgrest)\n";
    env_out << "POSTGRES_USER=sgrn_datastore\n";
    env_out << "POSTGRES_PASSWORD=change_me_secure_db_password\n\n";

    env_out << "# ── JWT (must be ≥ 32 chars for HS256) ────\n";
    env_out << "JWT_SECRET=change_me_at_least_32_chars_jwt_secret\n\n";

    env_out << "# ── MinIO Object Storage ───────────────────\n";
    env_out << "MINIO_ROOT_USER=minioadmin\n";
    env_out << "MINIO_ROOT_PASSWORD=change_me_minio_password\n";

    SGRN_INFO("DatastoreInit", "");
    SGRN_INFO("DatastoreInit", "╔══════════════════════════════════════════════╗");
    SGRN_INFO("DatastoreInit", "║  ACTION REQUIRED: edit your secrets first!   ║");
    SGRN_INFO("DatastoreInit", "╚══════════════════════════════════════════════╝");
    SGRN_INFO("DatastoreInit", "Generated default .env → {}", t_env_path.string());
    SGRN_INFO("DatastoreInit", "Fill in the secrets, then re-run with --init-db or --init to apply them.");
    SGRN_INFO("DatastoreInit", "");
}

inline void generateSelfSignedCert(const std::filesystem::path& t_cert_dir) {
    namespace fs = std::filesystem;
    fs::create_directories(t_cert_dir);
    fs::path cert_path = t_cert_dir / "server.crt";
    fs::path key_path = t_cert_dir / "server.key";
    if (fs::exists(cert_path) && fs::exists(key_path))
        return;

    SGRN_INFO("DatastoreInit", "SSL cert/key not found. Generating self-signed SSL certificates...");
    std::string cmd =
        fmt::format("openssl req -x509 -nodes -days 365 -newkey rsa:2048 -keyout {} -out {} -subj \"/CN=localhost\" 2>/dev/null",
            key_path.string(), cert_path.string());
    int ret = std::system(cmd.c_str());
    if (ret == 0) {
        SGRN_INFO("DatastoreInit", "  -> Generated: {} and {}", cert_path.string(), key_path.string());
    } else {
        SGRN_WARN("DatastoreInit", "Failed to generate self-signed SSL certificate automatically (openssl returned {})", ret);
    }
}

inline void extractConfigAssets(const std::string& t_base_dir, const std::function<std::string(std::string)>& t_tmpl) {
    namespace fs = std::filesystem;
    SGRN_INFO("DatastoreInit", "Extracting {} configuration files to {}", sgrn::datastore::assets::config::ASSET_COUNT, t_base_dir);
    for (size_t i = 0; i < sgrn::datastore::assets::config::ASSET_COUNT; ++i) {
        const auto& asset = sgrn::datastore::assets::config::ASSETS[i];
        std::string_view vpath(asset.virtual_path);
        if (vpath.starts_with("/"))
            vpath.remove_prefix(1);

        fs::path out_path = fs::path(t_base_dir) / vpath;
        auto dec_result = sgrn::utils::compression::decompressStringZstd(asset.compressedView());
        if (!dec_result.hasError()) {
            fs::create_directories(out_path.parent_path());
            std::ofstream out(out_path, std::ios::binary);
            out << t_tmpl(dec_result.value());
            SGRN_INFO("DatastoreInit", "  -> Wrote: {}", out_path.string());
        } else {
            SGRN_ERROR("DatastoreInit", "  -> Failed to decompress: {}", out_path.string());
        }
    }
}

inline void generateSystemdServices(const std::string& t_base_dir, const std::function<std::string(std::string)>& t_tmpl) {
    namespace fs = std::filesystem;
    SGRN_INFO("DatastoreInit", "Generating systemd service files...");
    fs::path systemd_out_dir = fs::path(t_base_dir) / "systemd";
    fs::create_directories(systemd_out_dir);

    static constexpr std::string_view kSystemdServices[] = {"SGRN-datastore.service", "SGRN-postgrest.service", "SGRN-minio.service",
        "SGRN-postgres.service", "SGRN-redis.service", "SGRN-nginx.service", "sgrn.service"};

    for (auto svc : kSystemdServices) {
        std::string vp = std::string("/systemd/") + std::string(svc);
        bool found = false;
        for (size_t i = 0; i < sgrn::datastore::assets::config::ASSET_COUNT; ++i) {
            if (sgrn::datastore::assets::config::ASSETS[i].virtual_path == vp) {
                auto& asset = sgrn::datastore::assets::config::ASSETS[i];
                auto dec = sgrn::utils::compression::decompressStringZstd(asset.compressedView());
                if (!dec.hasError()) {
                    fs::path out_svc = systemd_out_dir / svc;
                    std::ofstream f(out_svc);
                    f << t_tmpl(dec.value());
                    SGRN_INFO("DatastoreInit", "  -> Generated: {}", out_svc.string());
                    found = true;
                }
                break;
            }
        }
        if (!found)
            SGRN_WARN("DatastoreInit", "  -> Service asset not found: {}", vp);
    }
}

// ---------------------------------------------------------------------------
// initDatabaseOnly Helpers
// ---------------------------------------------------------------------------

inline std::string decompressSqlAssets(const std::function<std::string(std::string)>& t_tmpl) {
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

inline void recreateDatabase(drogon::orm::DbClient* t_client, const std::string& t_db_name) {
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

inline void executeSchemaViaPsql(const std::string& t_pg_host, const std::string& t_pg_port, const std::string& t_pg_superuser,
    const std::filesystem::path& t_schema_path, const std::string& t_deployment_env) {

    std::string pwd_prefix;
    if (const char* p_su_pass = std::getenv("POSTGRES_SUPERUSER_PASSWORD")) {
        pwd_prefix = fmt::format("PGPASSWORD='{}' ", p_su_pass);
    }

    std::string psql_cmd = fmt::format("{}{}psql -h {} -p {} -d postgres -U {} -f {} -q -v ON_ERROR_STOP=1", pwd_prefix,
        t_deployment_env.empty() ? "" : (t_deployment_env + "/bin/"), t_pg_host, t_pg_port, t_pg_superuser, t_schema_path.string());

    SGRN_INFO("DatastoreInit", "Executing flattened schema via psql...");
    int ret = std::system(psql_cmd.c_str());
    if (ret != 0) {
        SGRN_ERROR("DatastoreInit", "Failed to execute schema (psql returned {})", ret);
        std::exit(EXIT_FAILURE);
    }
    SGRN_INFO("DatastoreInit", "Database schema applied.");
}

// ---------------------------------------------------------------------------
// configureSystemd Helpers
// ---------------------------------------------------------------------------

inline std::string resolveSudoUser() {
    const char* p_sudo_user_env = std::getenv("SUDO_USER");
    std::string sudo_user = p_sudo_user_env ? p_sudo_user_env : "";
    if (sudo_user.empty()) {
        uid_t uid = ::getuid();
        if (struct passwd* p_pw = ::getpwuid(uid)) {
            sudo_user = p_pw->pw_name;
        }
    }
    if (sudo_user.empty() || sudo_user == "root") {
        SGRN_ERROR("DatastoreInit", "Error: Could not resolve the non-root sudo user.");
        std::exit(EXIT_FAILURE);
    }
    return sudo_user;
}

inline void stopLegacyServices(const std::string& t_sudo_user, uid_t t_user_uid) {
    SGRN_INFO("DatastoreInit", "Cleaning up any legacy user-level services...");
    std::string cmd = fmt::format("sudo -u {} XDG_RUNTIME_DIR=/run/user/{} systemctl --user stop "
                                  "SGRN-nginx.service sgrn.service SGRN-datastore.service "
                                  "SGRN-minio.service SGRN-postgres.service SGRN-postgrest.service "
                                  "SGRN-redis.service 2>/dev/null || true",
        t_sudo_user, t_user_uid);
    std::system(cmd.c_str());
}

inline sgrn::Result<void, std::string> copyIfChanged(
    const std::filesystem::path& t_src, const std::filesystem::path& t_dst, bool t_is_nginx, uid_t t_uid, gid_t t_gid) {
    namespace fs = std::filesystem;
    if (!fs::exists(t_src)) {
        return sgrn::Result<void, std::string>::Error(fmt::format("Source file does not exist: {}", t_src.string()));
    }
    bool copy_needed = true;
    if (fs::exists(t_dst)) {
        copy_needed = (fs::file_size(t_src) != fs::file_size(t_dst));
    }
    if (copy_needed) {
        SGRN_INFO("DatastoreInit", "  Copying {} to {}...", t_src.filename().string(), t_dst.string());
        try {
            fs::create_directories(t_dst.parent_path());
            fs::copy_file(t_src, t_dst, fs::copy_options::overwrite_existing);
            if (t_is_nginx) {
                ::chown(t_dst.c_str(), t_uid, t_gid);
            }
        } catch (const std::exception& e) {
            return sgrn::Result<void, std::string>::Error(
                fmt::format("Failed to copy {} to {}: {}", t_src.string(), t_dst.string(), e.what()));
        }
    }
    return {};
}

inline void restartChangedServices(const std::vector<std::string>& t_services) {
    if (t_services.empty()) {
        SGRN_INFO("DatastoreInit", "No changes detected. Systemd services are up to date.");
        return;
    }
    SGRN_INFO("DatastoreInit", "Reloading systemd daemon...");
    std::system("systemctl daemon-reload");
    for (const auto& svc : t_services) {
        SGRN_INFO("DatastoreInit", "Restarting service {}...", svc);
        std::string cmd = fmt::format("systemctl restart {} || systemctl reload {}", svc, svc);
        std::system(cmd.c_str());
    }
}

// ---------------------------------------------------------------------------
// Main Functions (refactored — each now delegates to focused helpers)
// ---------------------------------------------------------------------------

inline bool generateConfigOnly(const std::string& t_base_dir) {
    SGRN_INFO("DatastoreInit", "Starting Configuration Generation...");

    namespace fs = std::filesystem;
    createDirectoryStructure(t_base_dir);

    fs::path env_path = fs::path(t_base_dir) / ".env";
    bool env_exists = fs::exists(env_path);
    std::string sgrn_user = envOrDefault("USER", "admin");

    if (!env_exists) {
        generateDefaultEnvFile(env_path, sgrn_user, t_base_dir);
    }

    // Load .env into the process environment so that envOrDefault() calls
    // below return the user-edited values instead of hardcoded defaults.
    // See sgrn::utils::env::loadFile() for the parser implementation.
    if (auto env_result = loadEnvFile(env_path); env_result.hasError()) {
        SGRN_ERROR("DatastoreInit", "Failed to load .env: {}", env_result.error());
        std::exit(EXIT_FAILURE);
    }

    // Read env vars for templating — each call returns the env value or
    // the provided fallback if the variable is not set.
    std::string pg_db = envOrDefault("POSTGRES_DB", "sgrn");
    std::string pg_user = envOrDefault("POSTGRES_USER", "sgrn_datastore");
    std::string pg_pass = envOrDefault("POSTGRES_PASSWORD", "change_me_secure_db_password");
    std::string jwt_secret = envOrDefault("JWT_SECRET", "change_me_at_least_32_chars_jwt_secret");
    sgrn_user = envOrDefault("SGRN_USER", sgrn_user);
    std::string sgrn_data = envOrDefault("SGRN_DATA_DIR", t_base_dir);
    std::string sgrn_bin = envOrDefault("SGRN_BIN_DIR", "/usr/local/bin");
    std::string sgrn_deployment_env = envOrDefault("SGRN_DEPLOYMENT_ENV", "/home/" + sgrn_user + "/micromamba/envs/SGRN");
    std::string minio_user = envOrDefault("MINIO_ROOT_USER", "minioadmin");
    std::string minio_pass = envOrDefault("MINIO_ROOT_PASSWORD", "change_me_minio_password");

    auto tmpl = [&](std::string text) -> std::string {
        return replaceTemplateVars(std::move(text), pg_db, pg_pass, jwt_secret, sgrn_user, sgrn_data, sgrn_bin, sgrn_deployment_env,
            minio_user, minio_pass, pg_user);
    };

    generateSelfSignedCert(fs::path(t_base_dir) / "certs");
    extractConfigAssets(t_base_dir, tmpl);
    generateSystemdServices(t_base_dir, tmpl);

    return env_exists;
}

inline void initDatabaseOnly(const std::string& t_base_dir) {
    SGRN_INFO("DatastoreInit", "Starting Database Initialization...");

    namespace fs = std::filesystem;
    fs::path env_path = fs::path(t_base_dir) / ".env";
    if (!fs::exists(env_path)) {
        SGRN_ERROR("DatastoreInit", "Configuration file .env not found. Please run --generate-config first.");
        std::exit(EXIT_FAILURE);
    }

    // Load .env to populate DB connection parameters before reading them.
    if (auto env_result = loadEnvFile(env_path); env_result.hasError()) {
        SGRN_ERROR("DatastoreInit", "Failed to load .env: {}", env_result.error());
        std::exit(EXIT_FAILURE);
    }

    // Database connection parameters — read from env with safe defaults.
    std::string pg_host = envOrDefault("POSTGRES_HOST", "127.0.0.1");
    std::string pg_port = envOrDefault("POSTGRES_PORT", "5432");
    std::string pg_db = envOrDefault("POSTGRES_DB", "sgrn");
    std::string pg_superuser = envOrDefault("POSTGRES_SUPERUSER", envOrDefault("USER", "postgres"));
    std::string pg_user = envOrDefault("POSTGRES_USER", "sgrn_datastore");
    std::string pg_pass = envOrDefault("POSTGRES_PASSWORD", "change_me_secure_db_password");
    std::string jwt_secret = envOrDefault("JWT_SECRET", "change_me_at_least_32_chars_jwt_secret");
    std::string sgrn_user = envOrDefault("SGRN_USER", "admin");
    std::string sgrn_data = envOrDefault("SGRN_DATA_DIR", t_base_dir);
    std::string sgrn_bin = envOrDefault("SGRN_BIN_DIR", "/usr/local/bin");
    std::string sgrn_deployment_env = envOrDefault("SGRN_DEPLOYMENT_ENV", "/home/" + sgrn_user + "/micromamba/envs/SGRN");
    std::string minio_user = envOrDefault("MINIO_ROOT_USER", "minioadmin");
    std::string minio_pass = envOrDefault("MINIO_ROOT_PASSWORD", "change_me_minio_password");

    auto tmpl = [&](std::string text) -> std::string {
        return replaceTemplateVars(std::move(text), pg_db, pg_pass, jwt_secret, sgrn_user, sgrn_data, sgrn_bin, sgrn_deployment_env,
            minio_user, minio_pass, pg_user);
    };

    std::string sql = decompressSqlAssets(tmpl);

    std::string bootstrap_conn = fmt::format("host={} port={} dbname=postgres user={}", pg_host, pg_port, pg_superuser);
    if (const char* p_su_pass = std::getenv("POSTGRES_SUPERUSER_PASSWORD")) {
        bootstrap_conn += fmt::format(" password={}", p_su_pass);
    }

    trantor::EventLoopThread loop_thread;
    loop_thread.run();

    SGRN_INFO("DatastoreInit", "Bootstrapping via connection: {}", bootstrap_conn);
    auto client = drogon::orm::DbClient::newPgClient(bootstrap_conn, 1, loop_thread.getLoop());

    recreateDatabase(client.get(), pg_db);

    fs::path schema_path = fs::path(t_base_dir) / "schema.sql";
    {
        std::ofstream sql_out(schema_path, std::ios::binary);
        sql_out << sql;
    }

    executeSchemaViaPsql(pg_host, pg_port, pg_superuser, schema_path, sgrn_deployment_env);
}

inline void configureSystemd() {
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

    // Load .env to get the data directory and deployment env paths.
    if (auto env_result = loadEnvFile(env_path); env_result.hasError()) {
        SGRN_ERROR("DatastoreInit", "Failed to load .env: {}", env_result.error());
        std::exit(EXIT_FAILURE);
    }

    // Resolve runtime paths from env (set by --generate-config earlier).
    std::string sgrn_data = envOrDefault("SGRN_DATA_DIR", base_dir.string());
    std::string sgrn_deployment_env = envOrDefault("SGRN_DEPLOYMENT_ENV", "/home/" + sudo_user + "/micromamba/envs/SGRN");

    stopLegacyServices(sudo_user, user_uid);

    fs::path systemd_src = fs::path(sgrn_data) / "systemd";
    fs::path nginx_src = fs::path(sgrn_data) / "nginx";
    fs::path systemd_dst = "/etc/systemd/system";
    fs::path nginx_dst = fs::path(sgrn_deployment_env) / "etc" / "nginx";

    std::vector<std::string> changed_services;

    // Sync systemd unit files
    if (fs::exists(systemd_src)) {
        for (const auto& entry : fs::directory_iterator(systemd_src)) {
            if (entry.is_regular_file() && entry.path().extension() == ".service") {
                if (entry.path().filename() == "nginx.service" && fs::file_size(entry.path()) == 0)
                    continue;
                fs::path dest = systemd_dst / entry.path().filename();
                if (auto copy_result = copyIfChanged(entry.path(), dest, false, user_uid, user_gid); !copy_result.hasError()) {
                    changed_services.push_back(dest.filename().string());
                }
            }
        }
    }

    // Sync nginx configs
    if (fs::exists(nginx_src)) {
        for (const auto& entry : fs::recursive_directory_iterator(nginx_src)) {
            if (entry.is_regular_file()) {
                fs::path rel = fs::relative(entry.path(), nginx_src);
                fs::path dest = nginx_dst / rel;
                if (auto copy_result = copyIfChanged(entry.path(), dest, true, user_uid, user_gid); !copy_result.hasError()) {
                    changed_services.push_back("SGRN-nginx.service");
                }
            }
        }
    }

    restartChangedServices(changed_services);
}

inline void bootstrapDatabase() {
    std::string base_dir = sgrn::utils::filesystem::expandUserPath(std::string(kDefaultOperationDir));
    bool env_exists = generateConfigOnly(base_dir);
    if (!env_exists) {
        return; // stop and let user configure secrets
    }
    initDatabaseOnly(base_dir);

    SGRN_INFO("DatastoreInit", "");
    SGRN_INFO("DatastoreInit", "╔══════════════════════════════════════════════╗");
    SGRN_INFO("DatastoreInit", "║     SGRN Platform Ready!                     ║");
    SGRN_INFO("DatastoreInit", "╚══════════════════════════════════════════════╝");
    SGRN_INFO("DatastoreInit", "To install and activate system-wide systemd services:");
    SGRN_INFO("DatastoreInit", "  sudo <bin> --config-systemd");
    SGRN_INFO("DatastoreInit", "");
}

} // namespace sgrn::datastore::bootstrap
