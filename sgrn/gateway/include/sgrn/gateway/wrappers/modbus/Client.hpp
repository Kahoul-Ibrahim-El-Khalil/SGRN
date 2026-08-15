#pragma once

#include <sgrn/Result.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

struct _modbus;
typedef struct _modbus modbus_t;

namespace sgrn::gateway::wrappers::modbus
{

struct ClientConfig {
    std::string host;
    uint16_t port{502};
    int slave_id{1};
    uint32_t response_timeout_ms{2000};
};

/// RAII move-only wrapper over libmodbus TCP client (modbus_t).
///
/// THREAD SAFETY: individual read/write methods take an internal lock.
/// Use ioMutex() + raw() only when a single lock must span multiple calls
/// (adapter layer).
class Client {
public:
    ~Client() noexcept;

    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    /// Create context, apply config, and connect. Returns owning Client on success.
    static sgrn::Result<Client> connect(ClientConfig t_cfg);

    /// Shared ownership helper — context created, connect deferred to ensureConnected().
    static sgrn::Result<std::shared_ptr<Client>> createShared(ClientConfig t_cfg);

    /// Shared ownership helper — create, connect, and return on success.
    static sgrn::Result<std::shared_ptr<Client>> connectShared(ClientConfig t_cfg);

    void disconnect() noexcept;

    bool isConnected() const noexcept {
        return connected_.load(std::memory_order_acquire);
    }

    /// Lazily (re)connect — idempotent when already connected.
    sgrn::Result<void> ensureConnected();

    sgrn::Result<int> readCoils(int t_addr, int t_count, uint8_t* tp_dest);
    sgrn::Result<int> readDiscreteInputs(int t_addr, int t_count, uint8_t* tp_dest);
    sgrn::Result<int> readHoldingRegisters(int t_addr, int t_count, uint16_t* tp_dest);
    sgrn::Result<int> readInputRegisters(int t_addr, int t_count, uint16_t* tp_dest);

    sgrn::Result<void> writeCoil(int t_addr, bool t_value);
    sgrn::Result<void> writeRegister(int t_addr, uint16_t t_value);
    sgrn::Result<int> writeCoils(int t_addr, int t_count, const uint8_t* tp_src);
    sgrn::Result<int> writeRegisters(int t_addr, int t_count, const uint16_t* tp_src);

    std::mutex& ioMutex() noexcept {
        return mutex_;
    }

    /// INTERNAL — adapter layer only.
    modbus_t* raw() noexcept {
        return ctx_;
    }

private:
    explicit Client(modbus_t* tp_ctx, ClientConfig t_cfg);

    sgrn::Result<void> connectLocked();
    static sgrn::Result<modbus_t*> createContext(const ClientConfig& t_cfg);

    modbus_t* ctx_{nullptr};
    ClientConfig cfg_;
    std::atomic<bool> connected_{false};
    std::mutex mutex_;
};

} // namespace sgrn::gateway::wrappers::modbus
