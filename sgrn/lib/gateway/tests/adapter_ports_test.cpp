// Adapter port decoupling test.
//
// Proves the southbound protocol adapters (s7, modbus, ethernetip) build and
// behave against abstract sgrn::common ports with NO twin/security types:
// this TU includes no twin or security header, and the test target links no
// twin/security library — a link of this binary would fail on any leftover
// concrete dependency. Fakes stand in for TwinMemoryPort/GatewaySecurityPolicy.

#include <sgrn/common/AdapterBase.hpp>
#include <sgrn/common/ErrorClass.hpp>
#include <sgrn/common/MemoryPort.hpp>
#include <sgrn/common/SecurityPort.hpp>
#include <sgrn/gateway/adapters/ethernetip/EipAdapter.hpp>
#include <sgrn/gateway/adapters/ethernetip/errors.hpp>
#include <sgrn/gateway/adapters/modbus/ModbusAdapter.hpp>
#include <sgrn/gateway/adapters/modbus/errors.hpp>
#include <sgrn/gateway/adapters/opcua/errors.hpp>
#include <sgrn/gateway/adapters/s7/TypeTranslation.hpp>
#include <sgrn/gateway/adapters/s7/fromPlcMemoryErrorToS7MemoryError.hpp>

#include <fmt/core.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;

#define CHECK(cond)                                                                                                                        \
    do {                                                                                                                                   \
        if (!(cond)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                                    \
        }                                                                                                                                  \
    } while (0)

using sgrn::common::DbMemorySpan;
using sgrn::common::ErrorClass;

/// In-memory IMemoryPort with twin-identical failure semantics.
class FakeMemoryPort : public sgrn::common::IMemoryPort {
public:
    bool ready_{true};
    std::optional<ErrorClass> sticky_error_;
    std::vector<std::string> update_log_;

    bool isReady() const override {
        return ready_;
    }
    std::vector<uint16_t> topLevelDbNumbers() const override {
        std::vector<uint16_t> out;
        for (const auto& [db, _] : dbs_)
            out.push_back(db);
        return out;
    }
    std::optional<size_t> dbSize(uint16_t t_db) const override {
        auto it = dbs_.find(t_db);
        if (it == dbs_.end())
            return std::nullopt;
        return it->second.size();
    }
    sgrn::Result<void, ErrorClass> readDbMemory(uint16_t t_db, size_t t_off, size_t t_size, uint8_t* tp_out) override {
        if (sticky_error_)
            return *sticky_error_;
        if (!ready_)
            return ErrorClass::NotInitialized;
        auto it = dbs_.find(t_db);
        if (it == dbs_.end())
            return ErrorClass::NotFound;
        if (tp_out == nullptr && t_size > 0)
            return ErrorClass::Internal;
        if (t_off > it->second.size() || t_size > it->second.size() - t_off)
            return ErrorClass::OutOfRange;
        std::memcpy(tp_out, it->second.data() + t_off, t_size);
        return {};
    }
    sgrn::Result<void, ErrorClass> readDbMemory(std::span<const DbMemorySpan> t_spans) override {
        for (const auto& s : t_spans) {
            auto r = readDbMemory(s.db, s.offset, s.size, s.p_buffer);
            if (r.hasError())
                return r.error();
        }
        return {};
    }
    sgrn::Result<void, ErrorClass> writeDbMemory(uint16_t t_db, size_t t_off, size_t t_size, const uint8_t* tp_in) override {
        if (sticky_error_)
            return *sticky_error_;
        if (!ready_)
            return ErrorClass::NotInitialized;
        auto it = dbs_.find(t_db);
        if (it == dbs_.end())
            return ErrorClass::NotFound;
        if (tp_in == nullptr && t_size > 0)
            return ErrorClass::Internal;
        if (t_off > it->second.size() || t_size > it->second.size() - t_off)
            return ErrorClass::OutOfRange;
        std::memcpy(it->second.data() + t_off, tp_in, t_size);
        return {};
    }
    sgrn::Result<void, ErrorClass> writeBit(uint16_t t_db, size_t t_byte, int t_bit, bool t_val) override {
        if (sticky_error_)
            return *sticky_error_;
        if (!ready_)
            return ErrorClass::NotInitialized;
        auto it = dbs_.find(t_db);
        if (it == dbs_.end())
            return ErrorClass::NotFound;
        if (t_bit < 0 || t_bit > 7 || t_byte >= it->second.size())
            return ErrorClass::Internal;
        if (t_val)
            it->second[t_byte] = static_cast<uint8_t>(it->second[t_byte] | (1u << t_bit));
        else
            it->second[t_byte] = static_cast<uint8_t>(it->second[t_byte] & ~(1u << t_bit));
        return {};
    }
    sgrn::Result<void, ErrorClass> updateField(uint16_t t_db, const std::string& t_path, const std::string& t_json) override {
        if (sticky_error_)
            return *sticky_error_;
        if (!ready_)
            return ErrorClass::NotInitialized;
        if (dbs_.find(t_db) == dbs_.end())
            return ErrorClass::NotFound;
        update_log_.push_back(std::to_string(t_db) + ":" + t_path + "=" + t_json);
        return {};
    }

    std::map<uint16_t, std::vector<uint8_t>> dbs_;
};

class FakeSecurityPolicy : public sgrn::common::ISecurityPolicy {
public:
    bool write_ok_{true};
    bool modbus_ok_{true};
    bool eip_ok_{true};
    bool authorizeWrite(int, int, uint16_t) override {
        return write_ok_;
    }
    bool authorizeModbus(const std::string&, uint16_t) override {
        return modbus_ok_;
    }
    bool authorizeEip(const std::string&) override {
        return eip_ok_;
    }
};

// Minimal AdapterBase derivor: proves the CRTP base forwards ports.
struct ProbeAdapter : ::sgrn::common::AdapterBase<ProbeAdapter> {
    using Base = ::sgrn::common::AdapterBase<ProbeAdapter>;
    ProbeAdapter(sgrn::common::IMemoryPort& m, std::shared_ptr<sgrn::common::ISecurityPolicy> s)
        : Base(m, std::move(s)) {
    }
    bool configure(const std::string&, uint16_t) {
        return true;
    }
    void serveLoop() {
    }
    using Base::getMemory;
    using Base::getSecurityManager;
};

void checkMemoryPort() {
    FakeMemoryPort mem;
    mem.dbs_[1] = {0x10, 0x20, 0x30, 0x40};

    CHECK(mem.isReady());
    CHECK(mem.topLevelDbNumbers() == std::vector<uint16_t>{1});
    CHECK(mem.dbSize(1).value_or(0) == 4);
    CHECK(!mem.dbSize(99).has_value());

    uint8_t buf[2] = {};
    CHECK(!mem.readDbMemory(1, 1, 2, buf).hasError());
    CHECK(buf[0] == 0x20 && buf[1] == 0x30);

    const uint8_t w[1] = {0xFF};
    CHECK(!mem.writeDbMemory(1, 0, 1, w).hasError());
    CHECK(mem.dbs_[1][0] == 0xFF);

    CHECK(!mem.writeBit(1, 3, 0, true).hasError());
    CHECK(mem.dbs_[1][3] == 0x41);

    // Twin-identical failure buckets.
    CHECK(mem.readDbMemory(99, 0, 1, buf).error() == ErrorClass::NotFound);
    CHECK(mem.readDbMemory(1, 3, 2, buf).error() == ErrorClass::OutOfRange);
    CHECK(mem.readDbMemory(1, 0, 1, nullptr).error() == ErrorClass::Internal);
    CHECK(mem.writeBit(1, 0, 9, true).error() == ErrorClass::Internal);
    mem.ready_ = false;
    CHECK(mem.readDbMemory(1, 0, 1, buf).error() == ErrorClass::NotInitialized);
    mem.ready_ = true;

    // Batch read fans out to singles.
    uint8_t b0[2] = {}, b1[2] = {};
    mem.dbs_[2] = {0xAA, 0xBB};
    std::vector<DbMemorySpan> spans = {
        {.db = 1, .offset = 0, .size = 2, .p_buffer = b0}, {.db = 2, .offset = 0, .size = 2, .p_buffer = b1}};
    CHECK(!mem.readDbMemory(std::span(spans)).hasError());
    CHECK(b0[0] == 0xFF && b1[1] == 0xBB);
    spans[1].db = 77;
    CHECK(mem.readDbMemory(std::span(spans)).error() == ErrorClass::NotFound);

    // updateField is observed, not executed.
    CHECK(!mem.updateField(1, "speed", "12.5").hasError());
    CHECK(mem.update_log_.size() == 1);
    CHECK(mem.updateField(77, "x", "1").error() == ErrorClass::NotFound);
}

void checkSecurityPolicy() {
    FakeSecurityPolicy pol;
    CHECK(pol.authorizeWrite(0, 0, 1));
    CHECK(pol.authorizeModbus("10.0.0.1", 1));
    CHECK(pol.authorizeEip("10.0.0.1"));
    pol.write_ok_ = pol.modbus_ok_ = pol.eip_ok_ = false;
    CHECK(!pol.authorizeWrite(0, 0, 1));
    CHECK(!pol.authorizeModbus("10.0.0.1", 1));
    CHECK(!pol.authorizeEip("10.0.0.1"));
}

void checkWireCodes() {
    using sgrn::gateway::adapters::toUAStatusCode;
    using sgrn::gateway::adapters::ethernetip::toCipStatus;
    using sgrn::gateway::adapters::modbus::toExceptionCode;
    // Same four-way grouping the twin vocabulary used to carry (Task 1).
    CHECK(toExceptionCode(ErrorClass::NotInitialized) == 0x04);
    CHECK(toExceptionCode(ErrorClass::NotFound) == 0x02);
    CHECK(toExceptionCode(ErrorClass::OutOfRange) == 0x04);
    CHECK(toExceptionCode(ErrorClass::Internal) == 0x04);
    CHECK(toCipStatus(ErrorClass::NotInitialized) == 0x01);
    CHECK(toCipStatus(ErrorClass::NotFound) == 0x15);
    CHECK(toCipStatus(ErrorClass::OutOfRange) == 0x0F);
    CHECK(toCipStatus(ErrorClass::Internal) == 0x1F);
    CHECK(toUAStatusCode(ErrorClass::NotInitialized) == UA_STATUSCODE_BADSERVERNOTCONNECTED);
    CHECK(toUAStatusCode(ErrorClass::NotFound) == UA_STATUSCODE_BADNODEIDUNKNOWN);
    CHECK(toUAStatusCode(ErrorClass::OutOfRange) == UA_STATUSCODE_BADOUTOFRANGE);
    CHECK(toUAStatusCode(ErrorClass::Internal) == UA_STATUSCODE_BADINTERNALERROR);
    CHECK(::sgrn::wrappers::s7::fromMemoryErrorToS7Error(ErrorClass::NotInitialized) == ::sgrn::wrappers::s7::S7Error::NotConnected);
    CHECK(::sgrn::wrappers::s7::fromMemoryErrorToS7Error(ErrorClass::NotFound) == ::sgrn::wrappers::s7::S7Error::InvalidParam);
    CHECK(::sgrn::wrappers::s7::fromMemoryErrorToS7Error(ErrorClass::OutOfRange) == ::sgrn::wrappers::s7::S7Error::InvalidParam);
    CHECK(::sgrn::wrappers::s7::fromMemoryErrorToS7Error(ErrorClass::Internal) == ::sgrn::wrappers::s7::S7Error::InvalidParam);
    // Human + log surface.
    CHECK(sgrn::common::toString(ErrorClass::NotFound) == std::string_view{"segment not found"});
    CHECK(fmt::format("{}", ErrorClass::OutOfRange) == "range exceeds allowed space");
    // S7 server callback codes per class (replaces the old blanket OutOfRange).
    CHECK(sgrn::gateway::adapters::s7::TypeTranslation::evrCodeForError(ErrorClass::NotInitialized) == ::evrErrException);
    CHECK(sgrn::gateway::adapters::s7::TypeTranslation::evrCodeForError(ErrorClass::Internal) == ::evrErrException);
    CHECK(sgrn::gateway::adapters::s7::TypeTranslation::evrCodeForError(ErrorClass::NotFound) == ::evrErrAreaNotFound);
    CHECK(sgrn::gateway::adapters::s7::TypeTranslation::evrCodeForError(ErrorClass::OutOfRange) == ::evrErrOutOfRange);
}

void checkAdapterWiring() {
    FakeMemoryPort mem;
    mem.dbs_[1] = {0, 0, 0, 0};
    auto policy = std::make_shared<FakeSecurityPolicy>();

    // Base forwards the injected ports (no threads started).
    ProbeAdapter probe(mem, policy);
    uint8_t buf[1] = {};
    CHECK(!probe.getMemory().readDbMemory(1, 0, 1, buf).hasError());
    CHECK(probe.getSecurityManager()->authorizeModbus("x", 1));

    // Real adapters construct against fakes — compile+link proof that their
    // headers need no twin/security types. Never started (no sockets).
    {
        sgrn::gateway::adapters::modbus::ModbusAdapter modbus(mem, policy);
        sgrn::gateway::adapters::ethernetip::EipAdapter eip(mem, policy);
        (void)modbus;
        (void)eip;
    }
}

} // namespace

int main() {
    checkMemoryPort();
    checkSecurityPolicy();
    checkWireCodes();
    checkAdapterWiring();
    if (g_failures == 0)
        std::printf("adapter_ports_test: ALL CHECKS PASSED\n");
    else
        std::printf("adapter_ports_test: %d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
