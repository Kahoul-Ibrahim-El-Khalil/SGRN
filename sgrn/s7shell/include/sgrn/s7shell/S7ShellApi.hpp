// =============================================================================
// S7ShellApi.hpp
//
// AngelScript bindings for S7 shell with OOP interface and generic print()
// =============================================================================

#pragma once

#include <angelscript.h>

#include <memory>
#include <string>

namespace sgrn::s7shell::shell_api
{

// Forward declarations
struct IAngelScriptHost;
class S7ClientWrapper;

// =============================================================================
// Generic print() implementation
// =============================================================================

/**
 * @brief Generic print function that handles all types
 * Detects the type and formats appropriately using fmt
 */
void genericPrint(asIScriptGeneric* tp_gen);

// =============================================================================
// S7Client OOP Wrapper Class
// =============================================================================

/**
 * @brief Object-oriented S7Client wrapper for AngelScript
 * Supports: plc = s7client(); plc.load(); data = plc.get("DB1");
 */
class S7ClientWrapper {
public:
    S7ClientWrapper();
    ~S7ClientWrapper();

    // Connection methods
    void connect(const std::string& t_ip, int t_rack, int t_slot, int t_port);
    void disconnect();
    bool isConnected() const;

    // Data access methods (returns JSON string)
    std::string get(const std::string& t_target);
    void put(const std::string& t_target, const std::string& t_value);

    // Convenience methods
    bool load(const std::string& t_path);
    std::string info();
    std::string status();
    std::string plcTime();
    std::string dump(const std::string& t_area, int t_offset, int t_length);

private:
    void* conn_{nullptr};
};

// =============================================================================
// Registration functions
// =============================================================================

/**
 * @brief Register all S7 shell API functions and classes with AngelScript
 * Includes OOP S7Client class, generic print(), and backward-compatible functions
 */
void registerS7Api(asIScriptEngine* tp_engine, IAngelScriptHost* tp_host);

} // namespace sgrn::s7shell::shell_api
