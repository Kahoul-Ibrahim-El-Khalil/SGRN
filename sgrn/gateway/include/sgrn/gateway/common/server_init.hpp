#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/debug.hpp>
#include <functional>
#include <memory>
#include <string_view>

namespace sgrn::gateway::common
{

/**
 * @brief Server initialization helpers for protocol adapters
 */
namespace server_init
{

/**
 * @brief Initialize sequence: create → validate → configure → start
 *
 * @param t_creator Function that creates and returns the server object
 * @param t_validator Function that validates created server
 * @param t_configurator Function that configures the server
 * @param t_starter Function that starts the server
 * @param t_context Debug context string (e.g., "EtherNet/IP")
 * @return Success if all stages pass, error on first failure
 */
template <typename ServerT, typename ErrorT>
inline sgrn::Result<std::shared_ptr<ServerT>, ErrorT> initialize(std::function<sgrn::Result<ServerT, ErrorT>()> t_creator,
    std::function<sgrn::Result<void, ErrorT>()> t_validator, std::function<sgrn::Result<void, ErrorT>()> t_configurator,
    std::function<sgrn::Result<void, ErrorT>()> t_starter, std::string_view t_context) {

    // Stage 1: Create
    auto create_res = t_creator();
    if (create_res.hasError()) {
        SGRN_ERROR_LOG("{}: Create failed: {}", t_context, create_res.error());
        return create_res.error();
    }

    auto server = std::make_shared<ServerT>(std::move(create_res.value()));

    // Stage 2: Validate
    if (auto validate_res = t_validator(); validate_res.hasError()) {
        SGRN_ERROR_LOG("{}: Validate failed: {}", t_context, validate_res.error());
        return validate_res.error();
    }

    // Stage 3: Configure
    if (auto config_res = t_configurator(); config_res.hasError()) {
        SGRN_ERROR_LOG("{}: Configure failed: {}", t_context, config_res.error());
        return config_res.error();
    }

    // Stage 4: Start
    if (auto start_res = t_starter(); start_res.hasError()) {
        SGRN_ERROR_LOG("{}: Start failed: {}", t_context, start_res.error());
        return start_res.error();
    }

    SGRN_INFO_LOG("{}: Initialized successfully", t_context);
    return server;
}

/**
 * @brief Simplified initialize with only create and configure
 */
template <typename ServerT, typename ErrorT>
inline sgrn::Result<std::shared_ptr<ServerT>, ErrorT> simpleInit(std::function<sgrn::Result<ServerT, ErrorT>()> t_creator,
    std::function<sgrn::Result<void, ErrorT>()> t_configurator, std::string_view t_context) {

    auto create_res = t_creator();
    if (create_res.hasError()) {
        SGRN_ERROR_LOG("{}: Create failed: {}", t_context, create_res.error());
        return create_res.error();
    }

    auto server = std::make_shared<ServerT>(std::move(create_res.value()));

    if (auto config_res = t_configurator(); config_res.hasError()) {
        SGRN_ERROR_LOG("{}: Configure failed: {}", t_context, config_res.error());
        return config_res.error();
    }

    SGRN_INFO_LOG("{}: Initialized successfully", t_context);
    return server;
}

/**
 * @brief Pipeline pattern: run a sequence of operations, stop at first error
 *
 * @param t_operations Vector of operation functions
 * @param t_context Debug context
 * @return Success if all pass, error from first failure
 */
template <typename ErrorT>
inline sgrn::Result<void, ErrorT> pipeline(
    const std::vector<std::function<sgrn::Result<void, ErrorT>()>>& t_operations, std::string_view t_context) {

    for (size_t i = 0; i < t_operations.size(); ++i) {
        auto res = t_operations[i]();
        if (res.hasError()) {
            SGRN_ERROR_LOG("{}: Pipeline stage {} failed: {}", t_context, i, res.error());
            return res.error();
        }
    }
    return {};
}

/**
 * @brief Cleanup helper: safely destroy resources with error logging
 */
template <typename CleanupFunc>
inline void safeCleanup(CleanupFunc t_cleanup, std::string_view t_context) {
    try {
        t_cleanup();
    } catch (const std::exception& e) {
        SGRN_WARN_LOG("{}: Cleanup failed: {}", t_context, e.what());
    }
}

} // namespace server_init

} // namespace sgrn::gateway::common
