#include <sgrn/gateway/wrappers/modbus/Server.hpp>

#include <modbus.h>

#include <fmt/core.h>

#include <cerrno>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif

namespace sgrn::gateway::wrappers::modbus
{

Server::Server(modbus_t* tp_ctx) noexcept
    : ctx_(tp_ctx) {
}

Server::~Server() noexcept {
    destroy();
}

Server::Server(Server&& t_other) noexcept
    : ctx_(std::exchange(t_other.ctx_, nullptr))
    , listen_socket_(std::exchange(t_other.listen_socket_, -1)) {
}

Server& Server::operator=(Server&& t_other) noexcept {
    if (this != &t_other) {
        destroy();
        ctx_ = std::exchange(t_other.ctx_, nullptr);
        listen_socket_ = std::exchange(t_other.listen_socket_, -1);
    }
    return *this;
}

void Server::destroy() noexcept {
    closeListenSocket();
    if (ctx_) {
        ::modbus_free(ctx_);
        ctx_ = nullptr;
    }
}

sgrn::Result<Server> Server::createTcp(std::string_view t_host, uint16_t t_port) {
    std::string host_str(t_host);
    modbus_t* p_ctx = ::modbus_new_tcp(host_str.c_str(), static_cast<int>(t_port));
    if (!p_ctx)
        return sgrn::Result<Server>::Error(fmt::format("modbus_new_tcp({}:{}) failed: {}", t_host, t_port, ::modbus_strerror(errno)));
    return Server(p_ctx);
}

sgrn::Result<void> Server::listen(int t_backlog) {
    if (!ctx_)
        return "modbus server context is null";

    const int sock = ::modbus_tcp_listen(ctx_, t_backlog);
    if (sock == -1)
        return fmt::format("modbus_tcp_listen failed: {}", ::modbus_strerror(errno));

    listen_socket_ = sock;
    return {};
}

void Server::closeListenSocket() noexcept {
    if (listen_socket_ == -1)
        return;
#ifdef _WIN32
    ::closesocket(listen_socket_);
#else
    ::close(listen_socket_);
#endif
    listen_socket_ = -1;
}

sgrn::Result<int> Server::accept() {
    if (!ctx_ || listen_socket_ == -1)
        return "modbus server is not listening";

    int server_socket = listen_socket_;
    const int client_fd = ::modbus_tcp_accept(ctx_, &server_socket);
    if (client_fd == -1)
        return fmt::format("modbus_tcp_accept failed: {}", ::modbus_strerror(errno));
    return client_fd;
}

void Server::setClientSocket(int t_fd) noexcept {
    if (ctx_)
        ::modbus_set_socket(ctx_, t_fd);
}

sgrn::Result<int> Server::receive(uint8_t* tp_buffer, int t_max_len) {
    if (!ctx_)
        return "modbus server context is null";

    const int rc = ::modbus_receive(ctx_, tp_buffer);
    if (rc == -1)
        return fmt::format("modbus_receive failed: {}", ::modbus_strerror(errno));
    if (rc > t_max_len)
        return fmt::format("modbus_receive returned {} bytes (buffer size {}", rc, t_max_len);
    return rc;
}

sgrn::Result<void> Server::reply(const uint8_t* tp_query, int t_query_len, modbus_mapping_t* tp_mapping) {
    if (!ctx_)
        return "modbus server context is null";
    if (!tp_mapping)
        return "modbus mapping is null";

    if (::modbus_reply(ctx_, tp_query, t_query_len, tp_mapping) == -1)
        return fmt::format("modbus_reply failed: {}", ::modbus_strerror(errno));
    return {};
}

} // namespace sgrn::gateway::wrappers::modbus
