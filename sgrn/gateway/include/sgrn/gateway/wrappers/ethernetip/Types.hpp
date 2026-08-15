#pragma once

#include <cstdint>
#include <functional>
#include <string>

extern "C" {
#include <ciptypes.h>
#include <opener_api.h>
}

namespace sgrn::gateway::wrappers::ethernetip
{

/// Vendor-specific S7 gateway CIP class code used by the adapter.
inline constexpr uint32_t kS7GatewayClassCode = 0x64;

/// Standard EtherNet/IP encapsulation TCP port.
inline constexpr uint16_t kDefaultPort = 44818;

using PreGetCallback = std::function<EipStatus(void* tp_instance, void* tp_attribute, uint8_t t_service)>;
using PostSetCallback = std::function<EipStatus(void* tp_instance, void* tp_attribute, uint8_t t_service)>;

struct AttributeSpec {
    uint16_t attribute_number{0};
    uint8_t cip_type{0};
    CipAttributeEncodeInMessage encode{nullptr};
    CipAttributeDecodeFromMessage decode{nullptr};
    void* data{nullptr};
    uint8_t flags{0};
};

inline uint16_t instanceNumber(void* tp_instance_ptr) {
    return static_cast<CipInstance*>(tp_instance_ptr)->instance_number;
}

inline uint16_t attributeNumber(void* tp_attribute_ptr) {
    return static_cast<CipAttributeStruct*>(tp_attribute_ptr)->attribute_number;
}

} // namespace sgrn::gateway::wrappers::ethernetip
