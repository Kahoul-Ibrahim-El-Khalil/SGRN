// sgrn/lib/datastore/src/bootstrap/config-gen.cpp
//
// Config-file and certificate generation helpers — definitions for
// include/sgrn/datastore/bootstrap/config-gen.hpp.

#include <sgrn/datastore/bootstrap/config-gen.hpp>

#include <fmt/core.h>
#include <sgrn/datastore/bootstrap/bootstrap-helpers.hpp>
#include <sgrn/datastore/bootstrap/secrets.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <sgrn/utils/process.hpp>
#include <config_assets.hpp>
#include <fstream>
#include <string_view>

namespace sgrn::datastore::bootstrap
{

// ---------------------------------------------------------------------------
// Directory layout
// ---------------------------------------------------------------------------

void createDirectoryStructure(const std::string& t_base_dir) {
    namespace fs = std::filesystem;
    fs::create_directories(t_base_dir);
    fs::create_directories(fs::path(t_base_dir) / "Postgres" / "data");
    fs::create_directories(fs::path(t_base_dir) / "garage" / "meta");
    fs::create_directories(fs::path(t_base_dir) / "garage" / "data");
    fs::create_directories(fs::path(t_base_dir) / "var" / "log" / "nginx");
    fs::create_directories(fs::path(t_base_dir) / "var" / "run");
    // nginx expects pre-existing per-request buffer dirs under its -p prefix
    // (its mkdir() is not recursive). A missing var/tmp/nginx made every
    // start die with "[emerg] mkdir() ... failed (2: No such file or
    // directory)" while the unit alone was unchanged.
    for (const char* kTempSub : {"client", "proxy", "fastcgi", "uwsgi", "scgi"}) {
        fs::create_directories(fs::path(t_base_dir) / "var" / "tmp" / "nginx" / kTempSub);
    }
}

// ---------------------------------------------------------------------------
// Default .env generation
// ---------------------------------------------------------------------------

void generateDefaultEnvFile(const std::filesystem::path& t_env_path, const std::string& t_user, const std::string& t_base_dir) {
    namespace fs = std::filesystem;

    // Resolve runtime paths so the .env is portable
    std::string bin_dir = currentExeDir();
    if (bin_dir.empty())
        bin_dir = "/usr/local/bin";
    std::string conda_prefix = envOrDefault("CONDA_PREFIX", "$ENV{CONDA_PREFIX}");

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

    // Placeholder defaults are secrets-in-waiting: lock the file to the owner
    // immediately (the wizard path re-applies the same policy after --init).
    ensureOwnerOnlyPerms(t_env_path);

    SGRN_INFO("DatastoreInit", "");
    SGRN_INFO("DatastoreInit", "╔══════════════════════════════════════════════╗");
    SGRN_INFO("DatastoreInit", "║  ACTION REQUIRED: edit your secrets first!   ║");
    SGRN_INFO("DatastoreInit", "╚══════════════════════════════════════════════╝");
    SGRN_INFO("DatastoreInit", "Generated default .env → {}", t_env_path.string());
    SGRN_INFO("DatastoreInit", "Fill in the secrets, then re-run with --init-db or --init to apply them.");
    SGRN_INFO("DatastoreInit", "");
}

// ---------------------------------------------------------------------------
// TLS certificate generation
// ---------------------------------------------------------------------------

void generateSelfSignedCert(const std::filesystem::path& t_cert_dir) {
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
    int ret = sgrn::utils::process::execute(cmd);
    if (ret == 0) {
        ensureOwnerOnlyPerms(key_path);
        SGRN_INFO("DatastoreInit", "  -> Generated: {} and {}", cert_path.string(), key_path.string());
    } else {
        SGRN_WARN("DatastoreInit", "Failed to generate self-signed SSL certificate automatically (openssl returned {})", ret);
    }
}

// ---------------------------------------------------------------------------
// Embedded config asset extraction
// ---------------------------------------------------------------------------

void extractConfigAssets(const std::string& t_base_dir, const std::function<std::string(std::string)>& t_tmpl) {
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
            // Dashboard-tuned live configs and the nginx staging tree (now
            // served in place, see SGRN-nginx.service) must survive reinit:
            // back them up instead of silently discarding admin changes.
            // sgrn.json keeps its historical in-place .bak; nginx files use
            // var/backup/nginx/ because an in-place .bak inside sites-enabled
            // would be re-included by the `include sites-enabled/*` glob
            // (double-parse emergency). Other assets are stateless templates.
            const bool is_live_config = out_path.filename() == "sgrn.json" || vpath.starts_with("nginx/");
            if (is_live_config && fs::exists(out_path)) {
                std::error_code backup_ec;
                if (out_path.filename() == "sgrn.json") {
                    fs::copy_file(out_path, fs::path(out_path.string() + ".bak"), fs::copy_options::overwrite_existing, backup_ec);
                } else {
                    fs::path bak_dir = fs::path(t_base_dir) / "var" / "backup" / "nginx";
                    std::error_code mkdir_ec;
                    fs::create_directories(bak_dir, mkdir_ec);
                    fs::path bak = bak_dir / (fs::relative(out_path, fs::path(t_base_dir) / "nginx").string() + ".bak");
                    fs::create_directories(bak.parent_path(), mkdir_ec);
                    fs::copy_file(out_path, bak, fs::copy_options::overwrite_existing, backup_ec);
                }
                if (backup_ec) {
                    SGRN_WARN("DatastoreInit", "  -> Could not back up {}: {}", out_path.string(), backup_ec.message());
                } else {
                    SGRN_INFO("DatastoreInit", "  -> Backed up live config to var/backup (or .bak)");
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

// ---------------------------------------------------------------------------
// Systemd unit file generation from embedded templates
// ---------------------------------------------------------------------------

void generateSystemdServices(const std::string& t_base_dir, const std::function<std::string(std::string)>& t_tmpl) {
    namespace fs = std::filesystem;
    SGRN_INFO("DatastoreInit", "Generating systemd service files...");
    fs::path systemd_out_dir = fs::path(t_base_dir) / "systemd";
    fs::create_directories(systemd_out_dir);

    static constexpr std::string_view kSystemdServices[] = {
        "SGRN-datastore.service", "SGRN-garage.service", "SGRN-postgres.service", "SGRN-nginx.service", "sgrn.service"};

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

} // namespace sgrn::datastore::bootstrap
