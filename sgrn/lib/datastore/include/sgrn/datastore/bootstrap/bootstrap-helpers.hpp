#pragma once
/**
 * @file  sgrn/datastore/bootstrap/bootstrap-helpers.hpp
 * @brief Small shared helpers used by multiple bootstrap sub-headers.
 *
 * Intentionally lightweight: no generated headers, no Drogon, no trantor.
 * Included by config-gen.hpp, verify.hpp, systemd.hpp and bootstrap.hpp.
 */

#include <sgrn/Result.hpp>
#include <sgrn/utils/env.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <filesystem>
#include <string>
#include <unistd.h>

namespace sgrn::datastore::bootstrap
{

/// Parse a .env file and setenv() each KEY=VALUE pair.
inline sgrn::Result<void, std::string> loadEnvFile(const std::filesystem::path& t_env_path) {
    return sgrn::utils::env::loadFile(t_env_path);
}

/// Read an env var or return a default if unset.
inline std::string envOrDefault(const char* t_key, const std::string& t_default) {
    return sgrn::utils::env::get(t_key, t_default);
}

/// Directory containing the currently running binary (via /proc/self/exe).
/// Empty when it cannot be resolved (non-Linux, confined environments).
inline std::string currentExeDir() {
    return sgrn::utils::filesystem::currentExeDir();
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

} // namespace sgrn::datastore::bootstrap
