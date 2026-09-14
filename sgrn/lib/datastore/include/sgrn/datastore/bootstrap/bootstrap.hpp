#pragma once
#include <drogon/orm/DbClient.h>
#include <fmt/core.h>
#include <sgrn/assets/EmbeddedAsset.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/app.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/env.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <algorithm>
#include <chrono>
#include <config_assets.hpp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <pwd.h>
#include <sql_assets.hpp>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <system_error>
#include <thread>
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

/// Directory containing the currently running binary (via /proc/self/exe).
/// Empty when it cannot be resolved (non-Linux, confined environments).
inline std::string currentExeDir() {
    namespace fs = std::filesystem;
    char exe_buf[4096] = {};
    ssize_t exe_len = ::readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (exe_len <= 0)
        return {};
    return fs::path(std::string(exe_buf, static_cast<size_t>(exe_len))).parent_path().string();
}

/// Whether t_dir contains an executable sgrn_datastore binary.
inline bool hasDatastoreBinary(const std::string& t_dir) {
    namespace fs = std::filesystem;
    if (t_dir.empty())
        return false;
    std::error_code ec;
    auto st = fs::status(fs::path(t_dir) / "sgrn_datastore", ec);
    if (ec)
        return false;
    return fs::is_regular_file(st) && (st.permissions() & fs::perms::owner_exec) != fs::perms::none;
}

/// Replace (or append) one KEY=VALUE line in an .env file, preserving every
/// other byte. Returns true when the file was changed.
inline bool upsertEnvVar(const std::filesystem::path& t_env_path, const std::string& t_key, const std::string& t_value) {
    std::ifstream in(t_env_path);
    if (!in)
        return false;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string prefix = t_key + "=";
    bool changed = false;
    {
        std::string out;
        size_t pos = 0;
        while (pos < content.size()) {
            size_t eol = content.find('\n', pos);
            std::string line = content.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
            if (!changed && line.rfind(prefix, 0) == 0) {
                line = prefix + t_value;
                changed = true;
            }
            out += line;
            if (eol == std::string::npos)
                break;
            out += '\n';
            pos = eol + 1;
        }
        if (!changed) {
            if (!out.empty() && out.back() != '\n')
                out += '\n';
            out += prefix + t_value + "\n";
            changed = true;
        }
        content = std::move(out);
    }
    std::ofstream out_file(t_env_path, std::ios::binary | std::ios::trunc);
    if (!out_file)
        return false;
    out_file << content;
    return changed;
}

inline std::string replaceTemplateVars(std::string t_text, const std::string& t_pg_db, const std::string& t_pg_pass,
    const std::string& t_jwt_secret, const std::string& t_sgrn_user = {}, const std::string& t_sgrn_data = {},
    const std::string& t_sgrn_bin = {}, const std::string& t_sgrn_deployment_env = {}, const std::string& t_pg_user = {},
    const std::string& t_garage_access = {}, const std::string& t_garage_secret = {}, const std::string& t_garage_rpc = {},
    const std::string& t_garage_admin = {}) {
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
    replace_all(t_text, "${GARAGE_ACCESS_KEY}", t_garage_access);
    replace_all(t_text, "${GARAGE_SECRET_KEY}", t_garage_secret);
    replace_all(t_text, "${GARAGE_RPC_SECRET}", t_garage_rpc);
    replace_all(t_text, "${GARAGE_ADMIN_TOKEN}", t_garage_admin);
    return t_text;
}

// ---------------------------------------------------------------------------
// generateConfigOnly Helpers
// ---------------------------------------------------------------------------

inline void createDirectoryStructure(const std::string& t_base_dir) {
    namespace fs = std::filesystem;
    fs::create_directories(t_base_dir);
    fs::create_directories(fs::path(t_base_dir) / "Postgres" / "data");
    fs::create_directories(fs::path(t_base_dir) / "garage" / "meta");
    fs::create_directories(fs::path(t_base_dir) / "garage" / "data");
    fs::create_directories(fs::path(t_base_dir) / "var" / "log" / "nginx");
    fs::create_directories(fs::path(t_base_dir) / "var" / "run");
}

inline void generateDefaultEnvFile(const std::filesystem::path& t_env_path, const std::string& t_user, const std::string& t_base_dir) {
    namespace fs = std::filesystem;

    // Resolve runtime paths so the .env is portable
    std::string bin_dir = currentExeDir();
    if (bin_dir.empty())
        bin_dir = "/usr/local/bin";
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
    env_out << "# App-level role created by the schema (used by the datastore)\n";
    env_out << "POSTGRES_USER=sgrn_datastore\n";
    env_out << "POSTGRES_PASSWORD=change_me_secure_db_password\n\n";

    env_out << "# ── JWT (must be ≥ 32 chars for HS256) ────\n";
    env_out << "JWT_SECRET=change_me_at_least_32_chars_jwt_secret\n\n";

    env_out << "# ── Garage Object Storage ────────────────────\n";
    env_out << "# S3 API on :3900, region us-east-1 (matches sgrn.json).\n";
    env_out << "# Provision once per machine after --generate-config:\n";
    env_out << "#   garage -c $SGRN_DATA_DIR/garage/garage.toml layout assign -z dc1 -c 50G <node-id>\n";
    env_out << "#   garage -c $SGRN_DATA_DIR/garage/garage.toml layout apply --version 1\n";
    env_out
        << "#   garage -c $SGRN_DATA_DIR/garage/garage.toml key import <GARAGE_ACCESS_KEY> <GARAGE_SECRET_KEY> -n sgrn-datastore --yes\n";
    env_out << "#   garage -c $SGRN_DATA_DIR/garage/garage.toml bucket create sgrn-uploads\n";
    env_out << "#   garage -c $SGRN_DATA_DIR/garage/garage.toml bucket allow --read --write sgrn-uploads --key sgrn-datastore\n";
    env_out << "GARAGE_S3_ENDPOINT=http://127.0.0.1:3900\n";
    env_out << "GARAGE_REGION=us-east-1\n";
    env_out << "GARAGE_ACCESS_KEY=change_me_garage_access_key\n";
    env_out << "GARAGE_SECRET_KEY=change_me_garage_secret_key\n";
    env_out << "GARAGE_RPC_SECRET=change_me_garage_rpc_secret_64_hex_chars\n";
    env_out << "GARAGE_ADMIN_TOKEN=change_me_garage_admin_token_64_hex_chars\n";

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
            // Dashboard-tuned live configs must survive reinit: back up the
            // existing sgrn.json to sgrn.json.bak instead of silently
            // discarding admin changes. Other assets are stateless templates.
            if (out_path.filename() == "sgrn.json" && fs::exists(out_path)) {
                std::error_code backup_ec;
                fs::copy_file(out_path, fs::path(out_path.string() + ".bak"), fs::copy_options::overwrite_existing, backup_ec);
                if (backup_ec) {
                    SGRN_WARN("DatastoreInit", "  -> Could not back up {}: {}", out_path.string(), backup_ec.message());
                } else {
                    SGRN_INFO("DatastoreInit", "  -> Backed up live config to {}.bak", out_path.string());
                }
            }
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

    static constexpr std::string_view kSystemdServices[] = {"SGRN-datastore.service", "SGRN-garage.service", "SGRN-postgres.service",
        "SGRN-redis.service", "SGRN-nginx.service", "sgrn.service"};

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
    // NOTE: SGRN-postgrest.service is intentionally still stopped here: it is
    // obsolete (no longer deployed), so any lingering user-level instance
    // must be torn down, never started.
    std::string cmd = fmt::format("sudo -u {} XDG_RUNTIME_DIR=/run/user/{} systemctl --user stop "
                                  "SGRN-nginx.service sgrn.service SGRN-datastore.service "
                                  "SGRN-garage.service SGRN-minio.service SGRN-postgres.service SGRN-postgrest.service "
                                  "SGRN-redis.service 2>/dev/null || true",
        t_sudo_user, t_user_uid);
    std::system(cmd.c_str());
}

// Units removed from the deployment. If a previous release installed them,
// --config-systemd disables and deletes them so they can neither start at
// boot nor linger as confusion. System-level paths need root (guaranteed by
// the configureSystemd() euid check); every command tolerates absence.
inline void removeObsoleteUnits(const std::filesystem::path& t_data_dir) {
    namespace fs = std::filesystem;
    static constexpr std::string_view kObsoleteUnits[] = {"SGRN-postgrest.service", "SGRN-minio.service"};
    static constexpr std::string_view kObsoleteDataFiles[] = {
        "systemd/SGRN-postgrest.service", "systemd/SGRN-minio.service", "postgrest.conf"};

    for (auto unit : kObsoleteUnits) {
        SGRN_INFO("DatastoreInit", "Removing obsolete unit {} (if present)...", unit);
        std::system(fmt::format("systemctl disable --now {} 2>/dev/null || true", unit).c_str());
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

inline sgrn::Result<void, std::string> copyIfChanged(
    const std::filesystem::path& t_src, const std::filesystem::path& t_dst, bool t_is_nginx, uid_t t_uid, gid_t t_gid) {
    namespace fs = std::filesystem;
    if (!fs::exists(t_src)) {
        return sgrn::Result<void, std::string>::Error(fmt::format("Source file does not exist: {}", t_src.string()));
    }
    // Compare content, not just size: template substitutions (hostnames,
    // secrets, paths) routinely preserve file size while changing meaning.
    bool copy_needed = true;
    if (fs::exists(t_dst)) {
        std::error_code ec1, ec2;
        auto src_size = fs::file_size(t_src, ec1);
        auto dst_size = fs::file_size(t_dst, ec2);
        if (!ec1 && !ec2 && src_size == dst_size && src_size <= (1u << 24)) {
            std::ifstream f_src(t_src, std::ios::binary), f_dst(t_dst, std::ios::binary);
            if (f_src && f_dst) {
                copy_needed = !std::equal(
                    std::istreambuf_iterator<char>(f_src), std::istreambuf_iterator<char>(), std::istreambuf_iterator<char>(f_dst));
            }
        }
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

// Dedupe (the nginx sync pushes one entry per changed file) and order by
// dependency tier — infrastructure first, the sgrn.service umbrella last —
// so a restart run never bounces dependents before their dependencies.
inline std::vector<std::string> orderServicesForRestart(std::vector<std::string> t_services) {
    std::vector<std::string> ordered;
    for (auto& svc : t_services) {
        if (std::find(ordered.begin(), ordered.end(), svc) == ordered.end())
            ordered.push_back(std::move(svc));
    }
    auto tier = [](const std::string& t_svc) {
        if (t_svc == "SGRN-postgres.service" || t_svc == "SGRN-redis.service")
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

// Best-effort query of a unit's Type (e.g. "oneshot", "simple").
// Empty string when systemctl is unavailable.
inline std::string unitType(const std::string& t_svc) {
    std::string out;
    FILE* pipe = ::popen(fmt::format("systemctl show -p Type --value {} 2>/dev/null", t_svc).c_str(), "r");
    if (pipe == nullptr)
        return out;
    char buf[64];
    if (std::fgets(buf, sizeof(buf), pipe) != nullptr)
        out = buf;
    ::pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();
    return out;
}

// Logs a `systemctl status` snapshot so a failed unit's cause is visible
// inline in the deploy log instead of requiring a follow-up journalctl run.
inline void logUnitStatus(const std::string& t_svc) {
    FILE* pipe = ::popen(fmt::format("systemctl status {} --no-pager -l -n 15 2>&1", t_svc).c_str(), "r");
    if (pipe == nullptr)
        return;
    std::string out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
        out += buf;
        if (out.size() > 4000)
            break;
    }
    ::pclose(pipe);
    SGRN_ERROR("DatastoreInit", "--- systemctl status {} ---\n{}", t_svc, out);
}

inline bool restartOneService(const std::string& t_svc) {
    // Clear any latched failed / rate-limited state first: without this, a
    // unit that crash-looped earlier refuses every new start ("start of the
    // service was attempted too often") even after the cause is fixed.
    std::system(fmt::format("systemctl reset-failed {} 2>/dev/null || true", t_svc).c_str());
    SGRN_INFO("DatastoreInit", "Restarting service {}...", t_svc);
    if (std::system(fmt::format("systemctl restart {}", t_svc).c_str()) == 0)
        return true;
    SGRN_ERROR("DatastoreInit", "systemctl restart {} failed.", t_svc);
    // `reload` is never applicable to oneshot units (e.g. the sgrn.service
    // umbrella) — attempting it only adds a confusing second error.
    if (unitType(t_svc) == "oneshot") {
        SGRN_INFO("DatastoreInit", "{} is oneshot: skipping reload fallback.", t_svc);
    } else if (std::system(fmt::format("systemctl reload {}", t_svc).c_str()) == 0) {
        SGRN_INFO("DatastoreInit", "{} reloaded as fallback.", t_svc);
        return true;
    }
    logUnitStatus(t_svc);
    return false;
}

inline bool restartChangedServices(const std::vector<std::string>& t_services) {
    if (t_services.empty()) {
        SGRN_INFO("DatastoreInit", "No changes detected. Systemd services are up to date.");
        return true;
    }
    SGRN_INFO("DatastoreInit", "Reloading systemd daemon...");
    if (std::system("systemctl daemon-reload") != 0) {
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
    if (std::system("systemctl is-system-running --quiet 2>/dev/null") == 0) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        for (const auto& svc : t_services) {
            if (std::system(fmt::format("systemctl is-active --quiet {}", svc).c_str()) != 0) {
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

// Enables units for boot persistence. Best-effort: `enable` only writes
// symlinks (works without a running systemd), but report failures loudly.
inline bool enableServices(const std::vector<std::string>& t_services) {
    bool ok = true;
    for (const auto& svc : t_services) {
        if (std::system(fmt::format("systemctl enable {} 2>/dev/null", svc).c_str()) != 0) {
            SGRN_WARN("DatastoreInit", "Could not enable {} for boot (continuing).", svc);
            ok = false;
        } else {
            SGRN_INFO("DatastoreInit", "  -> Enabled {} for boot.", svc);
        }
    }
    return ok;
}

// Blocks until PostgreSQL accepts connections (or a ~60s timeout), so
// dependents restarted right after postgres don't boot against a cold
// server. Skipped with a warning when pg_isready is unavailable.
inline void waitForPostgres(const std::string& t_host, const std::string& t_port, const std::string& t_pg_isready) {
    if (t_pg_isready.empty() || !std::filesystem::exists(t_pg_isready)) {
        SGRN_WARN("DatastoreInit", "pg_isready not found; skipping postgres readiness wait.");
        return;
    }
    SGRN_INFO("DatastoreInit", "Waiting for PostgreSQL at {}:{} to accept connections...", t_host, t_port);
    for (int i = 0; i < 30; ++i) {
        if (std::system(fmt::format("{} -h {} -p {} -t 2 2>/dev/null", t_pg_isready, t_host, t_port).c_str()) == 0) {
            SGRN_INFO("DatastoreInit", "PostgreSQL is accepting connections.");
            return;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    SGRN_WARN("DatastoreInit", "PostgreSQL did not become ready in ~60s; continuing anyway.");
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

    // Heal a stale SGRN_BIN_DIR: it is captured once when .env is created,
    // so moving the checkout (or rebuilding elsewhere) leaves deployed units
    // pointing at a dead path (systemd 203/EXEC crash-loop). Only touch it
    // when the recorded dir no longer holds the binary; a valid custom
    // value is never overwritten.
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
    std::string garage_access = envOrDefault("GARAGE_ACCESS_KEY", "change_me_garage_access_key");
    std::string garage_secret = envOrDefault("GARAGE_SECRET_KEY", "change_me_garage_secret_key");
    std::string garage_rpc = envOrDefault("GARAGE_RPC_SECRET", "change_me_garage_rpc_secret_64_hex_chars");
    std::string garage_admin = envOrDefault("GARAGE_ADMIN_TOKEN", "change_me_garage_admin_token_64_hex_chars");

    auto tmpl = [&](std::string text) -> std::string {
        return replaceTemplateVars(std::move(text), pg_db, pg_pass, jwt_secret, sgrn_user, sgrn_data, sgrn_bin, sgrn_deployment_env,
            pg_user, garage_access, garage_secret, garage_rpc, garage_admin);
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

    auto tmpl = [&](std::string text) -> std::string {
        return replaceTemplateVars(
            std::move(text), pg_db, pg_pass, jwt_secret, sgrn_user, sgrn_data, sgrn_bin, sgrn_deployment_env, pg_user);
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

    // Preflight: the units ExecStart ${SGRN_BIN_DIR}/sgrn_datastore. A stale
    // path deploys units that die with systemd 203/EXEC and crash-loop the
    // whole platform — refuse with remediation instead of deploying them.
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
    fs::path nginx_dst = fs::path(sgrn_deployment_env) / "etc" / "nginx";

    // Precondition: units are staged by `sgrn_datastore --generate-config`
    // (step 1 of the deployment runbook). Refuse loudly instead of
    // reporting a misleading "up to date".
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

    // Drop units/files of services that no longer exist (e.g. PostgREST),
    // from both the data dir and the system, before syncing the current set.
    removeObsoleteUnits(sgrn_data);

    std::vector<std::string> changed_services;
    bool copy_failed = false;

    // Sync systemd unit files
    if (fs::exists(systemd_src)) {
        for (const auto& entry : fs::directory_iterator(systemd_src)) {
            if (entry.is_regular_file() && entry.path().extension() == ".service") {
                if (entry.path().filename() == "nginx.service" && fs::file_size(entry.path()) == 0)
                    continue;
                fs::path dest = systemd_dst / entry.path().filename();
                if (auto copy_result = copyIfChanged(entry.path(), dest, false, user_uid, user_gid); !copy_result.hasError()) {
                    changed_services.push_back(dest.filename().string());
                } else {
                    SGRN_ERROR("DatastoreInit", "{}", copy_result.error());
                    copy_failed = true;
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
                } else {
                    SGRN_ERROR("DatastoreInit", "{}", copy_result.error());
                    copy_failed = true;
                }
            }
        }
    }

    if (copy_failed) {
        SGRN_ERROR("DatastoreInit", "Error: one or more service files could not be deployed. Nothing was restarted.");
        std::exit(EXIT_FAILURE);
    }

    // Dedupe + tier order (infra first, sgrn.service umbrella last).
    changed_services = orderServicesForRestart(std::move(changed_services));

    enableServices(changed_services);

    // If postgres itself was (re)deployed, wait for readiness before
    // bouncing dependents against a cold server.
    if (std::find(changed_services.begin(), changed_services.end(), "SGRN-postgres.service") != changed_services.end()) {
        waitForPostgres(envOrDefault("POSTGRES_HOST", "127.0.0.1"), envOrDefault("POSTGRES_PORT", "5432"),
            (fs::path(sgrn_deployment_env) / "bin" / "pg_isready").string());
    }

    if (!restartChangedServices(changed_services)) {
        std::exit(EXIT_FAILURE);
    }
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
