#pragma once

#include <chrono>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace sgrn::utils::time
{

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr const char fmt_iso_timestamp[] = "%Y%m%dT%H%M%SZ";
constexpr const char fmt_iso8601_timestamp[] = "%Y-%m-%dT%H:%M:%SZ";

/**
 * @brief Returns the current UTC time in milliseconds since epoch.
 */
inline int64_t nowMilliseconds() {
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

/**
 * @brief Returns the current UTC time in a compact ISO-like format (YYYYMMDDTHHMMSSZ).
 */
/**
 * @brief Returns the provided UTC time (in milliseconds) in a compact ISO-like format (YYYYMMDDTHH:MM:SS:msZ).
 */
inline std::string isoTimestamp(int64_t t_ms) {
    auto tp = std::chrono::time_point<std::chrono::system_clock>(std::chrono::milliseconds(t_ms));
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y%m%dT%H:%M:%S");
    ss << ":" << std::setfill('0') << std::setw(3) << (t_ms % 1000) << "Z";
    return ss.str();
}

/**
 * @brief Returns the current UTC time in a compact ISO-like format (YYYYMMDDTHHMMSSZ).
 */
inline std::string isoTimestamp() {
    return isoTimestamp(nowMilliseconds());
}

/**
 * @brief Returns the provided UTC time (in milliseconds) in standard ISO 8601 format (YYYY-MM-DDTHH:MM:SSZ).
 */
inline std::string iso8601Timestamp(int64_t t_ms) {
    auto tp = std::chrono::time_point<std::chrono::system_clock>(std::chrono::milliseconds(t_ms));
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, fmt_iso8601_timestamp);
    return ss.str();
}

inline std::string iso8601Timestamp() {
    return iso8601Timestamp(static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
}

/**
 * @brief Parses a string in "YYYY-MM-DD HH:MM:SS" format into a std::tm struct.
 */
inline std::optional<std::tm> parseDateTimeInput(const std::string& t_val) {
    std::tm tm{};
    if (t_val.size() < 19) {
        return std::nullopt;
    }
    // Using sscanf for simple pattern matching, similar to the original implementation
    if (std::sscanf(t_val.c_str(), "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) < 6) {
        return std::nullopt;
    }
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return tm;
}

/**
 * @brief Formats a std::tm struct into a "YYYY-MM-DD HH:MM:SS" string.
 */
inline std::string formatDateTime(const std::tm& t_t) {
    std::ostringstream ss;
    ss << std::setfill('0') << std::setw(4) << (t_t.tm_year + 1900) << "-" << std::setw(2) << (t_t.tm_mon + 1) << "-" << std::setw(2)
       << t_t.tm_mday << " " << std::setw(2) << t_t.tm_hour << ":" << std::setw(2) << t_t.tm_min << ":" << std::setw(2) << t_t.tm_sec;
    return ss.str();
}

/**
 * @brief Gets the current local time as a std::tm struct.
 */
inline std::tm getLocalTime() {
    std::time_t now = std::time(nullptr);
    std::tm res{};
#ifdef _WIN32
    localtime_s(&res, &now);
#else
    localtime_r(&now, &res);
#endif
    return res;
}

/**
 * @brief Returns the date part (YYYY-MM-DD) for a given time in milliseconds.
 */
inline std::string datePath(int64_t t_ms) {
    auto tp = std::chrono::time_point<std::chrono::system_clock>(std::chrono::milliseconds(t_ms));
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d");
    return ss.str();
}

/**
 * @brief Returns the time part (HH:MM:SS:ms) for a given time in milliseconds.
 */
inline std::string timePath(int64_t t_ms) {
    auto tp = std::chrono::time_point<std::chrono::system_clock>(std::chrono::milliseconds(t_ms));
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%H:%M:%S");
    ss << ":" << std::setfill('0') << std::setw(3) << (t_ms % 1000);
    return ss.str();
}

/**
 * @brief Formats a given time in milliseconds with a custom format string.
 */
inline std::string formatTimestamp(int64_t t_ms, const std::string& t_fmt) {
    auto tp = std::chrono::time_point<std::chrono::system_clock>(std::chrono::milliseconds(t_ms));
    auto tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, t_fmt.c_str());
    return ss.str();
}

} // namespace sgrn::utils::time
