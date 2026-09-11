#include <regex>
#pragma once

#include <chrono>
#include <iostream>
#include <string>
#include <string_view>

#include <fmt/chrono.h>
#include <fmt/color.h>
#include <fmt/format.h>

#if __has_include(<drogon/HttpRequest.h>) && !defined(SGRN_NO_DROGON)
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#define SGRN_HAS_DROGON
#endif

#ifdef SGRN_HAS_DROGON

template <>
struct fmt::formatter<drogon::HttpRequestPtr> : formatter<std::string_view> {
    auto t_format(const drogon::HttpRequestPtr& t_req, format_context& t_ctx) const {
        if (!t_req) {
            return formatter<std::string_view>::format("HttpRequestPtr(null)", t_ctx);
        }

        return formatter<std::string_view>::format(
            fmt::format("HttpRequestPtr(method={}, path={})", t_req->methodString(), t_req->path()), t_ctx);
    }
};

template <>
struct fmt::formatter<drogon::HttpResponsePtr> : formatter<std::string_view> {
    auto t_format(const drogon::HttpResponsePtr& t_resp, format_context& t_ctx) const {
        if (!t_resp) {
            return formatter<std::string_view>::format("HttpResponsePtr(null)", t_ctx);
        }

        return formatter<std::string_view>::format(
            fmt::format("HttpResponsePtr(status={})", static_cast<int>(t_resp->getStatusCode())), t_ctx);
    }
};

template <>
struct fmt::formatter<drogon::HttpStatusCode> : formatter<int> {
    auto t_format(drogon::HttpStatusCode t_code, format_context& t_ctx) const {
        return formatter<int>::format(static_cast<int>(t_code), t_ctx);
    }
};
#endif

namespace sgrn
{

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
constexpr const char log_debug_str[] = "DEBUG";
constexpr const char log_info_str[] = "INFO";
constexpr const char log_warn_str[] = "WARN";
constexpr const char log_error_str[] = "ERROR";
constexpr const char log_unknown_str[] = "UNKNOWN";
constexpr const char log_format[] = "[{:%Y-%m-%d %H:%M:%S}] [{}] [{}] {}\n";
constexpr const char default_component[] = "App";
constexpr const char s7_poller_component[] = "S7Poller";

enum class LogLevel { LEVEL_DEBUG = 0, LEVEL_INFO = 1, LEVEL_WARN = 2, LEVEL_ERROR = 3 };

#ifndef SGRN_MIN_LOG_LEVEL
#ifdef NDEBUG
#define SGRN_MIN_LOG_LEVEL ::sgrn::LogLevel::LEVEL_DEBUG
#else
#define SGRN_MIN_LOG_LEVEL ::sgrn::LogLevel::LEVEL_DEBUG
#endif
#endif

inline constexpr std::string_view to_string(LogLevel t_level) {
    switch (t_level) {
        case LogLevel::LEVEL_DEBUG:
            return log_debug_str;
        case LogLevel::LEVEL_INFO:
            return log_info_str;
        case LogLevel::LEVEL_WARN:
            return log_warn_str;
        case LogLevel::LEVEL_ERROR:
            return log_error_str;
        default:
            return log_unknown_str;
    }
}

inline void log_base(LogLevel t_level, fmt::color t_color, std::string_view t_component, std::string_view t_message) {
    if (static_cast<int>(t_level) < static_cast<int>(SGRN_MIN_LOG_LEVEL)) {
        return;
    }

#ifdef SGRN_USE_DROGON_LOG
    if (t_level == LogLevel::LEVEL_INFO) {
        LOG_INFO << "[" << t_component << "] " << t_message;
    } else if (t_level == LogLevel::LEVEL_DEBUG) {
        LOG_DEBUG << "[" << t_component << "] " << t_message;
    } else if (t_level == LogLevel::LEVEL_ERROR) {
        LOG_ERROR << "[" << t_component << "] " << t_message;
    } else if (t_level == LogLevel::LEVEL_WARN) {
        LOG_WARN << "[" << t_component << "] " << t_message;
    }
#else
    const auto now = std::chrono::system_clock::now();
    std::cout << fmt::format(fg(t_color), log_format, now, to_string(t_level), t_component, t_message) << std::flush;
#endif
}

template <typename... Args>
inline void log(
    LogLevel t_level, fmt::color t_color, std::string_view t_component, fmt::format_string<Args...> t_format, Args&&... t_args) {
    log_base(t_level, t_color, t_component, fmt::format(t_format, std::forward<Args>(t_args)...));
}

// Simple non-template overload for the common case
inline void log_runtime(LogLevel t_level, fmt::color t_color, std::string_view t_component, std::string_view t_format) {
    log_base(t_level, t_color, t_component, t_format);
}

template <typename... Args>
inline void log_runtime(LogLevel t_level, fmt::color t_color, std::string_view t_component, std::string_view t_format, Args&&... t_args) {
    if constexpr (sizeof...(Args) > 0) {
        log_base(t_level, t_color, t_component, fmt::format(fmt::runtime(t_format), std::forward<Args>(t_args)...));
    } else {
        log_base(t_level, t_color, t_component, t_format);
    }
}

} // namespace sgrn

#define SGRN_LOG(level, color, comp, msg, ...) sgrn::log_runtime(level, color, comp, msg __VA_OPT__(, ) __VA_ARGS__)

#define SGRN_INFO(comp, msg, ...) SGRN_LOG(::sgrn::LogLevel::LEVEL_INFO, fmt::color::white, comp, msg __VA_OPT__(, ) __VA_ARGS__)
#define SGRN_DEBUG(comp, msg, ...) SGRN_LOG(::sgrn::LogLevel::LEVEL_DEBUG, fmt::color::yellow, comp, msg __VA_OPT__(, ) __VA_ARGS__)
#define SGRN_ERROR(comp, msg, ...) SGRN_LOG(::sgrn::LogLevel::LEVEL_ERROR, fmt::color::red, comp, msg __VA_OPT__(, ) __VA_ARGS__)
#define SGRN_WARN(comp, msg, ...) SGRN_LOG(::sgrn::LogLevel::LEVEL_WARN, fmt::color::orange, comp, msg __VA_OPT__(, ) __VA_ARGS__)

#define SGRN_INFO_LOG(msg, ...) SGRN_INFO(::sgrn::default_component, msg __VA_OPT__(, ) __VA_ARGS__)
#define SGRN_DEBUG_LOG(msg, ...) SGRN_DEBUG(::sgrn::default_component, msg __VA_OPT__(, ) __VA_ARGS__)
#define SGRN_ERROR_LOG(msg, ...) SGRN_ERROR(::sgrn::default_component, msg __VA_OPT__(, ) __VA_ARGS__)
#define SGRN_WARN_LOG(msg, ...) SGRN_WARN(::sgrn::default_component, msg __VA_OPT__(, ) __VA_ARGS__)

#ifdef DEBUG_S7CORE
#define S7_READER_LOG(msg, ...)                                                                                                            \
    SGRN_LOG(::sgrn::LogLevel::LEVEL_DEBUG, fmt::color::cyan, ::sgrn::S7_POLLER_COMPONENT, msg __VA_OPT__(, ) __VA_ARGS__)
#define S7_TREE_LOG(msg, ...)                                                                                                              \
    SGRN_LOG(::sgrn::LogLevel::LEVEL_DEBUG, fmt::color::green, ::sgrn::S7_POLLER_COMPONENT, msg __VA_OPT__(, ) __VA_ARGS__)
#define S7_DELTA_LOG(comp, msg, ...) SGRN_LOG(::sgrn::LogLevel::LEVEL_DEBUG, fmt::color::yellow, comp, msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define S7_READER_LOG(...) ((void)0)
#define S7_TREE_LOG(...) ((void)0)
#define S7_DELTA_LOG(...) ((void)0)
#endif
