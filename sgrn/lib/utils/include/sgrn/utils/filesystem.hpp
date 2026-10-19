#pragma once
#include "strings.hpp"
#include <filesystem>
#include <fstream>
#include <string_view>

#include <sgrn/Result.hpp>

namespace sgrn::utils::filesystem
{
// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr const char STR_HOME_ALIAS[] = "~";
constexpr const char HOME_PREFIX[] = "~/";
constexpr const char ENV_HOME[] = "HOME";

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

inline std::string expandUserPath(std::string t_path) {
    if (t_path == STR_HOME_ALIAS || t_path.rfind(HOME_PREFIX, 0) == 0) {
        const char* p_home = std::getenv(ENV_HOME);
        if (!p_home || *p_home == '\0') {
            return t_path;
        }
        return (t_path == STR_HOME_ALIAS) ? std::string(p_home) : (std::string(p_home) + t_path.substr(1));
    }
    return t_path;
}

inline std::string normalizePath(std::string t_path) {
    t_path = expandUserPath(sgrn::utils::strings::trim(std::move(t_path)));
    std::error_code ec;
    std::filesystem::path fp(std::move(t_path));
    if (fp.is_relative()) {
        fp = std::filesystem::absolute(fp, ec);
    }
    if (!ec) {
        fp = fp.lexically_normal();
    }
    fp.make_preferred();
    return fp.string();
}

inline bool readStringFromFile(const std::filesystem::path& t_path, std::string& t_content) {
    std::ifstream ifs(t_path);

    SGRN_RETURN_IF(!ifs.is_open(), false);

    t_content.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return true;
}

inline bool writeStringToFile(const std::filesystem::path& t_path, const char* tp_content) {
    std::ofstream ofs(t_path);

    SGRN_RETURN_IF(!ofs.is_open(), false);

    ofs << tp_content;
    return true;
}

inline bool writeStringToFile(const std::filesystem::path& t_path, const std::string& t_content) {
    std::ofstream ofs(t_path);

    SGRN_RETURN_IF(!ofs.is_open(), false);

    ofs << t_content.c_str();
    return true;
}

/// Copy a file only if it doesn't exist or has a different size.
/// Creates parent directories as needed.
///
/// Use case: The `configureSystemd()` bootstrap function uses this to
/// sync systemd unit files from the data directory to `/etc/systemd/system`
/// and nginx configs to the deployment's `etc/nginx/`. By comparing file
/// sizes, it avoids unnecessary writes (and thus unnecessary systemd
/// daemon-reload / service-restart cycles) when the files haven't changed.
///
/// Example:
///   auto result = sgrn::utils::filesystem::copyIfChanged(src, dst);
///   if (result.hasValue())
///       changed_services.push_back(dst.filename().string());
///
/// @return Result<void, std::string> — success if the file was copied or already
///         up-to-date, or an error string if the source file does not exist or the
///         copy operation failed.
inline sgrn::Result<void, std::string> copyIfChanged(const std::filesystem::path& t_src, const std::filesystem::path& t_dst) {
    namespace fs = std::filesystem;
    SGRN_RETURN_ERROR_IF(!fs::exists(t_src), fmt::format("Source file does not exist: {}", t_src.string()));
    bool copy_needed = true;
    if (fs::exists(t_dst)) {
        copy_needed = (fs::file_size(t_src) != fs::file_size(t_dst));
    }
    if (copy_needed) {
        try {
            fs::create_directories(t_dst.parent_path());
            fs::copy_file(t_src, t_dst, fs::copy_options::overwrite_existing);
        } catch (const std::exception& e) {
            return Error(fmt::format("Failed to copy {} to {}: {}", t_src.string(), t_dst.string(), e.what()));
        }
    }
    return {};
}

} // namespace sgrn::utils::filesystem
