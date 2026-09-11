#pragma once

#include <sgrn/common/MemoryPort.hpp>
#include <sgrn/common/SecurityPort.hpp>

#include <memory>

namespace sgrn::gateway::twin
{
class PlcMemory;
} // namespace sgrn::gateway::twin

namespace sgrn::gateway
{
class SecurityManager;
} // namespace sgrn::gateway

namespace sgrn::gateway::adapters::ports
{

/**
 * @brief IMemoryPort backed by the twin's PlcMemory.
 *
 * Thin forwarding glue constructed once by the gateway wiring and injected
 * into the southbound protocol adapters (s7, modbus, ethernetip). Twin
 * failures are mapped onto the protocol-agnostic ErrorClass vocabulary at
 * this boundary, so adapters never name twin error types.
 */
class TwinMemoryPort : public ::sgrn::common::IMemoryPort {
public:
    explicit TwinMemoryPort(twin::PlcMemory& t_memory);

    bool isReady() const override;
    std::vector<uint16_t> topLevelDbNumbers() const override;
    std::optional<size_t> dbSize(uint16_t t_db_number) const override;
    sgrn::Result<void, ::sgrn::common::ErrorClass> readDbMemory(
        uint16_t t_db_number, size_t t_offset, size_t t_size, uint8_t* tp_buffer) override;
    sgrn::Result<void, ::sgrn::common::ErrorClass> readDbMemory(std::span<const ::sgrn::common::DbMemorySpan> t_spans) override;
    sgrn::Result<void, ::sgrn::common::ErrorClass> writeDbMemory(
        uint16_t t_db_number, size_t t_offset, size_t t_size, const uint8_t* tp_buffer) override;
    sgrn::Result<void, ::sgrn::common::ErrorClass> writeBit(
        uint16_t t_db_number, size_t t_byte_offset, int t_bit_index, bool t_value) override;
    sgrn::Result<void, ::sgrn::common::ErrorClass> updateField(
        uint16_t t_db_number, const std::string& t_field_path, const std::string& t_value_json) override;

private:
    twin::PlcMemory& memory_;
};

/**
 * @brief ISecurityPolicy backed by the gateway's SecurityManager.
 *
 * Null-manager semantics mirror each adapter's current behavior: modbus and
 * ethernetip treat a missing manager as allow, the s7 write path stays
 * fail-closed (deny) rather than crashing on a null dereference.
 */
class GatewaySecurityPolicy : public ::sgrn::common::ISecurityPolicy {
public:
    explicit GatewaySecurityPolicy(std::shared_ptr<SecurityManager> tsp_security_manager);

    bool authorizeWrite(int t_sender_id, int t_area, uint16_t t_db_number) override;
    bool authorizeModbus(const std::string& t_client_ip, uint16_t t_db_number) override;
    bool authorizeEip(const std::string& t_client_ip) override;

private:
    std::shared_ptr<SecurityManager> security_;
};

} // namespace sgrn::gateway::adapters::ports
