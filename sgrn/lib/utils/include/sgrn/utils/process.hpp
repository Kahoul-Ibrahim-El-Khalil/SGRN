#pragma once
/**
 * @file  sgrn/utils/process.hpp
 * @brief Lightweight process execution and system user helpers.
 *
 * Provides safe, clean wrappers for running external commands, capturing
 * output from child processes via pipes, and resolving system users.
 */

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace sgrn::utils::process
{

/**
 * @brief Executes a shell command and captures its standard output.
 *
 * @param t_cmd The shell command to execute.
 * @param t_max_bytes Maximum number of bytes to read before stopping (default: 64 KiB).
 * @return The captured stdout as a string, or empty string on failure.
 */
inline std::string captureOutput(const std::string& t_cmd, size_t t_max_bytes = (1u << 16)) {
    std::string out;
    if (FILE* pipe = ::popen(t_cmd.c_str(), "r")) {
        char buf[512];
        while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
            out += buf;
            if (out.size() >= t_max_bytes) {
                break;
            }
        }
        ::pclose(pipe);
    }
    return out;
}

/**
 * @brief Executes a shell command synchronously and returns the exit status code.
 *
 * @param t_cmd The shell command to execute.
 * @return The integer exit status code returned by system().
 */
inline int execute(const std::string& t_cmd) {
    return std::system(t_cmd.c_str());
}

/**
 * @brief Resolves the username of the user invoking sudo.
 *
 * Checks the SUDO_USER environment variable first. If absent or empty,
 * falls back to looking up the current effective/real UID via getpwuid().
 *
 * @return The username string, or empty string if resolution failed.
 */
inline std::string resolveSudoUser() {
#if !defined(_WIN32)
    const char* p_sudo_user = std::getenv("SUDO_USER");
    if (p_sudo_user != nullptr && *p_sudo_user != '\0') {
        return std::string(p_sudo_user);
    }
    uid_t uid = ::getuid();
    if (struct passwd* p_pw = ::getpwuid(uid)) {
        if (p_pw->pw_name != nullptr) {
            return std::string(p_pw->pw_name);
        }
    }
#endif
    return {};
}

/**
 * @brief Returns the local hostname of the machine.
 */
inline std::string machineName() {
    char h[256] = {};
    if (::gethostname(h, sizeof(h) - 1) == 0) {
        return h;
    }
    return "localhost";
}

} // namespace sgrn::utils::process
