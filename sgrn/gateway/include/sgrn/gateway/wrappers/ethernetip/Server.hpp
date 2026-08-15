#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/gateway/wrappers/ethernetip/Types.hpp>

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace sgrn::gateway::wrappers::ethernetip
{

struct ServerConfig {
    std::string ip;
    uint16_t port{kDefaultPort};
};

/// RAII wrapper over the OpENer adapter (device/server) stack.
///
/// The adapter owns the serve thread and calls processCyclic() — matching the
/// OpcUaAdapter / ModbusAdapter pattern. This class manages CIP stack
/// lifetime, network handler setup, and object-model registration helpers.
class Server {
public:
    ~Server() noexcept;

    Server(Server&&) noexcept;
    Server& operator=(Server&&) noexcept;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    static sgrn::Result<Server> create(ServerConfig t_cfg);

    sgrn::Result<void> initializeStack(uint8_t t_unique_connection_id = 0x01);

    sgrn::Result<void> createVendorClass(uint32_t t_class_code, uint32_t t_max_instance_attributes, std::string_view t_name);

    void setGetSetCallbacks(PreGetCallback t_pre_get, PostSetCallback t_post_set);

    /// Optional override for kCipByteArray decode (e.g. pycomm3 compatibility).
    void setByteArrayDecoder(CipAttributeDecodeFromMessage t_decoder);

    sgrn::Result<uint16_t> addInstance(uint16_t t_instance_id);

    sgrn::Result<void> insertAttribute(uint16_t t_instance_number, const AttributeSpec& t_spec);

    sgrn::Result<void> startNetwork();

    /// One OpENer network-handler tick. Call from the adapter serve thread.
    sgrn::Result<void> processCyclic(uint32_t t_connection_timeout_ms = 10);

    void requestShutdown() noexcept;
    bool shutdownRequested() const noexcept;

    void shutdown() noexcept;

    /// INTERNAL — adapter layer only.
    CipClass* vendorClass() noexcept {
        return vendor_class_;
    }

private:
    explicit Server(ServerConfig t_cfg);

    void destroy() noexcept;

    ServerConfig cfg_;
    CipClass* vendor_class_{nullptr};
    PreGetCallback pre_get_;
    PostSetCallback post_set_;
    CipAttributeDecodeFromMessage byte_array_decoder_{nullptr};
    std::atomic<bool> shutdown_requested_{false};
    bool stack_initialized_{false};
    bool network_started_{false};
};

} // namespace sgrn::gateway::wrappers::ethernetip
