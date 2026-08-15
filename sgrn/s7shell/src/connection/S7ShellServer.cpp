#include "sgrn/s7shell/connection/S7ShellServer.hpp"
#include <fmt/core.h>
#include <algorithm>

namespace sgrn::s7shell::shell
{

namespace
{
constexpr int S7WLChar = 0x03;
constexpr int S7WLInt = 0x05;
constexpr int S7WLDInt = 0x07;

int normalizeServerAreaCode(int t_area_code) {
    switch (t_area_code) {
        case ::S7AreaPE:
            return ::srvAreaPE;
        case ::S7AreaPA:
            return ::srvAreaPA;
        case ::S7AreaMK:
            return ::srvAreaMK;
        case ::S7AreaDB:
            return ::srvAreaDB;
        case ::S7AreaCT:
            return ::srvAreaCT;
        case ::S7AreaTM:
            return ::srvAreaTM;
        default:
            return t_area_code;
    }
}

std::optional<size_t> requestByteSize(const TS7Tag& t_tag) {
    switch (t_tag.WordLen) {
        case S7WLBit:
        case S7WLByte:
        case S7WLChar:
            return static_cast<size_t>(std::max(1, t_tag.Size));
        case S7WLWord:
        case S7WLInt:
        case S7WLCounter:
        case S7WLTimer:
            return static_cast<size_t>(std::max(1, t_tag.Size)) * 2U;
        case S7WLDWord:
        case S7WLDInt:
        case S7WLReal:
            return static_cast<size_t>(std::max(1, t_tag.Size)) * 4U;
        default:
            return std::nullopt;
    }
}
} // namespace

ScriptS7Server::ScriptS7Server(runtime::PlcRuntimeSPtr tsp_rt, const std::string& t_ip, uint16_t t_port)
    : runtime_(std::move(tsp_rt))
    , ip_(t_ip)
    , port_(t_port) {
}

ScriptS7Server::~ScriptS7Server() {
    stopServer();
}

void ScriptS7Server::addRef() {
    ++ref_count_;
}

void ScriptS7Server::release() {
    if (--ref_count_ == 0)
        delete this;
}

std::shared_ptr<::sgrn::s7shell::runtime::PlcRuntime> ScriptS7Server::getRuntime() const {
    return runtime_;
}

sgrn::Result<void, ::sgrn::gateway::wrappers::s7::S7Error> ScriptS7Server::startServer() {
    return start(ip_, port_);
}

void ScriptS7Server::stopServer() {
    (void)stop();
}

void ScriptS7Server::configureBeforeStart() {
    server_->SetRWAreaCallback(s7RequestCallback, this);
}

int S7API ScriptS7Server::s7RequestCallback(void* tp_usr_ptr, int t_sender, int t_operation, PS7Tag t_tag, void* tp_data) {
    auto* p_srv = static_cast<ScriptS7Server*>(tp_usr_ptr);
    if (!p_srv || !t_tag) {
        return ::evrErrException;
    }

    auto size = requestByteSize(*t_tag);
    if (!size.has_value()) {
        return ::evrErrTransportSize;
    }

    const size_t start = t_tag->Start < 0 ? 0U : static_cast<size_t>(t_tag->Start);
    const size_t bytes = size.value();
    const int area = normalizeServerAreaCode(t_tag->Area);

    if (area == ::srvAreaDB) {
        const uint16_t db_num = static_cast<uint16_t>(t_tag->DBNumber);
        auto tsp_rt = p_srv->runtime_;
        if (!tsp_rt) {
            return ::evrErrException;
        }

        auto& memory = tsp_rt->getMemory();
        auto* p_state = memory.state();
        if (!p_state) {
            return ::evrErrException;
        }

        const auto* p_entry = p_state->findSegmentById(db_num);
        if (!p_entry) {
            return ::evrErrAreaNotFound;
        }
        if (start + bytes > p_entry->size) {
            return ::evrErrOutOfRange;
        }

        if (t_operation == OperationRead) {
            if (!(memory.readDbMemory(db_num, start, bytes, static_cast<uint8_t*>(tp_data)))) {
                return ::evrErrOutOfRange;
            }
        } else {
            if (auto r = memory.writeDbMemory(db_num, start, bytes, static_cast<const uint8_t*>(tp_data)); !r) {
                fmt::print(stderr, "[ERROR] S7 PUT to DB{} offset={} size={} failed: {}\n", db_num, start, bytes, r.error());
                return ::evrErrOutOfRange;
            }
            tsp_rt->markDirty(db_num, start, bytes);
        }
        return 0;
    }
    return ::evrErrAreaNotFound;
}

} // namespace sgrn::s7shell::shell
