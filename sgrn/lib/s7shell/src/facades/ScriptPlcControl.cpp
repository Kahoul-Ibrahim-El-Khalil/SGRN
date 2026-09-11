#include <sgrn/s7shell/connection/S7Connection.hpp>
#include <sgrn/s7shell/facades/ScriptPlcControl.hpp>
#include <sgrn/s7shell/utils/json_helpers.hpp>

#include <fmt/format.h>
#include <sgrn/utils/time.hpp>

namespace sgrn::s7shell::shell
{

ScriptS7PlcControl::ScriptS7PlcControl(ScriptS7Connection* tp_conn)
    : conn_(tp_conn) {
}

void ScriptS7PlcControl::addRef() {
    ++ref_count_;
}

void ScriptS7PlcControl::release() {
    if (--ref_count_ == 0)
        delete this;
}

void ScriptS7PlcControl::hotStart() {
    if (!conn_)
        return;
    (void)shell::ok(conn_->client_.plcHotStart(), "plcHotStart");
}

void ScriptS7PlcControl::coldStart() {
    if (!conn_)
        return;
    (void)shell::ok(conn_->client_.plcColdStart(), "plcColdStart");
}

void ScriptS7PlcControl::stop() {
    if (!conn_)
        return;
    (void)shell::ok(conn_->client_.plcStop(), "plcStop");
}

std::string ScriptS7PlcControl::clock() const {
    if (!conn_ || !conn_->client_.isConnected())
        return "SchemaError: not connected";
    auto r = conn_->client_.getPlcDateTime();
    if (r.hasError())
        return fmt::format("SchemaError: {}", toString(r.error()));
    return sgrn::utils::time::formatDateTime(r.value());
}

void ScriptS7PlcControl::setClock(int t_year, int t_month, int t_day, int t_hour, int t_minute, int t_second) {
    if (!conn_)
        return;
    std::tm ti{};
    ti.tm_year = t_year - 1900;
    ti.tm_mon = t_month - 1;
    ti.tm_mday = t_day;
    ti.tm_hour = t_hour;
    ti.tm_min = t_minute;
    ti.tm_sec = t_second;
    ti.tm_isdst = -1;
    (void)shell::ok(conn_->client_.setPlcDateTime(ti), "setPlcDateTime");
}

void ScriptS7PlcControl::syncClockToSystem() {
    if (!conn_)
        return;
    (void)shell::ok(conn_->client_.setPlcSystemDateTime(), "setPlcSystemDateTime");
}

void ScriptS7PlcControl::setPassword(const std::string& t_password) {
    if (!conn_)
        return;
    (void)shell::ok(conn_->client_.setSessionPassword(t_password), "setSessionPassword");
}

void ScriptS7PlcControl::clearPassword() {
    if (!conn_)
        return;
    (void)shell::ok(conn_->client_.clearSessionPassword(), "clearSessionPassword");
}

void ScriptS7PlcControl::copyRamToRom(int t_timeout_ms) {
    if (!conn_)
        return;
    (void)shell::ok(conn_->client_.copyRamToRom(t_timeout_ms), "copyRamToRom");
}

void ScriptS7PlcControl::compress(int t_timeout_ms) {
    if (!conn_)
        return;
    (void)shell::ok(conn_->client_.compress(t_timeout_ms), "compress");
}

} // namespace sgrn::s7shell::shell
