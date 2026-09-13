#include <fmt/core.h>
#include <sgrn/gateway/adapters/s7/S7ProtocolAdapter.hpp>
#include <stdexcept>
#include <string>

using ::sgrn::wrappers::s7::S7Error;

#include <sgrn/gateway/adapters/s7/TypeTranslation.hpp>

namespace sgrn::gateway::adapters::s7
{

S7ProtocolAdapter::S7ProtocolAdapter(IMemoryPort& t_memory, std::shared_ptr<ISecurityPolicy> tsp_security_policy)
    : memory_(t_memory)
    , security_policy_(std::move(tsp_security_policy)) {
}

S7ProtocolAdapter::~S7ProtocolAdapter() {
    (void)S7Server::stop();

    // Unregister memory areas that we registered
    for (uint16_t db_num : memory_.topLevelDbNumbers()) {
        server_->UnregisterArea(::srvAreaDB, static_cast<word>(db_num));
    }
    for (const auto& [key, buf] : area_buffers_) {
        (void)buf;
        server_->UnregisterArea(key.first, key.second);
    }
}

void S7ProtocolAdapter::configureBeforeStart() {
    int32_t max_clients = static_cast<int32_t>(max_clients_);
    int32_t pdu_size = static_cast<int32_t>(pdu_size_);
    server_->SetParam(p_i32_MaxClients, &max_clients);
    server_->SetParam(p_i32_PDURequest, &pdu_size);
    server_->SetRWAreaCallback(s7RequestCallback, this);
}

sgrn::Result<void, S7Error> S7ProtocolAdapter::bindToPlcMemory() {
    if (!memory_.isReady()) {
        return S7Error::Unknown;
    }

    for (uint16_t db_num : memory_.topLevelDbNumbers()) {
        const auto size_opt = memory_.dbSize(db_num);
        if (!size_opt) {
            continue;
        }

        // NOTE: We do NOT call server_->RegisterArea here for DBs anymore.
        // By NOT registering the area, Snap7 will trigger s7RequestCallback
        // for every read/write request. This allows us to intercept writes,
        // call memory.writeDbMemory() (which marks the DB as dirty), and
        // propagate changes to the TelemetryBroker/WebSocket layers.

        semantic_spans_[{::srvAreaDB, db_num}] = {{0, static_cast<int>(*size_opt)}};
    }

    return {};
}

int S7API S7ProtocolAdapter::s7RequestCallback(void* tp_usr_ptr, int t_sender, int t_operation, PS7Tag t_tag, void* tp_data) {

    auto* p_adapter = static_cast<S7ProtocolAdapter*>(tp_usr_ptr);

    SGRN_RETURN_IF(!p_adapter || !t_tag, ::evrErrException);

    std::optional<size_t> size_opt = TypeTranslation::requestByteSize(*t_tag);

    SGRN_RETURN_IF_NULL(size_opt, evrErrTransportSize);

    const size_t start = t_tag->Start < 0 ? 0U : static_cast<size_t>(t_tag->Start);
    const size_t bytes = size_opt.value();
    const int area = TypeTranslation::normalizeServerAreaCode(t_tag->Area);

    if (area == ::srvAreaDB) {
        const uint16_t db_num = static_cast<uint16_t>(t_tag->DBNumber);
        if (!p_adapter->memory_.isReady()) {
            return ::evrErrException;
        }
        const auto db_size_opt = p_adapter->memory_.dbSize(db_num);
        if (!db_size_opt) {
            return ::evrErrAreaNotFound;
        }
        if (start + bytes > *db_size_opt) {
            return ::evrErrOutOfRange;
        }

        if (t_operation == OperationRead) {
            if (auto r = p_adapter->memory_.readDbMemory(db_num, start, bytes, static_cast<uint8_t*>(tp_data)); !r) {
                return TypeTranslation::evrCodeForError(r.error());
            }
        } else {
            if (!p_adapter->security_policy_->authorizeWrite(t_sender, t_tag->Area, db_num)) {
                fmt::print(stderr, "[AUDIT] DENY  S7 PUT  DB{} sender={}\n", db_num, t_sender);
                return ::evrErrException;
            }

            if (auto r = p_adapter->memory_.writeDbMemory(db_num, start, bytes, static_cast<const uint8_t*>(tp_data)); r.hasError()) {
                fmt::print(stderr, "[ERROR] S7 PUT to DB{} offset={} size={} failed – writeDbMemory failed: {}\n", db_num, start, bytes,
                    r.error());
                return TypeTranslation::evrCodeForError(r.error());
            }
        }
        return 0;
    }
    return ::evrErrAreaNotFound;
}

bool S7ProtocolAdapter::isRequestInSemanticSpace(const TS7Tag& t_tag) const {
    const int start = t_tag.Start;
    const int t_size = std::max(1, t_tag.Size);
    const int area = TypeTranslation::normalizeServerAreaCode(t_tag.Area);

    if (area == ::srvAreaDB) {
        return memory_.isReady() && memory_.dbSize(static_cast<uint16_t>(t_tag.DBNumber)).has_value();
    }

    const std::pair<int, word> key = {area, static_cast<word>(t_tag.DBNumber)};
    const auto it = semantic_spans_.find(key);
    if (it == semantic_spans_.end())
        return false;

    const int end = start + t_size;
    for (const auto& [span_start, span_size] : it->second) {
        if (start >= span_start && end <= span_start + span_size)
            return true;
    }
    return false;
}

sgrn::Result<void, S7Error> S7ProtocolAdapter::registerSemanticArea(int t_area_code, word t_index, size_t t_size) {
    auto& buffer = area_buffers_[{t_area_code, t_index}];
    if (buffer)
        server_->UnregisterArea(t_area_code, t_index);
    buffer = std::make_shared<sgrn::SharedBuffer>(t_size);

    const int res = server_->RegisterArea(t_area_code, t_index, buffer->data(), static_cast<word>(buffer->size()));
    if (res != 0) {
        area_buffers_.erase({t_area_code, t_index});
        return makeStatus(res);
    }
    return {};
}

} // namespace sgrn::gateway::adapters::s7
