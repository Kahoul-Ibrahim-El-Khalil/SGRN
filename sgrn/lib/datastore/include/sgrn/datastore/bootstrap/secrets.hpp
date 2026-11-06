#pragma once
// sgrn/lib/datastore/include/sgrn/datastore/bootstrap/secrets.hpp
//
// Shared primitives for secret material: CSPRNG generation (getrandom),
// no-echo terminal prompts, .env upserting/permissions and change_me_*
// placeholder detection for the unconfigured-install gate.
//
// Consumers: secrets-wizard.hpp (--init) and secrets-rotate.hpp (--rotate).
// bootstrap.hpp includes both and re-exposes the helpers below.
// Definitions of the larger primitives live in src/bootstrap/secrets.cpp.

#include <sgrn/utils/filesystem.hpp>
#include <sgrn/utils/strings.hpp>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sgrn::datastore::bootstrap
{
// ---------------------------------------------------------------------------
// Placeholder detection (the unconfigured-install gate)
// ---------------------------------------------------------------------------
//
// `--generate-config` run WITHOUT the wizard writes .env with change_me_*
// placeholders. buildTemplateResolver() substitutes them into live configs and
// rendered systemd units, so a "deployed" platform can silently run on
// published defaults. initDatabaseOnly() refuses that state.

/// env key -> the placeholder token bare `--generate-config` writes for it.
inline const std::vector<std::pair<std::string_view, std::string_view>>& secretPlaceholderMap() {
    static const std::vector<std::pair<std::string_view, std::string_view>> kPlaceholders = {
        {"POSTGRES_PASSWORD", "change_me_secure_db_password"},
        {"JWT_SECRET", "change_me_at_least_32_chars_jwt_secret"},
        {"GARAGE_ACCESS_KEY", "change_me_garage_access_key"},
        {"GARAGE_SECRET_KEY", "change_me_garage_secret_key"},
        {"GARAGE_RPC_SECRET", "change_me_garage_rpc_secret_64_hex_chars"},
        {"GARAGE_ADMIN_TOKEN", "change_me_garage_admin_token_64_hex_chars"},
    };
    return kPlaceholders;
}

/// True when a currently-visible env value is still its "change_me" default.
/// Only meaningful AFTER the .env has been loaded into the environment.
inline bool envHasPlaceholderSecret() {
    for (const auto& [key, placeholder] : secretPlaceholderMap()) {
        if (const char* value = ::getenv(key.data()); value != nullptr && placeholder == value) {
            return true;
        }
    }
    return false;
}

/// Keys (from secretPlaceholderMap) whose current env value is a placeholder.
inline std::vector<std::string> placeholderEnvKeys() {
    std::vector<std::string> keys;
    for (const auto& [key, placeholder] : secretPlaceholderMap()) {
        if (const char* value = ::getenv(key.data()); value != nullptr && placeholder == value) {
            keys.emplace_back(key);
        }
    }
    return keys;
}

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------

/// Bytes of entropy per key. Matches the ephemeral dev-garage profile in
/// web/datastore/dev.ts (access 12, secret 20, tokens 32) plus a DB/JWT size
/// that clears the HS256 >= 32-byte and bcrypt-friendly minimums.
inline size_t secretByteLength(std::string_view t_key) {
    if (t_key == "POSTGRES_PASSWORD")
        return 18;
    if (t_key == "JWT_SECRET")
        return 32;
    if (t_key == "GARAGE_ACCESS_KEY")
        return 12;
    if (t_key == "GARAGE_SECRET_KEY")
        return 20;
    return 32; // GARAGE_RPC_SECRET / GARAGE_ADMIN_TOKEN / unknown keys
}

/// Minimum accepted length for a secrets the OPERATOR types by hand.
inline size_t secretMinLength(std::string_view t_key) {
    if (t_key == "POSTGRES_PASSWORD")
        return 12;
    return 32; // JWT secret, garage tokens — HS256/garage floor
}

/// CSPRNG secret generation: t_bytes random octets as lowercase hex.
/// Linux getrandom(2) with a std::random_device fallback (never rand()).
std::string generateSecretHex(size_t t_bytes);

// ---------------------------------------------------------------------------
// Prompts
// ---------------------------------------------------------------------------

/// Plain prompt; returns the typed line ("" on EOF). Empty means "default".
std::string promptLine(const std::string& t_prompt);

/// No-echo prompt (termios hides the typing). Falls back to echo when stdin
/// is piped (CI runs) — there is no tty to hide from in the first place.
std::string promptSecret(const std::string& t_prompt);

// ---------------------------------------------------------------------------
// .env surgery
// ---------------------------------------------------------------------------

using sgrn::utils::filesystem::ensureOwnerOnlyPerms;
using sgrn::utils::strings::sqlEscape;

/// Replace (or append) one KEY=VALUE line in an .env file, preserving every
/// other byte. Returns true when the file was changed.
bool upsertEnvVar(const std::filesystem::path& t_env_path, const std::string& t_key, const std::string& t_value);

/// Resolve $ENV{NAME} references inside one already-loaded env value and
/// persist the concrete value into the .env (best effort) + this process.
/// Needed because loadEnvFile does NOT expand $ENV{} — only
/// buildTemplateResolver does, for the staged config files — while shell
/// invocations (psql) build paths directly from SGRN_DEPLOYMENT_ENV.
void resolveEnvPlaceholderKey(const std::filesystem::path& t_env_path, const std::string& t_key);
} // namespace sgrn::datastore::bootstrap
