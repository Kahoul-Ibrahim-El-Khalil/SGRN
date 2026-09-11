#pragma once

#include <sgrn/common/MemoryPort.hpp>
#include <sgrn/common/SecurityPort.hpp>
#include <atomic>
#include <memory>
#include <thread>

namespace sgrn::common
{

/**
 * @brief CRTP base class for all protocol adapters.
 *
 * Provides common lifecycle management without virtual dispatch.
 * Derived classes implement serveLoop() and optionally configure().
 * Memory and authorization arrive as abstract ports (dependency inversion:
 * production wires the twin-backed TwinMemoryPort/GatewaySecurityPolicy,
 * tests wire fakes), so derived adapters never include twin or security
 * headers.
 *
 * @tparam Derived The concrete adapter type (CRTP pattern)
 */
template <typename Derived>
class AdapterBase {
public:
    AdapterBase(IMemoryPort& t_memory, std::shared_ptr<ISecurityPolicy> t_security)
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
    IMemoryPort& getMemory() {
        return memory_;
    }
    const IMemoryPort& getMemory() const {
        return memory_;
    }
    std::shared_ptr<ISecurityPolicy> getSecurityManager() {
        return security_;
    }
    std::atomic<bool>& runningFlag() {
        return running_;
    }

private:
    IMemoryPort& memory_;
    std::shared_ptr<ISecurityPolicy> security_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace sgrn::common
