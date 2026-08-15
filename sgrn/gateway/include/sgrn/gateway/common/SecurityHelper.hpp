#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/gateway/security/SecurityManager.hpp>
#include <optional>
#include <string>
#include <vector>

namespace sgrn::gateway::common
{

/**
 * @brief Helper functions for common security operations
 */
class SecurityHelper {
public:
    /**
     * @brief Authorize a read operation
     * @return Success if authorized, error string if denied
     */
    static sgrn::Result<void, std::string> authorizeRead(SecurityManager& t_security, security::Protocol t_protocol,
        const std::string& t_client_ip, std::optional<uint16_t> t_db_number, const std::string& t_field_path,
        const std::string& t_origin = "", const std::vector<std::string>& t_headers = {}) {

        if (t_security.authorizeField(t_protocol, t_client_ip, t_db_number, t_field_path, false, t_origin, t_headers, "")) {
            return {};
        }
        return "Authorization denied for read: " + t_field_path;
    }

    /**
     * @brief Authorize a write operation
     * @return Success if authorized, error string if denied
     */
    static sgrn::Result<void, std::string> authorizeWrite(SecurityManager& t_security, security::Protocol t_protocol,
        const std::string& t_client_ip, std::optional<uint16_t> t_db_number, const std::string& t_field_path,
        const std::string& t_origin = "", const std::vector<std::string>& t_headers = {}) {

        if (t_security.authorizeField(t_protocol, t_client_ip, t_db_number, t_field_path, true, t_origin, t_headers, "")) {
            return {};
        }
        return "Authorization denied for write: " + t_field_path;
    }

    /**
     * @brief Authorize connection (no field-level check)
     * @return Success if authorized, error string if denied
     */
    static sgrn::Result<void, std::string> authorizeConnection(SecurityManager& t_security, security::Protocol t_protocol,
        const std::string& t_client_ip, std::optional<uint16_t> t_db_number = std::nullopt, const std::string& t_origin = "") {

        bool authorized = false;
        switch (t_protocol) {
            case security::Protocol::HTTP:
                authorized = t_security.authorizeHttp(t_client_ip, t_origin, {}, t_db_number);
                break;
            case security::Protocol::WebSocket:
                authorized = t_security.authorizeWebSocket(t_client_ip, t_origin, t_db_number);
                break;
            case security::Protocol::Modbus:
                authorized = t_security.authorizeModbus(t_client_ip, t_db_number);
                break;
            case security::Protocol::OpcUA:
                authorized = t_security.authorizeOpcUa(t_client_ip, "", t_db_number);
                break;
            case security::Protocol::EthernetIP:
                authorized = t_security.authorizeEip(t_client_ip);
                break;
            default:
                return "Unknown protocol";
        }

        if (authorized) {
            return {};
        }
        return "Connection authorization denied";
    }
};

} // namespace sgrn::gateway::common