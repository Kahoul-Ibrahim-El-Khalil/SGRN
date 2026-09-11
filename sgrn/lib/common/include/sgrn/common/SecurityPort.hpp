#pragma once

#include <cstdint>
#include <string>

namespace sgrn::common
{

/**
 * @brief Abstract authorization policy for protocol adapters.
 *
 * Dependency-inversion boundary between the southbound protocol adapters
 * (s7, modbus, ethernetip) and the gateway's SecurityManager — in tests a
 * fake. Each method mirrors the SecurityManager check the corresponding
 * adapter performs today; a null policy at the adapter means "no gate"
 * exactly where the adapters currently treat a missing manager as allow
 * (modbus/ethernetip) — the s7 write path keeps its fail-closed shape and
 * denies when no policy is installed.
 */
struct ISecurityPolicy {
    virtual ~ISecurityPolicy() = default;

    /// S7 PUT authorization for one sender/area/DB (fail-closed).
    virtual bool authorizeWrite(int t_sender_id, int t_area, uint16_t t_db_number) = 0;
    /// Modbus master authorization for one client IP/DB (fail-open on null).
    virtual bool authorizeModbus(const std::string& t_client_ip, uint16_t t_db_number) = 0;
    /// EtherNet/IP client authorization (fail-open on null).
    virtual bool authorizeEip(const std::string& t_client_ip) = 0;
};

} // namespace sgrn::common
