#include <sgrn/gateway/wrappers/ethernetip/Client.hpp>

namespace sgrn::gateway::wrappers::ethernetip
{

namespace
{

constexpr const char client_unsupported[] =
    "EtherNet/IP client not supported: OpENer is adapter-only. Integrate a dedicated originator stack for explicit messaging.";

} // namespace

Client::Client(ClientConfig t_cfg)
    : cfg_(std::move(t_cfg)) {
}

sgrn::Result<Client> Client::connect(ClientConfig t_cfg) {
    (void)t_cfg;
    return fmt::format("{}", client_unsupported);
}

void Client::disconnect() noexcept {
    connected_ = false;
}

sgrn::Result<std::vector<uint8_t>> Client::readAttribute(uint32_t t_class_id, uint16_t t_instance_id, uint16_t t_attribute_id) {
    (void)t_class_id;
    (void)t_instance_id;
    (void)t_attribute_id;
    return fmt::format("{}", client_unsupported);
}

sgrn::Result<void> Client::writeAttribute(
    uint32_t t_class_id, uint16_t t_instance_id, uint16_t t_attribute_id, const std::vector<uint8_t>& t_data) {
    (void)t_class_id;
    (void)t_instance_id;
    (void)t_attribute_id;
    (void)t_data;
    return fmt::format("{}", client_unsupported);
}

} // namespace sgrn::gateway::wrappers::ethernetip
