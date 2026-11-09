#pragma once
#include "strings.hpp"
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>
#if !defined(_WIN32)
#include <unistd.h>
#endif

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

    RETURN_IF(!ifs.is_open(), false);

    t_content.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return true;
}

inline bool writeStringToFile(const std::filesystem::path& t_path, const char* tp_content) {
    std::ofstream ofs(t_path);

    RETURN_IF(!ofs.is_open(), false);

    ofs << tp_content;
    return true;
}

inline bool writeStringToFile(const std::filesystem::path& t_path, const std::string& t_content) {
    std::ofstream ofs(t_path);

    RETURN_IF(!ofs.is_open(), false);

    ofs << t_content.c_str();
    return true;
}

inline bool readBinaryFile(const std::filesystem::path& t_path, std::vector<uint8_t>& t_data) {
    std::ifstream ifs(t_path, std::ios::binary);
    RETURN_IF(!ifs.is_open(), false);
    t_data.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return true;
}

inline bool writeBinaryFile(const std::filesystem::path& t_path, const uint8_t* tp_data, size_t t_size) {
    std::ofstream ofs(t_path, std::ios::binary);
    RETURN_IF(!ofs.is_open(), false);
    if (t_size > 0 && tp_data != nullptr) {
        ofs.write(reinterpret_cast<const char*>(tp_data), static_cast<std::streamsize>(t_size));
    }
    return ofs.good();
}

inline bool writeBinaryFile(const std::filesystem::path& t_path, const std::vector<uint8_t>& t_data) {
    return writeBinaryFile(t_path, t_data.data(), t_data.size());
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
    RETURN_ERROR_IF(!fs::exists(t_src), fmt::format("Source file does not exist: {}", t_src.string()));
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

/// Whole-file byte comparison; returns false when either file is unreadable.
inline bool fileContentsEqual(const std::filesystem::path& t_a, const std::filesystem::path& t_b) {
    std::ifstream fa(t_a, std::ios::binary);
    std::ifstream fb(t_b, std::ios::binary);
    if (!fa || !fb)
        return false;
    return std::equal(std::istreambuf_iterator<char>(fa), std::istreambuf_iterator<char>(), std::istreambuf_iterator<char>(fb));
}

/// Read a file's full text; returns an empty string when the file is unreadable.
inline std::string readTextFile(const std::filesystem::path& t_path) {
    std::ifstream in(t_path);
    if (!in)
        return {};
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// Returns the directory containing the currently running binary via /proc/self/exe.
/// Returns an empty string when it cannot be resolved (non-Linux, confined environments).
inline std::string currentExeDir() {
    char exe_buf[4096] = {};
    ssize_t exe_len = ::readlink("/proc/self/exe", exe_buf, sizeof(exe_buf) - 1);
    if (exe_len <= 0)
        return {};
    return std::filesystem::path(std::string(exe_buf, static_cast<size_t>(exe_len))).parent_path().string();
}

/// Shell-like lookup of t_program inside a ':'-separated directory list.
/// Returns the full path of the first executable found, or empty string when absent.
/// @example whichInPaths("/usr/bin:/usr/local/bin", "nginx")
inline std::string whichInPaths(const std::string& t_dirs, const std::string& t_program) {
    size_t pos = 0;
    while (pos <= t_dirs.size()) {
        size_t sep = t_dirs.find(':', pos);
        std::string dir = t_dirs.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos);
        if (!dir.empty()) {
            std::filesystem::path candidate = std::filesystem::path(dir) / t_program;
            std::error_code ec;
            auto st = std::filesystem::status(candidate, ec);
            if (!ec && std::filesystem::is_regular_file(st) &&
                (st.permissions() & std::filesystem::perms::owner_exec) != std::filesystem::perms::none)
                return candidate.string();
        }
        if (sep == std::string::npos)
            break;
        pos = sep + 1;
    }
    return {};
}

/// Restrict a file to owner rw permissions (0600).
inline bool ensureOwnerOnlyPerms(const std::filesystem::path& t_path) {
    std::error_code ec;
    std::filesystem::permissions(
        t_path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write, std::filesystem::perm_options::replace, ec);
    return !ec;
}

/// Find the newest file in a directory matching a given suffix (e.g. ".pg", ".tar").
inline std::filesystem::path findLatestFile(const std::filesystem::path& t_dir, std::string_view t_suffix = "") {
    std::error_code ec;
    if (!std::filesystem::exists(t_dir, ec))
        return {};
    std::filesystem::path latest;
    std::filesystem::file_time_type latest_time{};
    for (const auto& entry : std::filesystem::directory_iterator(t_dir, ec)) {
        if (!entry.is_regular_file(ec))
            continue;
        const std::string name = entry.path().filename().string();
        if (!t_suffix.empty() && !name.ends_with(t_suffix))
            continue;
        const auto mtime = entry.last_write_time(ec);
        if (latest.empty() || mtime > latest_time) {
            latest = entry.path();
            latest_time = mtime;
        }
    }
    return latest;
}

} // namespace sgrn::utils::filesystem
