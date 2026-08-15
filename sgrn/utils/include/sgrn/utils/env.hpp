#pragma once

/// @file env.hpp
/// @brief Environment variable utilities.
///
/// Provides helpers for reading environment variables with defaults
/// and loading `.env` files into the process environment.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include <sgrn/Result.hpp>

namespace sgrn::utils::env
{

/// Read an environment variable or return a default if unset.
///
/// Use case: Bootstrap and configuration code that needs to read
/// deployment parameters (POSTGRES_HOST, JWT_SECRET, MINIO_ROOT_USER, …)
/// with sensible fallbacks when the variable is not yet exported.
///
/// Example:
///   std::string host = sgrn::utils::env::get("POSTGRES_HOST", "127.0.0.1");
///
/// @param t_key  The environment variable name.
/// @param t_default  The value to return if the variable is not set.
/// @return The environment variable value, or t_default if unset.
inline std::string get(const char* t_key, const std::string& t_default = {}) {
    const char* val = std::getenv(t_key);
    return val ? std::string(val) : t_default;
}

/// Parse a `.env` file and call `setenv()` for each `KEY=VALUE` pair.
/// Comments (lines starting with `#`) and blank lines are skipped.
///
/// Use case: The SGRN datastore bootstrap flow generates a default `.env`
/// on first run, then loads it so that subsequent `sgrn::utils::env::get()`
/// calls return the user-edited values instead of hardcoded defaults.
/// This is called in `generateConfigOnly()`, `initDatabaseOnly()`, and
/// `configureSystemd()` to ensure env vars are populated before reading.
///
/// Returns a Result to signal file-open failures rather than silently
/// ignoring a missing `.env`.
///
/// Example:
///   auto result = sgrn::utils::env::loadFile(env_path);
///   if (result.hasError())
///       SGRN_ERROR("Bootstrap", "Failed to load .env: {}", result.error());
///
/// @param t_env_path  Path to the `.env` file.
/// @return Result<void, std::string> — success if the file was parsed, or an
///         error string if the file could not be opened.
inline sgrn::Result<void, std::string> loadFile(const std::filesystem::path& t_env_path) {
    std::ifstream env_file(t_env_path);
    if (!env_file.is_open()) {
        return sgrn::Result<void, std::string>::Error(fmt::format("Failed to open .env file: {}", t_env_path.string()));
    }
    std::string line;
    while (std::getline(env_file, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        auto pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = line.substr(0, pos);
            std::string val = line.substr(pos + 1);
            setenv(key.c_str(), val.c_str(), 1);
        }
    }
    return {};
}

} // namespace sgrn::utils::env