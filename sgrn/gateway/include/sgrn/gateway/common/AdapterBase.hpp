#pragma once

#include <sgrn/gateway/security/SecurityManager.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <atomic>
#include <thread>

namespace sgrn::gateway::common
{

/**
 * @brief CRTP base class for all protocol adapters.
 *
 * Provides common lifecycle management without virtual dispatch.
 * Derived classes implement serveLoop() and optionally configure().
 *
 * @tparam Derived The concrete adapter type (CRTP pattern)
 */
template <typename Derived>
class AdapterBase {
public:
    AdapterBase(twin::PlcMemory& t_memory, std::shared_ptr<::sgrn::gateway::SecurityManager> t_security)
        : memory_(t_memory)
        , security_(std::move(t_security)) {
    }

    ~AdapterBase() {
        stop();
    }

    // Non-copyable, non-movable
    AdapterBase(const AdapterBase&) = delete;
    AdapterBase& operator=(const AdapterBase&) = delete;
    AdapterBase(AdapterBase&&) = delete;
    AdapterBase& operator=(AdapterBase&&) = delete;

    /**
     * @brief Start the adapter (launches serve loop in background thread)
     */
    sgrn::Result<void, std::string_view> start(const std::string& t_ip, uint16_t t_port) {
        if (running_.exchange(true)) {
            return "Adapter already running";
        }

        // Call derived class pre-start configuration
        if (!static_cast<Derived*>(this)->configure(t_ip, t_port)) {
            running_.store(false);
            return "Adapter configuration failed";
        }

        // Launch serve loop in background thread
        thread_ = std::thread([this]() { static_cast<Derived*>(this)->serveLoop(); });

        return {};
    }

    /**
     * @brief Stop the adapter and join the serve thread
     */
    void stop() {
        if (!running_.exchange(false)) {
            return;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    bool isRunning() const {
        return running_.load();
    }

protected:
    // Accessors for derived classes
    twin::PlcMemory& getMemory() {
        return memory_;
    }
    const twin::PlcMemory& getMemory() const {
        return memory_;
    }
    std::shared_ptr<::sgrn::gateway::SecurityManager> getSecurityManager() {
        return security_;
    }
    std::atomic<bool>& runningFlag() {
        return running_;
    }

private:
    twin::PlcMemory& memory_;
    std::shared_ptr<::sgrn::gateway::SecurityManager> security_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace sgrn::gateway::common
