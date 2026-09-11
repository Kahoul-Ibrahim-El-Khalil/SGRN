#include <sgrn/gateway/twin/time_utils.hpp>
#include <sgrn/utils/strings.hpp>
#include <cstdio>

namespace sgrn::gateway::twin
{
constexpr const char fmt_s7_time[] = "%u:%u:%u.%u";
constexpr const char fmt_s7_time_alt[] = "%u:%u:%u:%u";

std::optional<int64_t> parseS7Time(const std::string& t_raw) {
    std::string s = sgrn::utils::strings::trim(t_raw);
    if (s.empty())
        return std::nullopt;

    bool negative = false;
    if (s[0] == '-') {
        negative = true;
        s = s.substr(1);
    }

    // Handle #T prefix
    if (s.size() > 2 && s[0] == '#' && (s[1] == 'T' || s[1] == 't')) {
        s = s.substr(2);
    }

    unsigned h = 0, m = 0, sec = 0, ms = 0;
    // Try HH:MM:SS.mmm
    if (std::sscanf(s.c_str(), fmt_s7_time, &h, &m, &sec, &ms) >= 3) {
        // success
    } else if (std::sscanf(s.c_str(), fmt_s7_time_alt, &h, &m, &sec, &ms) >= 3) {
        // success with colon for ms (some TIA formats)
    } else {
        return std::nullopt;
    }

    const int64_t abs_ms =
        static_cast<int64_t>(h) * 3600000 + static_cast<int64_t>(m) * 60000 + static_cast<int64_t>(sec) * 1000 + static_cast<int64_t>(ms);
    return negative ? -abs_ms : abs_ms;
}
} // namespace sgrn::gateway::twin
