#pragma once

#include <cstdint>
#include <ctime>
#include <optional>

namespace sgrn::s7shell::shell
{

struct PlcSimClock {
    std::optional<int64_t> override_ms_;

    int64_t nowMs() const;
    std::tm nowLocalTm() const;
    void setLocal(int t_year, int t_month, int t_day, int t_hour, int t_minute, int t_second);
    void advanceMs(int64_t t_delta_ms);
    void useWallClock();
};

extern PlcSimClock g_plc_clock;

} // namespace sgrn::s7shell::shell
