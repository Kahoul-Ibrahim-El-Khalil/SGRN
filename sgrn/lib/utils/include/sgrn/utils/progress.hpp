#pragma once
/**
 * @file  sgrn/utils/progress.hpp
 * @brief Interactive terminal progress meter.
 */

#include <fmt/format.h>
#include <sgrn/utils/strings.hpp>
#include <sgrn/utils/time.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

#if !defined(_WIN32)
#include <unistd.h>
#else
#include <io.h>
#define isatty _isatty
#define fileno _fileno
#endif

namespace sgrn::utils
{

/**
 * @brief Real-time terminal progress meter with elapsed time, ETA, and throughput.
 */
class ProgressMeter {
public:
    ProgressMeter(size_t t_total_items, std::uintmax_t t_total_bytes)
        : total_items_(t_total_items)
        , total_bytes_(t_total_bytes)
        , start_time_(std::chrono::steady_clock::now())
        , last_update_(start_time_)
        , is_tty_(::isatty(::fileno(stdout)) != 0) {
    }

    void update(size_t t_current_item, std::uintmax_t t_carried_bytes, bool t_force = false) {
        const auto now = std::chrono::steady_clock::now();
        const auto diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_update_).count();
        if (!t_force && diff_ms < 150) {
            return;
        }
        last_update_ = now;

        const double elapsed_sec = std::chrono::duration<double>(now - start_time_).count();
        const double speed_mb = elapsed_sec > 0.1 ? (static_cast<double>(t_carried_bytes) / (1024.0 * 1024.0)) / elapsed_sec : 0.0;

        std::string eta = "--";
        if (total_bytes_ > 0 && t_carried_bytes > 0 && t_carried_bytes < total_bytes_ && speed_mb > 0.01) {
            const double remain_mb = static_cast<double>(total_bytes_ - t_carried_bytes) / (1024.0 * 1024.0);
            eta = sgrn::utils::time::formatDuration(std::chrono::seconds(static_cast<long long>(remain_mb / speed_mb)));
        } else if (total_items_ > 0 && t_current_item > 0 && t_current_item < total_items_) {
            const double sec_per_item = elapsed_sec / static_cast<double>(t_current_item);
            eta = sgrn::utils::time::formatDuration(
                std::chrono::seconds(static_cast<long long>(sec_per_item * static_cast<double>(total_items_ - t_current_item))));
        }

        const double pct = total_items_ > 0 ? (100.0 * static_cast<double>(t_current_item) / static_cast<double>(total_items_)) : 100.0;
        const std::string elapsed_str =
            sgrn::utils::time::formatDuration(std::chrono::duration_cast<std::chrono::seconds>(now - start_time_));

        if (is_tty_) {
            fmt::print("\r  [{:>4}/{:<4}] {:5.1f}% | {} / {} ({:.1f} MB/s) | elapsed: {} | ETA: {}   ", t_current_item, total_items_, pct,
                sgrn::utils::strings::formatBytes(t_carried_bytes), sgrn::utils::strings::formatBytes(total_bytes_), speed_mb, elapsed_str,
                eta);
            std::fflush(stdout);
        } else if (t_force || diff_ms >= 5000 || t_current_item == total_items_) {
            fmt::print("  progress: {}/{} ({:.1f}%) | {} / {} ({:.1f} MB/s) | elapsed: {} | ETA: {}\n", t_current_item, total_items_, pct,
                sgrn::utils::strings::formatBytes(t_carried_bytes), sgrn::utils::strings::formatBytes(total_bytes_), speed_mb, elapsed_str,
                eta);
            std::fflush(stdout);
        }
    }

    void finish(size_t t_items, std::uintmax_t t_carried_bytes) {
        if (is_tty_) {
            fmt::print("\r                                                                                         \r");
        }
        const auto total_elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start_time_);
        const double total_sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time_).count();
        const double avg_speed = total_sec > 0.1 ? (static_cast<double>(t_carried_bytes) / (1024.0 * 1024.0)) / total_sec : 0.0;
        fmt::print("  completed: {} objects ({}) in {} ({:.1f} MB/s avg)\n", t_items, sgrn::utils::strings::formatBytes(t_carried_bytes),
            sgrn::utils::time::formatDuration(total_elapsed), avg_speed);
    }

private:
    size_t total_items_ = 0;
    std::uintmax_t total_bytes_ = 0;
    std::chrono::steady_clock::time_point start_time_;
    std::chrono::steady_clock::time_point last_update_;
    bool is_tty_ = false;
};

} // namespace sgrn::utils
