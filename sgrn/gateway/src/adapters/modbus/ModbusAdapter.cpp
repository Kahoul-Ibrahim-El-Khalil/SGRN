/**
 * @file  ModbusAdapter.cpp
 * @brief Passive Modbus TCP slave adapter implementation.
 */

#include <sgrn/debug.hpp>
#include <sgrn/gateway/adapters/modbus/ModbusAdapter.hpp>
#include <sgrn/gateway/adapters/modbus/TypeTranslation.hpp>
#include <sgrn/gateway/common/SecurityHelper.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/wrappers/modbus/Server.hpp>
#include <sgrn/scl/types.hpp>

#include <fmt/core.h>
#include <s7codec/endian.hpp>
#include <s7codec/types.hpp>

#include <cerrno>
#include <cmath>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace sgrn::gateway::adapters::modbus
{

using ::sgrn::scl::DataType;
using ::sgrn::scl::ModbusVirtualEntry;

static constexpr int kMbapLen = 7;

static bool isWriteFC(uint8_t t_fc) {
    return t_fc == 0x05 || t_fc == 0x06 || t_fc == 0x0F || t_fc == 0x10;
}
static bool isReadFC(uint8_t t_fc) {
    return t_fc == 0x01 || t_fc == 0x02 || t_fc == 0x03 || t_fc == 0x04;
}
static bool isBitFC(uint8_t t_fc) {
    return t_fc == 0x01 || t_fc == 0x02 || t_fc == 0x05 || t_fc == 0x0F;
}

ModbusAdapter::ModbusAdapter(twin::PlcMemory& t_memory, std::shared_ptr<SecurityManager> tsp_security_manager)
    : common::AdapterBase<ModbusAdapter>(t_memory, std::move(tsp_security_manager)) {
}

ModbusAdapter::~ModbusAdapter() {
    stop();
}

sgrn::Result<void, ::sgrn::scl::Error> ModbusAdapter::start(
    const std::string& t_ip, uint16_t t_port, const ::sgrn::scl::PlcSchemaStore& t_store) {
    // Store configuration for configure() to use
    config_ip_ = t_ip;
    config_port_ = t_port;
    p_store_ = &t_store;

    // Call AdapterBase::start which invokes configure() then serveLoop()
    auto res = common::AdapterBase<ModbusAdapter>::start(t_ip, t_port);
    if (res.hasError()) {
        return ::sgrn::scl::Err::Generic("{}", res.error());
    }
    return {};
}

bool ModbusAdapter::configure(const std::string& /*t_ip*/, uint16_t /*t_port*/) {
    if (!p_store_) {
        SGRN_ERROR_LOG("Modbus: PlcSchemaStore not set during configure");
        return false;
    }

    vmap_ = ::sgrn::scl::buildModbusVirtualMap(*p_store_);

    if (vmap_.empty()) {
        SGRN_WARN_LOG("Modbus: no DBs annotated with #MODBUS_* — adapter has no mappings.");
    }
    for (const auto& w : vmap_.warnings)
        SGRN_WARN_LOG("Modbus map: {}", w);

    auto server_res = wrappers::modbus::Server::createTcp(config_ip_, config_port_);
    if (server_res.hasError()) {
        SGRN_ERROR_LOG("Modbus: {}", server_res.error());
        return false;
    }

    const int nb_bits = std::max(1, vmap_.total_coils);
    const int nb_ibit = std::max(1, vmap_.total_discrete);
    const int nb_regs = std::max(1, vmap_.total_holding);
    const int nb_iregs = std::max(1, vmap_.total_input);

    auto mapping_res = wrappers::modbus::Mapping::create(nb_bits, nb_ibit, nb_regs, nb_iregs);
    if (mapping_res.hasError()) {
        SGRN_ERROR_LOG("Modbus: {}", mapping_res.error());
        return false;
    }

    server_ = std::move(server_res.value());
    mapping_ = std::move(mapping_res.value());

    syncArenaToMapping();

    if (auto listen_res = server_->listen(5); listen_res.hasError()) {
        server_.reset();
        mapping_.reset();
        SGRN_ERROR_LOG("Modbus: listen on {}:{} failed: {}", config_ip_, config_port_, listen_res.error());
        return false;
    }

    SGRN_INFO_LOG("Modbus Adapter configured on {}:{}", config_ip_, config_port_);
    return true;
}

void ModbusAdapter::serveLoop() {
    while (runningFlag().load()) {
        if (!server_)
            break;

        const int listen_fd = server_->listenSocket();
        if (listen_fd == -1)
            break;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        struct timeval tv{0, 200'000};

        const int ready = ::select(listen_fd + 1, &rfds, nullptr, nullptr, &tv);
        if (ready <= 0)
            continue;

        auto accept_res = server_->accept();
        if (accept_res.hasError()) {
            if (runningFlag().load())
                SGRN_WARN_LOG("Modbus: accept failed: {}", accept_res.error());
            continue;
        }

        const int client_fd = accept_res.value();
        handleClient(client_fd);
    }
}

void ModbusAdapter::stop() {
    common::AdapterBase<ModbusAdapter>::stop();
    if (server_)
        server_->closeListenSocket();
    server_.reset();
    mapping_.reset();
}

void ModbusAdapter::handleClient(int t_client_fd) {
    if (!server_ || !mapping_)
        return;

    server_->setClientSocket(t_client_fd);

    std::string client_ip;
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(t_client_fd, (struct sockaddr*)&addr, &addr_len) == 0) {
        if (addr.ss_family == AF_INET) {
            char ip4[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(((struct sockaddr_in*)&addr)->sin_addr), ip4, INET_ADDRSTRLEN);
            client_ip = ip4;
        } else if (addr.ss_family == AF_INET6) {
            char ip6[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &(((struct sockaddr_in6*)&addr)->sin6_addr), ip6, INET6_ADDRSTRLEN);
            client_ip = ip6;
        }
    }

    std::vector<uint8_t> query(wrappers::modbus::kTcpMaxAduLength);

    while (runningFlag().load()) {
        auto rc_res = server_->receive(query.data(), static_cast<int>(query.size()));
        if (rc_res.hasError())
            break;

        const int rc = rc_res.value();
        const uint8_t fc = query[static_cast<size_t>(kMbapLen)];

        if (isReadFC(fc))
            syncArenaToMapping();

        if (server_->reply(query.data(), rc, mapping_->raw()).hasError())
            break;

        if (isWriteFC(fc)) {
            const uint8_t* p_pdu = query.data() + kMbapLen + 1;
            const uint16_t mb_addr = (static_cast<uint16_t>(p_pdu[0]) << 8) | p_pdu[1];
            const uint16_t count = (static_cast<uint16_t>(p_pdu[2]) << 8) | p_pdu[3];

            const bool bit_space = isBitFC(fc);
            const auto& entries = bit_space ? (fc == 0x05 || fc == 0x0F ? vmap_.coil : vmap_.discrete) : vmap_.holding;

            for (const auto& entry : entries) {
                if (entry.read_only)
                    continue;
                const int written_end = mb_addr + count;
                const int entry_end = entry.reg_count;
                if (entry.reg_start < written_end && entry_end > mb_addr) {
                    auto sec_mgr = getSecurityManager();
                    if (!sec_mgr || sec_mgr->authorizeModbus(client_ip, entry.db_number)) {
                        syncEntryToArena(entry, client_ip);
                    } else {
                        SGRN_WARN_LOG("Modbus: Write to DB{} denied for {}", entry.db_number, client_ip);
                    }
                }
            }
        }
    }

#ifdef _WIN32
    ::closesocket(client_fd);
#else
    ::close(t_client_fd);
#endif
}

void ModbusAdapter::syncArenaToMapping() {
    if (!mapping_)
        return;

    // Batch sync for registers using span API (single lock per DB instead of per entry)
    auto sync_reg_entries_batch = [&](const std::vector<ModbusVirtualEntry>& t_entries, uint16_t* tp_regs) {
        if (t_entries.empty())
            return;

        // Collect all read spans and buffers for batch operation
        std::vector<twin::DbMemorySpan> spans;
        std::vector<std::vector<uint8_t>> buffers;

        for (const auto& e : t_entries) {
            buffers.emplace_back(static_cast<size_t>(e.byte_count), 0);
            spans.push_back({.db = e.db_number,
                .offset = static_cast<size_t>(e.byte_offset),
                .size = static_cast<size_t>(e.byte_count),
                .p_buffer = buffers.back().data()});
        }

        // Single batch read: one lock per unique DB instead of per entry
        if (auto r = getMemory().readDbMemory(std::span(spans)); !r) {
            SGRN_WARN_LOG("Modbus syncRegEntries batch read failed (DB scope): {}", r.error());
            return;
        }

        // Encode all successfully-read buffers
        for (size_t i = 0; i < t_entries.size(); ++i) {
            encodeToRegisters(t_entries[i], buffers[i].data(), tp_regs + t_entries[i].reg_start);
        }
    };

    // Batch sync for bits using span API
    auto sync_bit_entries_batch = [&](const std::vector<ModbusVirtualEntry>& t_entries, uint8_t* tp_bits) {
        if (t_entries.empty())
            return;

        // Collect all read spans and buffers for batch operation
        std::vector<twin::DbMemorySpan> spans;
        std::vector<std::vector<uint8_t>> buffers;

        for (const auto& e : t_entries) {
            buffers.emplace_back(static_cast<size_t>(e.byte_count), 0);
            spans.push_back({.db = e.db_number,
                .offset = static_cast<size_t>(e.byte_offset),
                .size = static_cast<size_t>(e.byte_count),
                .p_buffer = buffers.back().data()});
        }

        // Single batch read: one lock per unique DB instead of per entry
        if (auto r = getMemory().readDbMemory(std::span(spans)); !r) {
            SGRN_WARN_LOG("Modbus syncBitEntries batch read failed (DB scope): {}", r.error());
            return;
        }

        // Encode all successfully-read buffers
        for (size_t i = 0; i < t_entries.size(); ++i) {
            encodeToBits(t_entries[i], buffers[i].data(), tp_bits + t_entries[i].reg_start);
        }
    };

    // Invoke batch sync for all four Modbus mapping types
    if (mapping_->nbRegisters() > 0)
        sync_reg_entries_batch(vmap_.holding, mapping_->registers());
    if (mapping_->nbInputRegisters() > 0)
        sync_reg_entries_batch(vmap_.input, mapping_->inputRegisters());
    if (mapping_->nbBits() > 0)
        sync_bit_entries_batch(vmap_.coil, mapping_->bits());
    if (mapping_->nbInputBits() > 0)
        sync_bit_entries_batch(vmap_.discrete, mapping_->inputBits());
}

/**
 * @brief Synchronizes a Modbus register/coil change to the central Digital Twin.
 *
 * DESIGN NOTE: Why push a JSON string instead of raw binary bytes?
 *
 * 1. Struct Padding & Safety: S7 structs have hidden padding bytes. Blasting raw Modbus registers
 *    directly into memory would silently corrupt down-stream fields if alignments don't perfectly match.
 * 2. Endianness & Math Safety: Different Modbus masters word-swap 32-bit floats differently. By
 *    mathematically decoding the bytes into a string (e.g., "123.45"), the Twin engine re-encodes
 *    it perfectly into the native S7 binary format, completely eliminating float corruption.
 * 3. Twin Validation & Telemetry: updateField() handles schema type-checking, bounds validation,
 *    and triggers change-detection events (for MQTT, Websockets, etc.). A raw memory injection
 *    would bypass this, requiring an expensive background polling thread to detect changes.
 */
void ModbusAdapter::syncEntryToArena(const ModbusVirtualEntry& t_entry, const std::string& t_client_ip) {
    if (!mapping_) {
        return;
    }

    std::string json_val;

    if (t_entry.type == DataType::Bool) {
        if (t_entry.reg_start >= mapping_->nbBits())
            return;
        const bool v = mapping_->bits()[t_entry.reg_start] != 0;
        json_val = v ? "true" : "false";
    } else {
        if (t_entry.reg_start + t_entry.reg_count > mapping_->nbRegisters())
            return;
        const uint16_t* p_regs = mapping_->registers() + t_entry.reg_start;
        json_val = decodeRegisters(t_entry, p_regs);
    }

    if (json_val.empty())
        return;

    auto res = getMemory().updateField(t_entry.db_number, t_entry.field_path, json_val);
    if (res.hasError()) {
        SGRN_WARN_LOG("Modbus: updateField DB{}/'{}'  failed: {}", t_entry.db_number, t_entry.field_path, res.error().string());
    }
}

std::string ModbusAdapter::decodeRegisters(const ModbusVirtualEntry& t_entry, const uint16_t* tp_regs) const {
    const int useful_bytes = t_entry.byte_count;
    std::vector<uint8_t> bytes(static_cast<size_t>(useful_bytes), 0);

    for (int r = 0; r < t_entry.reg_count; ++r) {
        const int hi_idx = r * 2;
        const int lo_idx = r * 2 + 1;
        if (hi_idx < useful_bytes)
            bytes[hi_idx] = static_cast<uint8_t>(tp_regs[r] >> 8);
        if (lo_idx < useful_bytes)
            bytes[lo_idx] = static_cast<uint8_t>(tp_regs[r] & 0xFF);
    }

    return TypeTranslation::decodeBytesToString(t_entry.type, useful_bytes, bytes.data());
}

std::string ModbusAdapter::decodeBits(const ModbusVirtualEntry& t_entry, const uint8_t* tp_bits) const {
    return tp_bits[t_entry.reg_start] ? "true" : "false";
}

void ModbusAdapter::encodeToRegisters(const ModbusVirtualEntry& t_entry, const uint8_t* tp_arena_bytes, uint16_t* tp_regs_out) const {
    for (int r = 0; r < t_entry.reg_count; ++r) {
        const int hi_idx = r * 2;
        const int lo_idx = r * 2 + 1;
        const uint8_t hi = (hi_idx < t_entry.byte_count) ? tp_arena_bytes[hi_idx] : 0;
        const uint8_t lo = (lo_idx < t_entry.byte_count) ? tp_arena_bytes[lo_idx] : 0;
        tp_regs_out[r] = static_cast<uint16_t>((hi << 8) | lo);
    }
}

void ModbusAdapter::encodeToBits(const ModbusVirtualEntry& t_entry, const uint8_t* tp_arena_bytes, uint8_t* tp_bits_out) const {
    for (int i = 0; i < t_entry.reg_count; ++i) {
        const int absolute_bit = t_entry.bit_index + i;
        const int byte_idx = absolute_bit / 8;
        const int bit_idx = absolute_bit % 8;
        if (byte_idx < t_entry.byte_count)
            tp_bits_out[i] = (tp_arena_bytes[byte_idx] >> bit_idx) & 0x01;
        else
            tp_bits_out[i] = 0;
    }
}

} // namespace sgrn::gateway::adapters::modbus
