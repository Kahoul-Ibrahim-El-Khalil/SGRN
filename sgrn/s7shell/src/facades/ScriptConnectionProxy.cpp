#include <sgrn/s7shell/connection/S7Connection.hpp>
#include <sgrn/s7shell/facades/ScriptConnectionProxy.hpp>
#include <sgrn/s7shell/utils/json_helpers.hpp>

#include <fmt/format.h>

namespace sgrn::s7shell::shell
{

ScriptS7ConnectionProxy::ScriptS7ConnectionProxy(ScriptS7Connection* tp_conn)
    : conn_(tp_conn) {
}

void ScriptS7ConnectionProxy::addRef() {
    ++ref_count_;
}

void ScriptS7ConnectionProxy::release() {
    if (--ref_count_ == 0)
        delete this;
}

bool ScriptS7ConnectionProxy::connectWithTsap(const std::string& t_ip, uint16_t t_local_tsap, uint16_t t_remote_tsap) {
    if (!conn_)
        return false;
    return shell::ok(conn_->connectWithTsap(t_ip, t_local_tsap, t_remote_tsap), "connectWithTsap");
}

void ScriptS7ConnectionProxy::useTsap(uint16_t t_local_tsap, uint16_t t_remote_tsap) {
    if (!conn_)
        return;
    conn_->setTsapMode(t_local_tsap, t_remote_tsap);
}

void ScriptS7ConnectionProxy::useRackSlot() {
    if (!conn_)
        return;
    conn_->setRackSlotMode();
}

bool ScriptS7ConnectionProxy::usesTsap() const {
    return conn_ && conn_->conn_use_tsap_;
}

uint16_t ScriptS7ConnectionProxy::localTsap() const {
    return conn_ ? conn_->conn_local_tsap_ : 0;
}

uint16_t ScriptS7ConnectionProxy::remoteTsap() const {
    return conn_ ? conn_->conn_remote_tsap_ : 0;
}

int ScriptS7ConnectionProxy::getParamInt(int t_param) const {
    if (!conn_)
        return 0;
    const auto res = conn_->client_.getParamValue<int32_t>(t_param);
    return res.hasError() ? 0 : static_cast<int>(res.value());
}

void ScriptS7ConnectionProxy::setParamInt(int t_param, int t_value) {
    if (!conn_)
        return;
    (void)shell::ok(conn_->client_.setParamValue(t_param, static_cast<int32_t>(t_value)), "setParam");
}

uint16_t ScriptS7ConnectionProxy::getParamUInt16(int t_param) const {
    if (!conn_)
        return 0;
    const auto res = conn_->client_.getParamValue<uint16_t>(t_param);
    return res.hasError() ? 0 : res.value();
}

void ScriptS7ConnectionProxy::setParamUInt16(int t_param, uint16_t t_value) {
    if (!conn_)
        return;
    (void)shell::ok(conn_->client_.setParamValue(t_param, t_value), "setParam");
}

std::string ScriptS7ConnectionProxy::paramSummary() const {
    if (!conn_)
        return "Error: no connection";
    std::string out = "Snap7 client parameters:\n";
    out += fmt::format("  RemotePort     = {}\n", getParamUInt16(p_u16_RemotePort));
    out += fmt::format("  PingTimeout    = {} ms\n", getParamInt(p_i32_PingTimeout));
    out += fmt::format("  SendTimeout    = {} ms\n", getParamInt(p_i32_SendTimeout));
    out += fmt::format("  RecvTimeout    = {} ms\n", getParamInt(p_i32_RecvTimeout));
    out += fmt::format("  WorkInterval   = {} ms\n", getParamInt(p_i32_WorkInterval));
    out += fmt::format("  SrcTSap        = 0x{:04X}\n", getParamUInt16(p_u16_SrcTSap));
    out += fmt::format("  PDURequest     = {} bytes\n", getParamInt(p_i32_PDURequest));
    out += fmt::format("  BSendTimeout   = {} ms\n", getParamInt(p_i32_BSendTimeout));
    out += fmt::format("  BRecvTimeout   = {} ms\n", getParamInt(p_i32_BRecvTimeout));
    return out;
}

} // namespace sgrn::s7shell::shell
