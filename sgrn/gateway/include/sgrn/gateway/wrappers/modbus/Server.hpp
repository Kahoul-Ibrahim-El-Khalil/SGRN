#pragma once

#include <sgrn/Result.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

struct _modbus;
typedef struct _modbus modbus_t;

struct _modbus_mapping_t;
typedef struct _modbus_mapping_t modbus_mapping_t;

namespace sgrn::gateway::wrappers::modbus
{

/// libmodbus MODBUS_TCP_MAX_ADU_LENGTH — exposed for adapter receive buffers.
inline constexpr int kTcpMaxAduLength = 260;

/// RAII wrapper over libmodbus TCP server context (modbus_t).
///
/// The adapter owns the accept/serve thread and select loop — matching the
/// OpcUaAdapter pattern. This class manages modbus_t lifetime, listen
/// socket, and request/reply helpers.
class Server {
public:
    ~Server() noexcept;

    Server(Server&&) noexcept;
    Server& operator=(Server&&) noexcept;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    static sgrn::Result<Server> createTcp(std::string_view t_host, uint16_t t_port);

    sgrn::Result<void> listen(int t_backlog = 5);
    void closeListenSocket() noexcept;

    int listenSocket() const noexcept {
        return listen_socket_;
    }

    /// Accept one master connection and bind it to this context.
    sgrn::Result<int> accept();

    void setClientSocket(int t_fd) noexcept;

    sgrn::Result<int> receive(uint8_t* tp_buffer, int t_max_len);
    sgrn::Result<void> reply(const uint8_t* tp_query, int t_query_len, modbus_mapping_t* tp_mapping);

    /// INTERNAL — adapter layer only.
    modbus_t* raw() noexcept {
        return ctx_;
    }

private:
    explicit Server(modbus_t* tp_ctx) noexcept;

    void destroy() noexcept;

    modbus_t* ctx_{nullptr};
    int listen_socket_{-1};
};

} // namespace sgrn::gateway::wrappers::modbus
