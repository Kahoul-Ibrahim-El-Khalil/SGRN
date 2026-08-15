#include <sgrn/gateway/twin/utils.hpp>
#include <sgrn/gateway/wrappers/s7/S7Server.hpp>

namespace sgrn::gateway::wrappers::s7
{

S7Server::S7Server()
    : server_(std::make_unique<TS7Server>()) {
}

S7Server::~S7Server() {
    if (server_) {
        (void)server_->Stop();
    }
}

S7Server::S7Server(S7Server&& t_other) noexcept
    : server_(std::move(t_other.server_)) {
}

S7Server& S7Server::operator=(S7Server&& t_other) noexcept {
    if (this != &t_other) {
        server_ = std::move(t_other.server_);
    }
    return *this;
}

std::pair<int, word> S7Server::areaKey(int t_area_code, word t_index) {
    return {t_area_code, t_index};
}

sgrn::Result<void, ::sgrn::gateway::wrappers::s7::S7Error> S7Server::start(const std::string& t_ip, uint16_t t_port) {
    if (!server_) {
        return S7Error{S7ProtocolCode::NotConnected};
    }

    int port_status = server_->SetParam(p_u16_LocalPort, &t_port);
    if (port_status != 0) {
        return makeStatus(port_status);
    }

    configureBeforeStart();

    int res = server_->StartTo(t_ip.c_str());
    if (res != 0) {
        return makeStatus(res);
    }

    res = server_->SetCpuStatus(S7CpuStatusRun);
    return makeStatus(res);
}

sgrn::Result<void, ::sgrn::gateway::wrappers::s7::S7Error> S7Server::stop() {
    if (!server_) {
        return S7Error{S7ProtocolCode::NotConnected};
    }
    int res = server_->Stop();
    return makeStatus(res);
}

int S7Server::statusStatus() const {
    if (!server_) {
        return 0;
    }
    return server_->ServerStatus();
}

bool S7Server::isRunning() const {
    return statusStatus() == 1;
}

int S7Server::clientsCount() const {
    if (!server_) {
        return 0;
    }
    return server_->ClientsCount();
}

int S7Server::getCpuStatus() const {
    if (!server_) {
        return 0;
    }
    return server_->GetCpuStatus();
}

sgrn::Result<void, ::sgrn::gateway::wrappers::s7::S7Error> S7Server::setCpuStatus(int t_status) {
    if (!server_) {
        return S7Error{
            S7ProtocolCode::NotConnected,
        };
    }
    int res = server_->SetCpuStatus(t_status);
    return makeStatus(res);
}

void S7Server::setEventsCallback(pfn_SrvCallBack t_callback, void* tp_usr_ptr) {
    if (server_) {
        server_->SetEventsCallback(t_callback, tp_usr_ptr);
    }
}

std::optional<S7ServerEvent> S7Server::pickEvent() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!server_) {
        return std::nullopt;
    }

    TSrvEvent ev;
    if (server_->PickEvent(&ev)) {
        return S7ServerEvent{.timestamp = static_cast<int64_t>(ev.EvtTime),
            .sender = ev.EvtSender,
            .code = ev.EvtCode,
            .ret_code = ev.EvtRetCode,
            .param1 = ev.EvtParam1,
            .param2 = ev.EvtParam2,
            .param3 = ev.EvtParam3,
            .param4 = ev.EvtParam4};
    }
    return std::nullopt;
}

std::string S7Server::eventText(const S7ServerEvent& t_event) const {
    TSrvEvent ev{.EvtTime = static_cast<time_t>(t_event.timestamp),
        .EvtSender = t_event.sender,
        .EvtCode = t_event.code,
        .EvtRetCode = t_event.ret_code,
        .EvtParam1 = t_event.param1,
        .EvtParam2 = t_event.param2,
        .EvtParam3 = t_event.param3,
        .EvtParam4 = t_event.param4};
    char buf[1024];
    if (Srv_EventText(&ev, buf, sizeof(buf)) == 0) {
        return std::string(buf);
    }
    return "";
}

sgrn::Result<void, ::sgrn::gateway::wrappers::s7::S7Error> S7Server::makeStatus(int t_error_code) const {
    if (t_error_code == 0) {
        return {};
    }

    char buffer[256];
    Srv_ErrorText(t_error_code, buffer, sizeof(buffer));
    return Error(fromSnap7(t_error_code, std::string(buffer)));
}

} // namespace sgrn::gateway::wrappers::s7
