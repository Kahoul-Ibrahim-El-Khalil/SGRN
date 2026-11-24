#pragma once
// shell-common.hpp — shared includes for the datastore shell translation units.

#include <sgrn/datastore/shell/DatastoreShell.hpp>

#include <fmt/color.h>
#include <fmt/core.h>
#include <json/json.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

#include "render-kit.hpp"

namespace sgrn::datastore::shell
{

// ── option parsing shared by the commands that take --flag arguments ─────────

/// Value following `t_flag` (`--flag VALUE`), or `t_dflt` when absent.
inline std::string shellFlag(const std::vector<std::string>& t_args, const std::string& t_flag, const std::string& t_dflt = "") {
    for (size_t i = 0; i + 1 < t_args.size(); ++i) {
        if (t_args[i] == t_flag)
            return t_args[i + 1];
    }
    return t_dflt;
}

inline bool shellHasFlag(const std::vector<std::string>& t_args, const std::string& t_flag) {
    return std::find(t_args.begin(), t_args.end(), t_flag) != t_args.end();
}

inline uint32_t shellUint(const std::vector<std::string>& t_args, const std::string& t_flag, uint32_t t_dflt) {
    const std::string raw = shellFlag(t_args, t_flag);
    if (raw.empty())
        return t_dflt;
    try {
        return static_cast<uint32_t>(std::stoul(raw));
    } catch (const std::exception&) {
        return t_dflt;
    }
}

/// Arguments that are neither flags (leading '-') nor the value of one of
/// `t_value_flags`, in order.
inline std::vector<std::string> shellPositionals(
    const std::vector<std::string>& t_args, const std::vector<std::string_view>& t_value_flags = {}) {
    std::vector<std::string> out;
    for (size_t i = 0; i < t_args.size(); ++i) {
        const std::string& arg = t_args[i];
        if (!arg.empty() && arg[0] == '-') {
            if (std::find(t_value_flags.begin(), t_value_flags.end(), arg) != t_value_flags.end())
                ++i; // skip the flag's value
            continue;
        }
        out.push_back(arg);
    }
    return out;
}

/// Single reporting point for command results: commands return errors, the
/// dispatcher (or an AngelScript wrapper) decides how to surface them.
inline int reportResult(std::string_view t_command, const sgrn::Result<void>& t_result) {
    if (t_result.hasError()) {
        fmt::print(stderr, "{}: {}\n", t_command, t_result.error());
        return 1;
    }
    return 0;
}

} // namespace sgrn::datastore::shell
