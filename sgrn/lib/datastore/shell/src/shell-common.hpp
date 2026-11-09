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
