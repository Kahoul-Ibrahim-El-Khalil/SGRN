#pragma once

#include <sgrn/debug.hpp>
#include <csignal>
#include <functional>
#include <string_view>

namespace sgrn::utils::app
{

/**
 * @brief Setup common system signals (SIGINT, SIGTERM) to trigger a callback.
 *
 * @param t_handler The callback to execute when a signal is received.
 */
inline void setupSignals(std::function<void(int)> t_handler) {
    static std::function<void(int)> s_handler;
    s_handler = std::move(t_handler);

    auto signal_handler = [](int t_signum) {
        if (s_handler) {
            s_handler(t_signum);
        }
    };

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
}

/**
 * @brief Template for the main function wrapper to handle global exceptions and logging.
 *
 * @tparam Func The main logic function.
 * @param t_argc Argument count.
 * @param t_argv Argument values.
 * @param t_func The function containing the app logic.
 * @param t_app_name The name of the application for logging.
 * @return int Exit code.
 */
template <typename Func>
int runMain(int t_argc, char** tp_argv, Func&& t_func, std::string_view t_app_name = "SGRN App") {
    try {
        return t_func(t_argc, tp_argv);
    } catch (const std::exception& ex) {
        SGRN_ERROR(t_app_name, "Initialization failed: {}", ex.what());
        return EXIT_FAILURE;
    } catch (...) {
        SGRN_ERROR(t_app_name, "Unknown error occurred during initialization.");
        return EXIT_FAILURE;
    }
}

} // namespace sgrn::utils::app
