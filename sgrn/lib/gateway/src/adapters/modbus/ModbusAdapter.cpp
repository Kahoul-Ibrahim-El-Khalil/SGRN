/**
 * @file  ModbusAdapter.cpp
 * @brief Passive Modbus TCP slave adapter implementation.
 */

#include <sgrn/debug.hpp>
#include <sgrn/gateway/adapters/modbus/ModbusAdapter.hpp>
#include <sgrn/gateway/adapters/modbus/TypeTranslation.hpp>
#include <sgrn/gateway/adapters/modbus/errors.hpp>
#include <sgrn/scl/types.hpp>
#include <sgrn/wrappers/modbus/Server.hpp>

#include <fmt/core.h>
#include <algorithm>
#include <chrono>
#include <mutex>
#include <s7codec/endian.hpp>
#include <s7codec/types.hpp>
#include <stdexcept>
#include <string>
#include <thread>

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

using ::sgrn::common::DbMemorySpan;
using ::sgrn::common::ErrorClass;
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

namespace
{

void closeFd(int t_fd) noexcept {
#ifdef _WIN32
    ::closesocket(t_fd);
#else
    ::close(t_fd);
#endif
}

/// Half/full shutdown that wakes a thread parked in select()/recv() on the
/// fd without closing it (close happens on the serve thread via dropClient
/// or in stop() after the join).
void shutdownFd(int t_fd) noexcept {
#ifdef _WIN32
    ::shutdown(t_fd, SD_BOTH);
#else
    ::shutdown(t_fd, SHUT_RDWR);
#endif
}

std::string peerIp(int t_fd) {
    std::string ip;
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    if (getpeername(t_fd, (struct sockaddr*)&addr, &addr_len) == 0) {
        if (addr.ss_family == AF_INET) {
            char ip4[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &(((struct sockaddr_in*)&addr)->sin_addr), ip4, INET_ADDRSTRLEN);
            ip = ip4;
        } else if (addr.ss_family == AF_INET6) {
            char ip6[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &(((struct sockaddr_in6*)&addr)->sin6_addr), ip6, INET6_ADDRSTRLEN);
            ip = ip6;
        }
    }
    return ip;
}

} // namespace

ModbusAdapter::ModbusAdapter(::sgrn::common::IMemoryPort& t_memory, std::shared_ptr<::sgrn::common::ISecurityPolicy> tsp_security_policy)
    : ::sgrn::common::AdapterBase<ModbusAdapter>(t_memory, std::move(tsp_security_policy)) {
}

ModbusAdapter::~ModbusAdapter() {
    stop();
}

sgrn::Result<void, ModbusAdapterError> ModbusAdapter::start(
    const std::string& t_ip, uint16_t t_port, const ::sgrn::scl::PlcSchemaStore& t_store) {
    // Store configuration for configure() to use
    config_ip_ = t_ip;
    config_port_ = t_port;
    p_store_ = &t_store;

    // Call AdapterBase::start which invokes configure() then serveLoop().
    // configure() logs the precise failure cause (schema store missing, TCP
    // bind, mapping alloc); the typed error collapses those onto the
    // server-device-failure class for the wire boundary.
    auto res = ::sgrn::common::AdapterBase<ModbusAdapter>::start(t_ip, t_port);
    if (res.hasError()) {
        return ModbusAdapterError::SERVER_DEVICE_FAILURE;
    }
    return {};
}

bool ModbusAdapter::configure(const std::string& /*t_ip*/, uint16_t /*t_port*/) {
    if (!p_store_) {
        SGRN_ERROR_LOG("Modbus: PlcSchemaStore not set during configure");
        return false;
    }

    vmap_ = ::sgrn::scl::buildModbusVirtualMap(*p_store_);

    // Fresh map, fresh generations: the twin may have changed while we were
    // down, so previously recorded versions must not skip the first sync.
    // (Runs on the start caller thread, before the serve thread exists.)
    db_sync_versions_.clear();

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

    // Backstop for stalled masters: a socket that select() flagged readable
    // but then goes silent mid-PDU must not park the serve thread forever.
    // Normal traffic never hits this (readability implies a full PDU is
    // imminent); expiry surfaces as a receive error and drops that master.
    server_->setIndicationTimeout(2, 0);

    // Best-effort seed; serve-loop reads re-sync and now propagate failures.
    (void)syncArenaToMapping();

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
    // Single-threaded multiplex: one select() over the listen socket plus
    // every connected master, at most one request per ready socket per pass.
    // Masters are served round-robin — a persistent or idle connection can
    // neither starve the others nor park this thread (see also the
    // indication-timeout backstop in configure()).
    while (runningFlag().load()) {
        if (!server_)
            break;

        const int listen_fd = server_->listenSocket();
        if (listen_fd == -1)
            break;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int max_fd = listen_fd;

        struct ReadyClient {
            int fd;
            std::string ip;
        };
        std::vector<ReadyClient> snapshot;
        {
            std::lock_guard<std::mutex> lk(clients_mutex_);
            snapshot.reserve(clients_.size());
            for (const auto& c : clients_) {
                FD_SET(c.fd, &rfds);
                max_fd = std::max(max_fd, c.fd);
                snapshot.push_back({c.fd, c.ip});
            }
        }

        struct timeval tv{0, 200'000};
        const int ready = ::select(max_fd + 1, &rfds, nullptr, nullptr, &tv);
        if (ready < 0) {
#ifdef _WIN32
            const int select_err = ::WSAGetLastError();
            if (select_err == WSAEINTR)
                continue;
#else
            if (errno == EINTR)
                continue;
#endif
            // Listen socket closed by onStopRequested(), or a fatal error.
            if (runningFlag().load()) {
                SGRN_WARN_LOG("Modbus: select failed, retrying");
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            break;
        }
        if (ready == 0) {
            sweepIdleClients();
            continue;
        }

        if (FD_ISSET(listen_fd, &rfds)) {
            auto accept_res = server_->accept();
            if (accept_res.hasError()) {
                if (runningFlag().load())
                    SGRN_WARN_LOG("Modbus: accept failed: {}", accept_res.error());
            } else {
                addClient(accept_res.value());
            }
        }

        for (const auto& [fd, ip] : snapshot) {
            if (!FD_ISSET(fd, &rfds))
                continue;
            if (!serveOneRequest(fd, ip))
                dropClient(fd);
        }

        sweepIdleClients();
    }
}

void ModbusAdapter::onStopRequested() {
    if (!server_)
        return;
    // Close the listen socket first: no new accepts, and select() wakes.
    server_->closeListenSocket();
    // Shut down every master socket (without closing): a serve thread parked
    // in select()/recv() wakes with an error instead of hanging the join.
    // Actual close()+erase happens on the serve thread (dropClient) or in
    // stop() after the join — never here, so no lock is held across blocking
    // calls and no fd is closed twice.
    std::vector<int> fds;
    {
        std::lock_guard<std::mutex> lk(clients_mutex_);
        fds.reserve(clients_.size());
        for (const auto& c : clients_)
            fds.push_back(c.fd);
    }
    for (int fd : fds)
        shutdownFd(fd);
}

void ModbusAdapter::stop() {
    // Base clears the flag, runs onStopRequested() above, then joins.
    ::sgrn::common::AdapterBase<ModbusAdapter>::stop();
    std::lock_guard<std::mutex> lk(clients_mutex_);
    for (const auto& c : clients_)
        closeFd(c.fd);
    clients_.clear();
    if (server_)
        server_->closeListenSocket();
    server_.reset();
    mapping_.reset();
}

void ModbusAdapter::addClient(int t_client_fd) {
    const std::string ip = peerIp(t_client_fd);
    const std::string label = ip.empty() ? "unknown" : ip;
    std::lock_guard<std::mutex> lk(clients_mutex_);
    if (clients_.size() >= kMaxMasters) {
        SGRN_WARN_LOG("Modbus: master limit ({}) reached, refusing {}", kMaxMasters, label);
        closeFd(t_client_fd);
        return;
    }
    clients_.push_back(ClientSlot{t_client_fd, ip, std::chrono::steady_clock::now()});
    SGRN_INFO_LOG("Modbus: master {} connected ({}/{} slots)", label, clients_.size(), kMaxMasters);
}

void ModbusAdapter::dropClient(int t_client_fd) {
    std::lock_guard<std::mutex> lk(clients_mutex_);
    auto it = std::find_if(clients_.begin(), clients_.end(), [&](const ClientSlot& c) { return c.fd == t_client_fd; });
    if (it == clients_.end())
        return;
    closeFd(it->fd);
    clients_.erase(it);
}

void ModbusAdapter::touchClient(int t_client_fd) {
    std::lock_guard<std::mutex> lk(clients_mutex_);
    for (auto& c : clients_) {
        if (c.fd == t_client_fd) {
            c.last_activity = std::chrono::steady_clock::now();
            break;
        }
    }
}

void ModbusAdapter::sweepIdleClients() {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(clients_mutex_);
    for (auto it = clients_.begin(); it != clients_.end();) {
        if (now - it->last_activity > kMasterIdleTimeout) {
            SGRN_INFO_LOG("Modbus: dropping idle master {}", it->ip.empty() ? "unknown" : it->ip);
            closeFd(it->fd);
            it = clients_.erase(it);
        } else {
            ++it;
        }
    }
}

bool ModbusAdapter::serveOneRequest(int t_client_fd, const std::string& t_client_ip) {
    if (!server_ || !mapping_)
        return false;

    // The shared libmodbus context is re-pointed at this master for the
    // duration of this one request. Requests are strictly sequential on the
    // single serve thread, so no two masters ever interleave here.
    server_->setClientSocket(t_client_fd);

    std::vector<uint8_t> query(wrappers::modbus::kTcpMaxAduLength);

    auto rc_res = server_->receive(query.data(), static_cast<int>(query.size()));
    if (rc_res.hasError())
        return false; // disconnect, stall timeout, or garbage — drop the master

    const int rc = rc_res.value();
    if (rc < kMbapLen + 1)
        return false; // runt frame, cannot even read the function code

    touchClient(t_client_fd);

    const uint8_t fc = query[static_cast<size_t>(kMbapLen)];
    const std::string& client_ip = t_client_ip;

    if (isReadFC(fc)) {
        if (auto sync_res = syncArenaToMapping(); !sync_res) {
            SGRN_WARN_LOG("Modbus: read sync failed: {} (modbus_exception=0x{:02x})", sync_res.error(), toExceptionCode(sync_res.error()));
            if (server_->replyException(query.data(), toExceptionCode(sync_res.error())).hasError())
                return false;
            return true;
        }
        if (server_->reply(query.data(), rc, mapping_->raw()).hasError())
            return false;
    } else if (isWriteFC(fc)) {
        if (auto write_res = processWriteRequest(fc, query.data(), rc, client_ip); !write_res) {
            SGRN_WARN_LOG(
                "Modbus: write request failed: {} (modbus_exception=0x{:02x})", write_res.error(), toExceptionCode(write_res.error()));
            if (server_->replyException(query.data(), toExceptionCode(write_res.error())).hasError())
                return false;
            return true;
        }
        if (server_->reply(query.data(), rc, mapping_->raw()).hasError())
            return false;
    } else {
        if (server_->reply(query.data(), rc, mapping_->raw()).hasError())
            return false;
    }
    return true;
}

sgrn::Result<void, ::sgrn::common::ErrorClass> ModbusAdapter::syncArenaToMapping() {
    if (!mapping_)
        return ErrorClass::Internal;

    // Batch sync for registers using span API (single lock per DB instead of per entry)
    auto sync_reg_entries_batch = [&](const std::vector<ModbusVirtualEntry>& t_entries,
                                      uint16_t* tp_regs) -> sgrn::Result<void, ::sgrn::common::ErrorClass> {
        if (t_entries.empty())
            return {};

        // Collect all read spans and buffers for batch operation
        std::vector<DbMemorySpan> spans;
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
            SGRN_WARN_LOG("Modbus syncRegEntries batch read failed (DB scope): {} (modbus_exception=0x{:02x})", r.error(),
                toExceptionCode(r.error()));
            return r.error();
        }

        // Encode all successfully-read buffers
        for (size_t i = 0; i < t_entries.size(); ++i) {
            encodeToRegisters(t_entries[i], buffers[i].data(), tp_regs + t_entries[i].reg_start);
        }
        return {};
    };

    // Batch sync for bits using span API
    auto sync_bit_entries_batch = [&](const std::vector<ModbusVirtualEntry>& t_entries,
                                      uint8_t* tp_bits) -> sgrn::Result<void, ::sgrn::common::ErrorClass> {
        if (t_entries.empty())
            return {};

        // Collect all read spans and buffers for batch operation
        std::vector<DbMemorySpan> spans;
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
            return r.error();
        }

        // Encode all successfully-read buffers
        for (size_t i = 0; i < t_entries.size(); ++i) {
            encodeToBits(t_entries[i], buffers[i].data(), tp_bits + t_entries[i].reg_start);
        }
        return {};
    };

    // Invoke batch sync per DB, skipping DBs whose write generation has not
    // moved since our last sync (see IMemoryPort::dbVersion seqlock
    // discipline: the recorded generation is always the pre-sync one, so a
    // write racing the sync only ever costs one redundant re-sync — it can
    // never be missed). Unknown generations (0) always sync.
    auto sync_db = [&](uint16_t t_db) -> sgrn::Result<void, ::sgrn::common::ErrorClass> {
        const uint64_t gen = getMemory().dbVersion(t_db);
        auto known = db_sync_versions_.find(t_db);
        if (gen != 0 && known != db_sync_versions_.end() && known->second == gen)
            return {}; // clean — skip all of this DB's entries
        auto by_db = [&](const std::vector<ModbusVirtualEntry>& t_all) {
            std::vector<ModbusVirtualEntry> out;
            for (const auto& e : t_all) {
                if (e.db_number == t_db)
                    out.push_back(e);
            }
            return out;
        };
        if (auto r = sync_reg_entries_batch(by_db(vmap_.holding), mapping_->registers()); !r)
            return r;
        if (auto r = sync_reg_entries_batch(by_db(vmap_.input), mapping_->inputRegisters()); !r)
            return r;
        if (auto r = sync_bit_entries_batch(by_db(vmap_.coil), mapping_->bits()); !r)
            return r;
        if (auto r = sync_bit_entries_batch(by_db(vmap_.discrete), mapping_->inputBits()); !r)
            return r;
        if (gen != 0)
            db_sync_versions_[t_db] = gen;
        return {};
    };

    // Distinct DBs across all four mapping spaces, ascending for stable logs.
    std::vector<uint16_t> dbs;
    auto collect_dbs = [&](const std::vector<ModbusVirtualEntry>& t_entries) {
        for (const auto& e : t_entries) {
            if (std::find(dbs.begin(), dbs.end(), e.db_number) == dbs.end())
                dbs.push_back(e.db_number);
        }
    };
    collect_dbs(vmap_.holding);
    collect_dbs(vmap_.input);
    collect_dbs(vmap_.coil);
    collect_dbs(vmap_.discrete);
    std::sort(dbs.begin(), dbs.end());

    for (uint16_t db : dbs) {
        if (auto r = sync_db(db); !r)
            return r;
    }
    return {};
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
sgrn::Result<void, ::sgrn::common::ErrorClass> ModbusAdapter::syncEntryToArena(const ModbusVirtualEntry& t_entry) {
    if (!mapping_) {
        return ErrorClass::Internal;
    }

    std::string json_val;

    if (t_entry.type == DataType::Bool) {
        if (t_entry.reg_start >= mapping_->nbBits()) {
            SGRN_WARN_LOG("Modbus: entry '{}' out of mapping bounds, skipping", t_entry.field_path);
            return ErrorClass::Internal;
        }
        const bool v = mapping_->bits()[t_entry.reg_start] != 0;
        json_val = v ? "true" : "false";
    } else {
        if (t_entry.reg_start + t_entry.reg_count > mapping_->nbRegisters()) {
            SGRN_WARN_LOG("Modbus: entry '{}' out of mapping bounds, skipping", t_entry.field_path);
            return ErrorClass::Internal;
        }
        const uint16_t* p_regs = mapping_->registers() + t_entry.reg_start;
        json_val = decodeRegisters(t_entry, p_regs);
    }

    if (json_val.empty())
        return ErrorClass::Internal;

    auto res = getMemory().updateField(t_entry.db_number, t_entry.field_path, json_val);
    if (res.hasError()) {
        SGRN_WARN_LOG("Modbus: updateField DB{}/'{}'  failed: {}", t_entry.db_number, t_entry.field_path, toString(res.error()));
        return res.error();
    }
    return {};
}

sgrn::Result<void, ::sgrn::common::ErrorClass> ModbusAdapter::stageWriteRequest(
    uint8_t t_function, uint16_t t_address, uint16_t t_count, const uint8_t* tp_pdu) {
    if (!mapping_) {
        return ErrorClass::Internal;
    }

    // Mirror libmodbus's own address checks (it answers 0x02 itself); value
    // validation stays libmodbus's job in reply().
    // NOTE the PDU shapes: FC05/FC06 carry addr(2)+value(2) — value at [2..3].
    // FC15/FC16 carry addr(2)+count(2)+bytecount(1)+data — data at [5..].
    const bool bit_space = (t_function == 0x05 || t_function == 0x0F);
    if (bit_space) {
        if (static_cast<size_t>(t_address) + t_count > static_cast<size_t>(mapping_->nbBits()))
            return ErrorClass::NotFound;
        if (t_function == 0x05) {
            const uint16_t v = (static_cast<uint16_t>(tp_pdu[2]) << 8) | tp_pdu[3];
            if (v != 0xFF00 && v != 0x0000)
                return ErrorClass::OutOfRange;
            mapping_->bits()[t_address] = (v == 0xFF00) ? 1 : 0;
        } else {
            for (uint16_t i = 0; i < t_count; ++i)
                mapping_->bits()[t_address + i] = ((tp_pdu[5 + i / 8] >> (i % 8)) & 1);
        }
    } else {
        if (static_cast<size_t>(t_address) + t_count > static_cast<size_t>(mapping_->nbRegisters()))
            return ErrorClass::NotFound;
        uint16_t* p_regs = mapping_->registers();
        if (t_function == 0x06) {
            p_regs[t_address] = (static_cast<uint16_t>(tp_pdu[2]) << 8) | tp_pdu[3];
        } else {
            for (uint16_t i = 0; i < t_count; ++i)
                p_regs[t_address + i] = (static_cast<uint16_t>(tp_pdu[5 + 2 * i]) << 8) | tp_pdu[5 + 2 * i + 1];
        }
    }
    return {};
}

sgrn::Result<void, ::sgrn::common::ErrorClass> ModbusAdapter::processWriteRequest(
    uint8_t t_function, const uint8_t* tp_query, int t_query_len, const std::string& t_client_ip) {
    // PDU past the MBAP header + function code: addr(2) count(2) [data...].
    if (tp_query == nullptr || t_query_len < kMbapLen + 1 + 4)
        return ErrorClass::OutOfRange;
    const uint8_t* p_pdu = tp_query + kMbapLen + 1;
    const uint16_t mb_addr = (static_cast<uint16_t>(p_pdu[0]) << 8) | p_pdu[1];
    // FC05/FC06 carry addr(2)+value(2) with NO count field — the single
    // target is implicit (Modbus spec §6.5, §6.6). Reading p_pdu[2..3] as a
    // count would mistake the value bytes for one.
    const bool single_write = (t_function == 0x05 || t_function == 0x06);
    const uint16_t count = single_write ? 1 : static_cast<uint16_t>((static_cast<uint16_t>(p_pdu[2]) << 8) | p_pdu[3]);

    // Data bytes must be present: value(2) for single writes, bytecount + payload for multi.
    size_t need = static_cast<size_t>(kMbapLen) + 1 + 4 + (single_write ? 0 : 2);
    if (t_function == 0x0F || t_function == 0x10) {
        if (t_query_len < kMbapLen + 1 + 5)
            return ErrorClass::OutOfRange;
        need = static_cast<size_t>(kMbapLen) + 1 + 5 + p_pdu[4];
    }
    if (t_query_len < static_cast<int>(need))
        return ErrorClass::OutOfRange;

    if (auto r = stageWriteRequest(t_function, mb_addr, count, p_pdu); !r)
        return r;

    const bool bit_space = isBitFC(t_function);
    const auto& entries = bit_space ? (t_function == 0x05 || t_function == 0x0F ? vmap_.coil : vmap_.discrete) : vmap_.holding;

    for (const auto& entry : entries) {
        if (entry.read_only)
            continue;
        const int written_end = mb_addr + count;
        const int entry_end = entry.reg_start + entry.reg_count;
        if (entry.reg_start < written_end && entry_end > mb_addr) {
            auto sec_mgr = getSecurityManager();
            if (!sec_mgr || sec_mgr->authorizeModbus(t_client_ip, entry.db_number)) {
                if (auto r = syncEntryToArena(entry); !r)
                    return r;
            } else {
                SGRN_WARN_LOG("Modbus: Write to DB{} denied for {}", entry.db_number, t_client_ip);
                // Denied-but-present must still fail the wire request (was:
                // skip + success reply). Internal → 0x04 keeps the mapping
                // layout undisclosed beyond what bounds already reveal.
                return ErrorClass::Internal;
            }
        }
    }
    // updateField() only queues twin writes — flush once per request so the
    // values (and their dirty/telemetry fan-out) actually land. Raw
    // writeDbMemory/writeBit need no flush; only this queued path does.
    getMemory().flushCommands();
    return {};
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
