#pragma once

/**
 * @file  ModbusAdapter.hpp
 * @brief Passive Modbus TCP slave adapter — gateway listens, never initiates.
 *
 * ROLE
 * ─────
 * The gateway acts as a Modbus TCP slave/server.  External Modbus masters
 * (SCADA systems, HMIs, the future modbusproxy process) connect to it and
 * write register data into it.  This is architecturally equivalent to the
 * S7 PUT model: data arrives only when a master sends it; the gateway
 * remains entirely event-driven.
 *
 * MAPPING
 * ────────
 * The virtual register map is built from the PlcSchemaStore at startup,
 * using the #MODBUS_HOLDING / #MODBUS_INPUT / #MODBUS_COIL / #MODBUS_DISCRETE
 * directives in the .scl schema (Appendix G of the thesis).  No external
 * mapping configuration is required.
 *
 * WRITE PATH
 * ──────────
 * On each FC05/FC06/FC15/FC16 (write coil / write register) PDU received:
 *   1. Identify the affected ModbusVirtualEntry from the virtual map.
 *   2. Read raw register words from the libmodbus mapping.
 *   3. Decode to JSON (strip padding if needed).
 * 4. Call the memory port's updateField() — the same path used by S7 PUT events.
 *      This marks the DB dirty → signalDirty() → TelemetryBroker → all
 *      northbound adapters (HTTP, WebSocket, OPC UA, cloud bridge).
 *
 * READ PATH
 * ─────────
 * On each FC01/FC02/FC03/FC04 (read) PDU:
 *   1. For each entry in the requested address range, fetch the current
 *      value from the memory port.
 *   2. Pack bytes into the libmodbus mapping (inserting padding if needed).
 *   3. Let libmodbus build and send the response.
 *
 * THREADING
 * ─────────
 * A single dedicated std::thread runs the accept/serve loop.  All calls
 * into the twin are thread-safe via the existing command queue.
 */

#include <sgrn/common/AdapterBase.hpp>
#include <sgrn/gateway/adapters/modbus/errors.hpp>
#include <sgrn/scl/functions/modbus.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/types.hpp>
#include <sgrn/wrappers/modbus/Mapping.hpp>
#include <sgrn/wrappers/modbus/Server.hpp>

#include <atomic>
#include <optional>
#include <string>
#include <thread>

namespace sgrn::gateway::adapters::modbus
{

/**
 * @brief Passive Modbus TCP slave adapter.
 *
 * Lifecycle:  start(ip, port, store) → [serve loop] → stop()
 */
class ModbusAdapter : public ::sgrn::common::AdapterBase<ModbusAdapter> {
public:
    explicit ModbusAdapter(::sgrn::common::IMemoryPort& t_memory, std::shared_ptr<::sgrn::common::ISecurityPolicy> tsp_security_policy);
    ~ModbusAdapter();

    ModbusAdapter(const ModbusAdapter&) = delete;
    ModbusAdapter& operator=(const ModbusAdapter&) = delete;

    /**
     * @brief Bind, listen, and start the serve thread.
     *
     * Builds the virtual register map from the store, allocates the
     * libmodbus mapping, and launches the background thread.
     *
     * @return Empty on success; a typed ModbusAdapterError (see
     *         <sgrn/gateway/adapters/modbus/errors.hpp>) on failure.
     */
    sgrn::Result<void, ModbusAdapterError> start(const std::string& t_ip, uint16_t t_port, const ::sgrn::scl::PlcSchemaStore& t_store);

    void stop();

    /**
     * @brief Returns the virtual register map for /registry/modbus.
     * Only valid after a successful start().
     */
    const ::sgrn::scl::ModbusVirtualMap& virtualMap() const {
        return vmap_;
    }

private:
    friend class ::sgrn::common::AdapterBase<ModbusAdapter>;

    // AdapterBase calls these during lifecycle
    bool configure(const std::string& t_ip, uint16_t t_port);
    void serveLoop();

    // ── Serve loop ────────────────────────────────────────────────────────
    void handleClient(int t_client_fd);

    // ── Arena ↔ libmodbus mapping sync ───────────────────────────────────
    /// Populate mb_mapping_ from the current arena state (called before reads).
    /// The first backend failure aborts the sync so the caller can answer the
    /// master with a Modbus exception instead of stale mapping content.
    sgrn::Result<void, ::sgrn::common::ErrorClass> syncArenaToMapping();

    /// Push one changed entry from mb_mapping_ back into the twin. Backend
    /// failures propagate; the audit log is kept.
    sgrn::Result<void, ::sgrn::common::ErrorClass> syncEntryToArena(const ::sgrn::scl::ModbusVirtualEntry& t_entry);

    /// Stage an incoming write request's values into mb_mapping_ WITHOUT
    /// replying yet — mirroring libmodbus's own storage conventions (tab_bits
    /// as 0/1 entries, tab_registers as BE words) and address checks — so a
    /// backend failure can still be answered with an exception frame.
    /// Address violations map onto NotFound (0x02, as libmodbus itself);
    /// malformed values collapse onto OutOfRange (0x04 rather than
    /// libmodbus's 0x03 — any exception still signals failure). Cases this
    /// misses are still rejected by reply() itself, exactly as before.
    sgrn::Result<void, ::sgrn::common::ErrorClass> stageWriteRequest(
        uint8_t t_function, uint16_t t_address, uint16_t t_count, const uint8_t* tp_pdu);

    /// Authorize + twin-sync every entry touched by a staged write request.
    /// The first deny/failure aborts with its class for the exception frame
    /// (deny maps onto Internal → 0x04, keeping the layout undisclosed).
    /// Parses and length-checks the request PDU itself (truncation collapses
    /// onto OutOfRange → 0x04).
    sgrn::Result<void, ::sgrn::common::ErrorClass> processWriteRequest(
        uint8_t t_function, const uint8_t* tp_query, int t_query_len, const std::string& t_client_ip);

    // ── Decode: register words → JSON value string ───────────────────────
    std::string decodeRegisters(const ::sgrn::scl::ModbusVirtualEntry& t_entry, const uint16_t* tp_regs) const;
    std::string decodeBits(const ::sgrn::scl::ModbusVirtualEntry& t_entry, const uint8_t* tp_bits) const;

    // ── Encode: arena bytes → register words ────────────────────────────
    void encodeToRegisters(const ::sgrn::scl::ModbusVirtualEntry& t_entry, const uint8_t* tp_arena_bytes, uint16_t* tp_regs_out) const;
    void encodeToBits(const ::sgrn::scl::ModbusVirtualEntry& t_entry, const uint8_t* tp_arena_bytes, uint8_t* tp_bits_out) const;

    // ── State ────────────────────────────────────────────────────────────
    ::sgrn::scl::ModbusVirtualMap vmap_;
    std::optional<wrappers::modbus::Server> server_;
    std::optional<wrappers::modbus::Mapping> mapping_;
    std::string config_ip_;
    uint16_t config_port_{0};
    const ::sgrn::scl::PlcSchemaStore* p_store_{nullptr};
};

} // namespace sgrn::gateway::adapters::modbus
