#pragma once

#include <sgrn/gateway/wrappers/s7/S7Server.hpp>
#include <sgrn/gateway/wrappers/s7/error.hpp>
#include <sgrn/scl/types.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <snap7.h>
#include <string>
#include <utility>

namespace sgrn::gateway::wrappers::s7
{
using ::sgrn::gateway::wrappers::s7::S7Error;
using ::sgrn::scl::S7ServerEvent;

/**
 * @brief RAII wrapper around the Snap7 `TS7Server` API.
 *
 * This class owns the Snap7 server handle, lifecycle, and event queue access.
 * Semantic registry loading and access control live in `SemanticAwareS7Server`.
 */
class S7Server {
public:
    S7Server();
    virtual ~S7Server();

    S7Server(const S7Server&) = delete;
    S7Server& operator=(const S7Server&) = delete;
    S7Server(S7Server&& t_other) noexcept;
    S7Server& operator=(S7Server&& t_other) noexcept;

    // Lifecycle ---------------------------------------------------------------

    /**
     * @brief Start the server listening on the specified IP.
     * @param t_ip The IP address to bind to (default "0.0.0.0").
     * @return sgrn::Result<void, ::sgrn::gateway::wrappers::s7::S7Error> Success or error.
     */
    sgrn::Result<void, ::sgrn::gateway::wrappers::s7::S7Error> start(const std::string& t_ip = "0.0.0.0", uint16_t t_port = 102);

    /**
     * @brief Stop the server.
     */
    sgrn::Result<void, ::sgrn::gateway::wrappers::s7::S7Error> stop();

    /**
     * @brief Get the numeric status of the server.
     */
    int statusStatus() const;

    /**
     * @brief Check if the server is currently running.
     */
    bool isRunning() const;

    // Properties and Events ---------------------------------------------------

    int clientsCount() const;
    int getCpuStatus() const;
    sgrn::Result<void, S7Error> setCpuStatus(int t_status);

    /**
     * @brief Set a callback for server events.
     */
    void setEventsCallback(pfn_SrvCallBack t_callback, void* tp_usr_ptr);

    /**
     * @brief Pick the next pending event from the server's event queue.
     * @return std::optional<S7ServerEvent> The event, or nullopt if queue is empty.
     */
    std::optional<S7ServerEvent> pickEvent();

    /**
     * @brief Get the human-readable text for a server event.
     */
    std::string eventText(const S7ServerEvent& t_event) const;

protected:
    sgrn::Result<void, ::sgrn::gateway::wrappers::s7::S7Error> makeStatus(int t_error_code) const;
    static std::pair<int, word> areaKey(int t_area_code, word t_index);

    /**
     * @brief Hook invoked after parameter setup and before `StartTo`.
     *
     * Derived classes use this to install Snap7 callbacks.
     */
    virtual void configureBeforeStart() {
    }

    std::unique_ptr<TS7Server> server_;
    mutable std::recursive_mutex mutex_;
};

} // namespace sgrn::gateway::wrappers::s7
