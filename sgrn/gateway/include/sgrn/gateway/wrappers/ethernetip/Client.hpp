#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/gateway/wrappers/ethernetip/Types.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace sgrn::gateway::wrappers::ethernetip
{

struct ClientConfig {
    std::string host;
    uint16_t port{kDefaultPort};
    uint16_t slot{0};
    uint32_t timeout_ms{5000};
};

/// High-level EtherNet/IP originator (client) wrapper.
///
/// OpENer is adapter-only — it does not implement explicit-message originator
/// role. This class defines the future API surface; connect() and all I/O
/// methods return an error until a dedicated originator stack is integrated.
class Client {
public:
    ~Client() noexcept = default;

    Client(Client&&) noexcept = default;
    Client& operator=(Client&&) noexcept = default;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    static sgrn::Result<Client> connect(ClientConfig t_cfg);

    void disconnect() noexcept;

    bool isConnected() const noexcept {
        return connected_;
    }

    sgrn::Result<std::vector<uint8_t>> readAttribute(uint32_t t_class_id, uint16_t t_instance_id, uint16_t t_attribute_id);

    sgrn::Result<void> writeAttribute(
        uint32_t t_class_id, uint16_t t_instance_id, uint16_t t_attribute_id, const std::vector<uint8_t>& t_data);

private:
    explicit Client(ClientConfig t_cfg);

    ClientConfig cfg_;
    bool connected_{false};
};

} // namespace sgrn::gateway::wrappers::ethernetip
