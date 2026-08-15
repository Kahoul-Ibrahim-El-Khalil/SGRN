#include <sgrn/s7shell/utils/PlcSimClock.hpp>
#include <sgrn/utils/time.hpp>

namespace sgrn::s7shell::shell
{

PlcSimClock g_plc_clock;

int64_t PlcSimClock::nowMs() const {
    return override_ms_ ? *override_ms_ : sgrn::utils::time::nowMilliseconds();
}

std::tm PlcSimClock::nowLocalTm() const {
    const time_t secs = static_cast<time_t>(nowMs() / 1000);
    std::tm ti{};
#ifdef _WIN32
    localtime_s(&ti, &secs);
#else
    localtime_r(&secs, &ti);
#endif
    return ti;
}

void PlcSimClock::setLocal(int t_year, int t_month, int t_day, int t_hour, int t_minute, int t_second) {
    std::tm ti{};
    ti.tm_year = t_year - 1900;
    ti.tm_mon = t_month - 1;
    ti.tm_mday = t_day;
    ti.tm_hour = t_hour;
    ti.tm_min = t_minute;
    ti.tm_sec = t_second;
    ti.tm_isdst = -1;
    override_ms_ = static_cast<int64_t>(mktime(&ti)) * 1000;
}

void PlcSimClock::advanceMs(int64_t t_delta_ms) {
    if (!override_ms_)
        override_ms_ = sgrn::utils::time::nowMilliseconds();
    *override_ms_ += t_delta_ms;
}

void PlcSimClock::useWallClock() {
    override_ms_.reset();
}

} // namespace sgrn::s7shell::shell
