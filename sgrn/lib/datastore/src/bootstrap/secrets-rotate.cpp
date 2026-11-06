// sgrn/lib/datastore/src/bootstrap/secrets-rotate.cpp
//
// `--rotate KEY[=VALUE]` — safe secret rotation. Definitions for
// include/sgrn/datastore/bootstrap/secrets-rotate.hpp (the orchestration
// entry runRotation is the only symbol exported by the header; everything
// else here is file-local).

#include <sgrn/datastore/bootstrap/secrets-rotate.hpp>

#include <fmt/core.h>
#include <sgrn/datastore/bootstrap/secrets.hpp>
#include <sgrn/utils/env.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace sgrn::datastore::bootstrap::rotate
{
namespace fs = std::filesystem;

namespace
{

/// Keys the rotate command accepts. GARAGE_RPC_SECRET is deliberately absent.
bool isRotatableKey(const std::string& t_key) {
    return t_key == "POSTGRES_PASSWORD" || t_key == "JWT_SECRET" || t_key == "GARAGE_ACCESS_KEY" || t_key == "GARAGE_SECRET_KEY" ||
           t_key == "GARAGE_ADMIN_TOKEN";
}

bool listContains(const std::vector<std::string>& t_keys, std::string_view t_key) {
    for (const auto& key : t_keys) {
        if (key == t_key) {
            return true;
        }
    }
    return false;
}

// UTC timestamp, filename-friendly (used for the backup name and audit line).
std::string utcStamp(const char* t_format) {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[64] = {};
    std::strftime(buf, sizeof(buf), t_format, std::gmtime(&now));
    return buf;
}

/// psql path: prefer the deployment env (same precedence as
/// executeSchemaViaPsql), fall back to the process PATH.
std::string resolvePsql() {
    const std::string deployment_env = sgrn::utils::env::get("SGRN_DEPLOYMENT_ENV", "");
    if (!deployment_env.empty() && deployment_env.find("$ENV{") == std::string::npos) {
        fs::path candidate = fs::path(deployment_env) / "bin" / "psql";
        std::error_code ec;
        if (fs::is_regular_file(candidate, ec)) {
            return candidate.string();
        }
    }
    return "psql";
}

// Step 2 (postgres): ALTER ROLE FIRST. Returns false with a reason when the
// role could not be updated — the caller must then abort WITHOUT touching
// .env, otherwise the next datastore restart cannot connect.
bool alterPostgresRole(const std::string& t_role, const std::string& t_new_password) {
    const std::string psql = resolvePsql();
    std::string env_prefix;
    if (const char* su_pass = ::getenv("POSTGRES_SUPERUSER_PASSWORD"); su_pass != nullptr) {
        env_prefix = fmt::format("PGPASSWORD='{}' ", su_pass);
    }
    const std::string cmd = fmt::format("{}{} -h {} -p {} -d postgres -U {} -tAc \"ALTER ROLE {} WITH LOGIN PASSWORD '{}';\"", env_prefix,
        psql, sgrn::utils::env::get("POSTGRES_HOST", "127.0.0.1"), sgrn::utils::env::get("POSTGRES_PORT", "5432"),
        sgrn::utils::env::get("POSTGRES_SUPERUSER", "postgres"), t_role, sqlEscape(t_new_password));
    fmt::print("  applying new password to PostgreSQL role '{}'...\n", t_role);
    const int rc = std::system(cmd.c_str());
    if (rc != 0) {
        fmt::print(stderr, "  error: ALTER ROLE failed (exit {}); .env left untouched.\n", rc);
        fmt::print("  Check that PostgreSQL is running and POSTGRES_SUPERUSER is correct.\n");
        return false;
    }
    return true;
}

} // namespace

int runRotation(const std::string& t_base_dir, const std::vector<std::pair<std::string, std::string>>& t_requests, bool t_non_interactive) {
    const fs::path env_path = fs::path(t_base_dir) / ".env";
    if (!fs::exists(env_path)) {
        fmt::print(stderr, "error: {} not found — run --init first\n", env_path.string());
        return 1;
    }
    if (auto env_result = sgrn::utils::env::loadFile(env_path); env_result.hasError()) {
        fmt::print(stderr, "error: cannot load {}: {}\n", env_path.string(), env_result.error());
        return 1;
    }
    if (t_requests.empty()) {
        fmt::print(stderr, "error: --rotate requires KEY or KEY=VALUE\n"
                           "       rotatable keys: POSTGRES_PASSWORD, JWT_SECRET, GARAGE_ACCESS_KEY,\n"
                           "                       GARAGE_SECRET_KEY, GARAGE_ADMIN_TOKEN\n");
        return 1;
    }

    // Validate the request set first: any unknown/refused key aborts before
    // a single byte is written.
    for (const auto& [key, value] : t_requests) {
        if (key.empty()) {
            fmt::print(stderr, "error: --rotate received an empty KEY\n");
            return 1;
        }
        if (!isRotatableKey(key)) {
            if (key == "GARAGE_RPC_SECRET") {
                fmt::print(stderr, "error: GARAGE_RPC_SECRET is not rotatable in place.\n"
                                   "       It is Garage cluster identity material, not a credential;\n"
                                   "       changing it means re-provisioning the node (fresh install).\n");
                return 1;
            }
            fmt::print(stderr, "error: unknown key '{}'\n", key);
            return 1;
        }
    }

    fmt::print("\nRotating {} secret(s) in {}.\n", t_requests.size(), env_path.string());

    // 1. Resolve values: provided via KEY=VALUE, else prompt (interactive) or
    //    CSPRNG (Enter / --non-interactive). Length floor still enforced for
    //    provided values.
    std::vector<std::pair<std::string, std::string>> resolved;
    for (const auto& [key, value] : t_requests) {
        std::string final_value = value;
        if (final_value.empty()) {
            while (true) {
                std::string typed;
                if (!t_non_interactive) {
                    typed = promptSecret(fmt::format("  {} (Enter = generate, or type value): ", key));
                }
                final_value = typed.empty() ? generateSecretHex(secretByteLength(key)) : typed;
                if (final_value.size() >= secretMinLength(key)) {
                    break;
                }
                fmt::print(stderr, "    too short — need at least {} characters; try again\n", secretMinLength(key));
                if (t_non_interactive) {
                    final_value = generateSecretHex(secretByteLength(key));
                    break;
                }
            }
        } else if (final_value.size() < secretMinLength(key)) {
            fmt::print(stderr, "error: provided value for {} is too short (need >= {} chars)\n", key, secretMinLength(key));
            return 1;
        }
        resolved.emplace_back(key, final_value);
    }

    std::vector<std::string> keys;
    keys.reserve(resolved.size());
    std::string keys_joined;
    for (const auto& [key, value] : resolved) {
        keys.push_back(key);
        if (!keys_joined.empty()) {
            keys_joined += ',';
        }
        keys_joined += key;
    }

    // 2. Apply POSTGRES_PASSWORD to the role BEFORE anything touches .env.
    if (std::find(keys.begin(), keys.end(), "POSTGRES_PASSWORD") != keys.end()) {
        const std::string pg_role = sgrn::utils::env::get("POSTGRES_USER", "sgrn_datastore");
        for (const auto& [key, value] : resolved) {
            if (key == "POSTGRES_PASSWORD") {
                if (!alterPostgresRole(pg_role, value)) {
                    return 1;
                }
                break;
            }
        }
    }

    // 3. Archive the previous .env + append an audit line (keys only).
    const fs::path backup_dir = fs::path(t_base_dir) / "var" / "backup" / "env";
    fs::create_directories(backup_dir);
    const std::string stamp = utcStamp("%Y%m%dT%H%M%SZ");
    const fs::path backup = backup_dir / fmt::format(".env.{}", stamp);
    std::error_code copy_ec;
    fs::copy_file(env_path, backup, fs::copy_options::none, copy_ec);
    if (copy_ec) {
        fmt::print(stderr, "error: could not create backup {}: {} — aborting\n", backup.string(), copy_ec.message());
        return 1;
    }
    ensureOwnerOnlyPerms(backup);
    {
        std::ofstream audit(backup_dir / "audit.log", std::ios::app);
        audit << fmt::format("[{} UTC] rotated {} (backup {})\n", utcStamp("%Y-%m-%d %H:%M:%S"), keys_joined, backup.filename().string());
    }

    // 4. Upsert the .env (owner-only preserved) and mirror into this process.
    for (const auto& [key, value] : resolved) {
        if (!upsertEnvVar(env_path, key, value)) {
            fmt::print(stderr, "error: could not rewrite {} in {}\n", key, env_path.string());
            return 1;
        }
        ::setenv(key.c_str(), value.c_str(), 1);
    }
    ensureOwnerOnlyPerms(env_path);
    fmt::print("  .env updated (owner-only, 0600)\n");
    fmt::print("  previous version archived: {}\n", backup.string());
    fmt::print("  audit line appended: {}/audit.log\n", backup_dir.string());

    // 5. Re-render configs with the new values (the runtime does NOT expand
    //    ${VAR} — without this the rendered sgrn.json / garage.toml keep the
    //    old secrets and rotation would silently not take effect).
    fmt::print("  re-rendering configs...\n");
    generateConfigOnly(t_base_dir);

    // 6. Restart / cluster-side instructions.
    fmt::print("\n  Next steps:\n");
    fmt::print("    sudo systemctl restart SGRN-datastore.service\n");
    if (listContains(keys, "GARAGE_ACCESS_KEY") || listContains(keys, "GARAGE_SECRET_KEY")) {
        const std::string data_dir = sgrn::utils::env::get("SGRN_DATA_DIR", t_base_dir);
        const std::string access = sgrn::utils::env::get("GARAGE_ACCESS_KEY", "");
        const std::string secret_key = sgrn::utils::env::get("GARAGE_SECRET_KEY", "");
        fmt::print("  Garage cluster-side key rotation (import BEFORE restarting the datastore):\n");
        fmt::print("    garage -c {}/garage/garage.toml key import {} {} -n sgrn-datastore --yes\n", data_dir, access, secret_key);
        fmt::print("    garage -c {}/garage/garage.toml bucket allow --read --write sgrn-uploads --key sgrn-datastore\n", data_dir);
        fmt::print("    (after the datastore is healthy: `garage key list`, then `key delete <old-key-id>`)\n");
        fmt::print("    sudo systemctl restart SGRN-datastore.service\n");
    }
    if (listContains(keys, "GARAGE_ADMIN_TOKEN")) {
        fmt::print("  GARAGE_ADMIN_TOKEN is substituted into the rendered garage.toml:\n");
        fmt::print("    sudo systemctl restart SGRN-garage.service  (then the datastore)\n");
    }
    fmt::print("  verify afterwards: {}/sgrn_datastore --verify-install\n\n", sgrn::utils::env::get("SGRN_BIN_DIR", "/usr/local/bin"));
    return 0;
}
} // namespace sgrn::datastore::bootstrap::rotate
