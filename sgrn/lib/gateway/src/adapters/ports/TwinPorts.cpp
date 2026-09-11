#include <sgrn/gateway/adapters/ports/TwinPorts.hpp>
#include <sgrn/gateway/common/ErrorClass.hpp>
#include <sgrn/gateway/security/SecurityManager.hpp>
#include <sgrn/gateway/twin/PlcCommandProcessor.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>

namespace sgrn::gateway::adapters::ports
{

TwinMemoryPort::TwinMemoryPort(twin::PlcMemory& t_memory)
    : memory_(t_memory) {
}

bool TwinMemoryPort::isReady() const {
    return memory_.state() != nullptr;
}

std::vector<uint16_t> TwinMemoryPort::topLevelDbNumbers() const {
    if (!isReady())
        return {};
    return memory_.state()->topLevelNumbers();
}

std::optional<size_t> TwinMemoryPort::dbSize(uint16_t t_db_number) const {
    if (!isReady())
        return std::nullopt;
    const auto* p_entry = memory_.state()->findSegmentById(t_db_number);
    if (!p_entry)
        return std::nullopt;
    return p_entry->size;
}

sgrn::Result<void, ::sgrn::common::ErrorClass> TwinMemoryPort::readDbMemory(
    uint16_t t_db_number, size_t t_offset, size_t t_size, uint8_t* tp_buffer) {
    auto r = memory_.readDbMemory(t_db_number, t_offset, t_size, tp_buffer);
    if (r.hasError())
        return ::sgrn::gateway::common::classify(r.error());
    return {};
}

sgrn::Result<void, ::sgrn::common::ErrorClass> TwinMemoryPort::readDbMemory(std::span<const ::sgrn::common::DbMemorySpan> t_spans) {
    std::vector<twin::DbMemorySpan> spans;
    spans.reserve(t_spans.size());
    for (const auto& s : t_spans)
        spans.push_back({.db = s.db, .offset = s.offset, .size = s.size, .p_buffer = s.p_buffer});
    auto r = memory_.readDbMemory(std::span(spans));
    if (r.hasError())
        return ::sgrn::gateway::common::classify(r.error());
    return {};
}

sgrn::Result<void, ::sgrn::common::ErrorClass> TwinMemoryPort::writeDbMemory(
    uint16_t t_db_number, size_t t_offset, size_t t_size, const uint8_t* tp_buffer) {
    auto r = memory_.writeDbMemory(t_db_number, t_offset, t_size, tp_buffer);
    if (r.hasError())
        return ::sgrn::gateway::common::classify(r.error());
    return {};
}

sgrn::Result<void, ::sgrn::common::ErrorClass> TwinMemoryPort::writeBit(
    uint16_t t_db_number, size_t t_byte_offset, int t_bit_index, bool t_value) {
    auto r = memory_.writeBit(t_db_number, t_byte_offset, t_bit_index, t_value);
    if (r.hasError())
        return ::sgrn::gateway::common::classify(r.error());
    return {};
}

sgrn::Result<void, ::sgrn::common::ErrorClass> TwinMemoryPort::updateField(
    uint16_t t_db_number, const std::string& t_field_path, const std::string& t_value_json) {
    auto r = memory_.updateField(t_db_number, t_field_path, t_value_json);
    if (r.hasError())
        return ::sgrn::gateway::common::classify(r.error());
    return {};
}

void TwinMemoryPort::flushCommands() {
    if (auto* p_proc = memory_.processor())
        p_proc->processCommands();
}

uint64_t TwinMemoryPort::dbVersion(uint16_t t_db_number) const {
    const twin::PlcState* p_state = memory_.state();
    if (!p_state)
        return 0;
    // DB number → node version via the segment registry. O(DBs) scan plus
    // one hashed node lookup; versions are lock-free atomics bumped on every
    // write up the ancestor chain, so this is safe to call per poll.
    for (const auto& [name, seg] : p_state->segments()) {
        if (seg && seg->id == t_db_number) {
            const twin::PlcNode* p_node = p_state->find(name);
            if (p_node && p_node->state_)
                return p_node->state_->version_.load(std::memory_order_acquire);
            return 0;
        }
    }
    return 0;
}

GatewaySecurityPolicy::GatewaySecurityPolicy(std::shared_ptr<SecurityManager> tsp_security_manager)
    : security_(std::move(tsp_security_manager)) {
}

bool GatewaySecurityPolicy::authorizeWrite(int t_sender_id, int t_area, uint16_t t_db_number) {
    return security_ ? security_->authorizeWrite(t_sender_id, t_area, t_db_number) : false;
}

bool GatewaySecurityPolicy::authorizeModbus(const std::string& t_client_ip, uint16_t t_db_number) {
    return !security_ || security_->authorizeModbus(t_client_ip, t_db_number);
}

bool GatewaySecurityPolicy::authorizeEip(const std::string& t_client_ip) {
    return !security_ || security_->authorizeEip(t_client_ip);
}

} // namespace sgrn::gateway::adapters::ports
