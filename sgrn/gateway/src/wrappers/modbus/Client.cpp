#include <sgrn/gateway/wrappers/modbus/Client.hpp>

#include <modbus.h>

#include <fmt/core.h>

#include <cerrno>
#include <utility>

namespace sgrn::gateway::wrappers::modbus
{

namespace
{

sgrn::Result<void> checkRc(int t_rc, std::string_view t_op) {
    if (t_rc == -1)
        return fmt::format("{}: {}", t_op, ::modbus_strerror(errno));
    return {};
}

} // namespace

Client::Client(modbus_t* tp_ctx, ClientConfig t_cfg)
    : ctx_(tp_ctx)
    , cfg_(std::move(t_cfg)) {
}

Client::~Client() noexcept {
    std::lock_guard lock(mutex_);
    if (ctx_) {
        if (connected_.load(std::memory_order_relaxed))
            ::modbus_close(ctx_);
        ::modbus_free(ctx_);
        ctx_ = nullptr;
    }
    connected_.store(false, std::memory_order_relaxed);
}

Client::Client(Client&& t_other) noexcept
    : ctx_(std::exchange(t_other.ctx_, nullptr))
    , cfg_(std::move(t_other.cfg_))
    , connected_(t_other.connected_.load(std::memory_order_relaxed))
    , mutex_() {
    t_other.connected_.store(false, std::memory_order_relaxed);
}

Client& Client::operator=(Client&& t_other) noexcept {
    if (this != &t_other) {
        if (ctx_) {
            if (connected_.load(std::memory_order_relaxed))
                ::modbus_close(ctx_);
            ::modbus_free(ctx_);
            ctx_ = nullptr;
        }
        connected_.store(false, std::memory_order_relaxed);
        ctx_ = std::exchange(t_other.ctx_, nullptr);
        cfg_ = std::move(t_other.cfg_);
        connected_.store(t_other.connected_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        t_other.connected_.store(false, std::memory_order_relaxed);
    }
    return *this;
}

sgrn::Result<modbus_t*> Client::createContext(const ClientConfig& t_cfg) {
    modbus_t* p_ctx = ::modbus_new_tcp(t_cfg.host.c_str(), static_cast<int>(t_cfg.port));
    if (!p_ctx)
        return fmt::format("modbus_new_tcp({}:{}) failed: {}", t_cfg.host, t_cfg.port, ::modbus_strerror(errno));

    if (::modbus_set_slave(p_ctx, t_cfg.slave_id) == -1) {
        const int err = errno;
        ::modbus_free(p_ctx);
        return fmt::format("modbus_set_slave({}) failed: {}", t_cfg.slave_id, ::modbus_strerror(err));
    }

    const uint32_t sec = t_cfg.response_timeout_ms / 1000U;
    const uint32_t usec = (t_cfg.response_timeout_ms % 1000U) * 1000U;
    if (::modbus_set_response_timeout(p_ctx, static_cast<uint32_t>(sec), usec) == -1) {
        const int err = errno;
        ::modbus_free(p_ctx);
        return fmt::format("modbus_set_response_timeout failed: {}", ::modbus_strerror(err));
    }

    return p_ctx;
}

sgrn::Result<Client> Client::connect(ClientConfig t_cfg) {
    auto ctx_res = createContext(t_cfg);
    if (ctx_res.hasError())
        return sgrn::Result<Client>::Error(ctx_res.error());

    Client client(ctx_res.value(), std::move(t_cfg));
    if (auto conn = client.connectLocked(); conn.hasError())
        return sgrn::Result<Client>::Error(conn.error());
    return client;
}

sgrn::Result<std::shared_ptr<Client>> Client::createShared(ClientConfig t_cfg) {
    auto ctx_res = createContext(t_cfg);
    if (ctx_res.hasError())
        return sgrn::Result<std::shared_ptr<Client>>::Error(ctx_res.error());
    return std::shared_ptr<Client>(new Client(ctx_res.value(), std::move(t_cfg)));
}

sgrn::Result<std::shared_ptr<Client>> Client::connectShared(ClientConfig t_cfg) {
    auto res = connect(std::move(t_cfg));
    if (res.hasError())
        return sgrn::Result<std::shared_ptr<Client>>::Error(res.error());
    return std::shared_ptr<Client>(new Client(std::move(res.value())));
}

void Client::disconnect() noexcept {
    std::lock_guard lock(mutex_);
    if (ctx_ && connected_.load(std::memory_order_relaxed))
        ::modbus_close(ctx_);
    connected_.store(false, std::memory_order_relaxed);
}

sgrn::Result<void> Client::connectLocked() {
    if (!ctx_)
        return fmt::format("modbus client context is null");
    if (connected_.load(std::memory_order_relaxed))
        return {};

    if (::modbus_connect(ctx_) == -1)
        return fmt::format("modbus_connect({}:{}) failed: {}", cfg_.host, cfg_.port, ::modbus_strerror(errno));

    connected_.store(true, std::memory_order_release);
    return {};
}

sgrn::Result<void> Client::ensureConnected() {
    std::lock_guard lock(mutex_);
    if (connected_.load(std::memory_order_acquire))
        return {};

    if (!ctx_) {
        auto ctx_res = createContext(cfg_);
        if (ctx_res.hasError())
            return fmt::format("{}", ctx_res.error());
        ctx_ = ctx_res.value();
    }
    return connectLocked();
}

sgrn::Result<int> Client::readCoils(int t_addr, int t_count, uint8_t* tp_dest) {
    std::lock_guard lock(mutex_);
    if (!connected_.load(std::memory_order_acquire))
        return fmt::format("readCoils: not connected");
    const int t_rc = ::modbus_read_bits(ctx_, t_addr, t_count, tp_dest);
    if (t_rc == -1)
        return fmt::format("modbus_read_bits: {}", ::modbus_strerror(errno));
    return t_rc;
}

sgrn::Result<int> Client::readDiscreteInputs(int t_addr, int t_count, uint8_t* tp_dest) {
    std::lock_guard lock(mutex_);
    if (!connected_.load(std::memory_order_acquire))
        return fmt::format("readDiscreteInputs: not connected");
    const int t_rc = ::modbus_read_input_bits(ctx_, t_addr, t_count, tp_dest);
    if (t_rc == -1)
        return fmt::format("modbus_read_input_bits: {}", ::modbus_strerror(errno));
    return t_rc;
}

sgrn::Result<int> Client::readHoldingRegisters(int t_addr, int t_count, uint16_t* tp_dest) {
    std::lock_guard lock(mutex_);
    if (!connected_.load(std::memory_order_acquire))
        return fmt::format("readHoldingRegisters: not connected");
    const int t_rc = ::modbus_read_registers(ctx_, t_addr, t_count, tp_dest);
    if (t_rc == -1)
        return fmt::format("modbus_read_registers: {}", ::modbus_strerror(errno));
    return t_rc;
}

sgrn::Result<int> Client::readInputRegisters(int t_addr, int t_count, uint16_t* tp_dest) {
    std::lock_guard lock(mutex_);
    if (!connected_.load(std::memory_order_acquire))
        return fmt::format("readInputRegisters: not connected");
    const int t_rc = ::modbus_read_input_registers(ctx_, t_addr, t_count, tp_dest);
    if (t_rc == -1)
        return fmt::format("modbus_read_input_registers: {}", ::modbus_strerror(errno));
    return t_rc;
}

sgrn::Result<void> Client::writeCoil(int t_addr, bool t_value) {
    std::lock_guard lock(mutex_);
    if (!connected_.load(std::memory_order_acquire))
        return fmt::format("writeCoil: not connected");
    return checkRc(::modbus_write_bit(ctx_, t_addr, t_value ? 1 : 0), "modbus_write_bit");
}

sgrn::Result<void> Client::writeRegister(int t_addr, uint16_t t_value) {
    std::lock_guard lock(mutex_);
    if (!connected_.load(std::memory_order_acquire))
        return fmt::format("writeRegister: not connected");
    return checkRc(::modbus_write_register(ctx_, t_addr, t_value), "modbus_write_register");
}

sgrn::Result<int> Client::writeCoils(int t_addr, int t_count, const uint8_t* tp_src) {
    std::lock_guard lock(mutex_);
    if (!connected_.load(std::memory_order_acquire))
        return fmt::format("writeCoils: not connected");
    const int t_rc = ::modbus_write_bits(ctx_, t_addr, t_count, const_cast<uint8_t*>(tp_src));
    if (t_rc == -1)
        return fmt::format("modbus_write_bits: {}", ::modbus_strerror(errno));
    return t_rc;
}

sgrn::Result<int> Client::writeRegisters(int t_addr, int t_count, const uint16_t* tp_src) {
    std::lock_guard lock(mutex_);
    if (!connected_.load(std::memory_order_acquire)) {
        return "writeRegisters: not connected";
    }
    const int t_rc = ::modbus_write_registers(ctx_, t_addr, t_count, const_cast<uint16_t*>(tp_src));
    if (t_rc == -1)
        return fmt::format("modbus_write_registers: {}", ::modbus_strerror(errno));
    return t_rc;
}

} // namespace sgrn::gateway::wrappers::modbus
