#include <sgrn/s7shell/connection/S7Connection.hpp>
#include <sgrn/s7shell/facades/ScriptDiagnostics.hpp>
#include <sgrn/s7shell/utils/json_helpers.hpp>

#include <fmt/format.h>
#include <algorithm>

namespace sgrn::s7shell::shell
{

ScriptS7Diagnostics::ScriptS7Diagnostics(ScriptS7Connection* tp_conn)
    : conn_(tp_conn) {
}

void ScriptS7Diagnostics::addRef() {
    ++ref_count_;
}

void ScriptS7Diagnostics::release() {
    if (--ref_count_ == 0)
        delete this;
}

std::string ScriptS7Diagnostics::connectionInfo() const {
    if (!conn_)
        return "Error: no connection";
    const auto& ci = conn_->client_.getConnectionInfo();
    if (conn_->conn_use_tsap_) {
        return fmt::format("ip={} port={} type={}(0x{:04X}) mode=TSAP local_tsap=0x{:04X} remote_tsap=0x{:04X} connected={}",
            conn_->conn_ip_, conn_->conn_port_, shell::connectionTypeName(conn_->conn_type_), ci.connection_type, conn_->conn_local_tsap_,
            conn_->conn_remote_tsap_, conn_->client_.isConnected() ? "true" : "false");
    }
    return fmt::format("ip={} rack={} slot={} port={} type={}(0x{:04X}) mode=RackSlot connected={}", conn_->conn_ip_, conn_->conn_rack_,
        conn_->conn_slot_, conn_->conn_port_, shell::connectionTypeName(conn_->conn_type_), ci.connection_type,
        conn_->client_.isConnected() ? "true" : "false");
}

int ScriptS7Diagnostics::lastError() const {
    return conn_ ? conn_->client_.lastErrorCode() : 0;
}

std::string ScriptS7Diagnostics::lastErrorText() const {
    return conn_ ? conn_->client_.getLastErrorText() : std::string{};
}

std::string ScriptS7Diagnostics::pduInfo() const {
    if (!conn_ || !conn_->client_.isConnected())
        return "Error: not connected";
    auto r = conn_->client_.getPduLength();
    if (r.hasError())
        return fmt::format("Error: {}", toString(r.error()));
    return fmt::format("requested={} negotiated={}", r->requested, r->negotiated);
}

std::string ScriptS7Diagnostics::status() const {
    if (!conn_ || !conn_->client_.isConnected())
        return "Unknown";
    auto r = conn_->client_.getPlcStatus();
    if (r.hasError())
        return "Unknown";
    return shell::plcStatusText(r.value());
}

bool ScriptS7Diagnostics::isRunning() const {
    return status() == "Run";
}

std::string ScriptS7Diagnostics::cpuInfo() const {
    if (!conn_ || !conn_->client_.isConnected())
        return "Error: not connected";
    auto r = conn_->client_.getCpuInfo();
    if (r.hasError())
        return fmt::format("Error: {}", toString(r.error()));
    return fmt::format("Module Type : {}\nSerial No   : {}\nAS Name     : {}\nCopyright   : {}\nModule Name : {}\n", r->module_type_name,
        r->serial_number, r->as_name, r->copyright, r->module_name);
}

std::string ScriptS7Diagnostics::orderCode() const {
    if (!conn_ || !conn_->client_.isConnected())
        return "Error: not connected";
    auto r = conn_->client_.getOrderCode();
    if (r.hasError())
        return fmt::format("Error: {}", toString(r.error()));
    return fmt::format("Order Code  : {}\nFirmware    : {}.{}.{}\n", r->code, r->version_major, r->version_minor, r->version_patch);
}

std::string ScriptS7Diagnostics::cpInfo() const {
    if (!conn_ || !conn_->client_.isConnected())
        return "Error: not connected";
    auto r = conn_->client_.getCpInfo();
    if (r.hasError())
        return fmt::format("Error: {}", toString(r.error()));
    return fmt::format("Max PDU     : {}\nMax Conn    : {}\nMax MPI     : {}\nMax Bus     : {}\n", r->max_pdu_length, r->max_connections,
        r->max_mpi_rate, r->max_bus_rate);
}

std::string ScriptS7Diagnostics::info() const {
    return cpuInfo() + orderCode() + cpInfo();
}

std::string ScriptS7Diagnostics::diagnosticBuffer(int t_count) const {
    if (!conn_ || !conn_->client_.isConnected())
        return "Error: not connected";
    auto r = conn_->client_.readDiagnosticBuffer();
    if (r.hasError())
        return fmt::format("Error: {}", toString(r.error()));
    const int n = std::max(0, std::min(t_count, static_cast<int>(r->size())));
    std::string out = fmt::format("Last {} diagnostic entries:\n", n);
    const int start = std::max(0, static_cast<int>(r->size()) - n);
    for (size_t i = static_cast<size_t>(start); i < r->size(); ++i) {
        const auto& e = (*r)[i];
        out += fmt::format("  [{}] ID=0x{:04X} OB={:03d} P={} I1=0x{:04X} I2=0x{:08X}\n", e.timestamp, e.event_id, e.ob_number, e.priority,
            e.info1, e.info2);
    }
    return out;
}

std::string ScriptS7Diagnostics::szl(int t_id, int t_index) const {
    if (!conn_ || !conn_->client_.isConnected())
        return "Error: not connected";
    auto r = conn_->client_.readSzl(t_id, t_index);
    if (r.hasError())
        return fmt::format("Error: {}", toString(r.error()));
    const auto& szl = r.value();
    const int entry_len = szl.Header.LENTHDR;
    const int entry_count = szl.Header.N_DR;
    std::string out = fmt::format("SZL ID 0x{:04X} Index 0x{:04X} ({} entries x {} bytes):\n", t_id, t_index, entry_count, entry_len);
    for (int i = 0; i < entry_count; ++i) {
        const uint8_t* p_data = szl.Data + (i * entry_len);
        out += fmt::format("  [{:03d}] ", i);
        for (int j = 0; j < std::min(entry_len, 16); ++j)
            out += fmt::format("{:02X} ", p_data[j]);
        if (entry_len > 16)
            out += "...";
        out += "\n";
    }
    return out;
}

std::string ScriptS7Diagnostics::listBlocks() const {
    if (!conn_ || !conn_->client_.isConnected())
        return "Error: not connected";
    auto r = conn_->client_.listBlocks();
    if (r.hasError())
        return fmt::format("Error: {}", toString(r.error()));
    return fmt::format("OB={} FB={} FC={} SFB={} SFC={} DB={} SDB={}\n", r->ob_count, r->fb_count, r->fc_count, r->sfb_count, r->sfc_count,
        r->db_count, r->sdb_count);
}

std::string ScriptS7Diagnostics::listBlocksOfType(int t_block_type) const {
    if (!conn_ || !conn_->client_.isConnected())
        return "Error: not connected";
    auto r = conn_->client_.listBlocksOfType(t_block_type);
    if (r.hasError())
        return fmt::format("Error: {}", toString(r.error()));
    std::string out;
    for (uint16_t num : r.value())
        out += fmt::format("{}\n", num);
    return out.empty() ? "(none)\n" : out;
}

std::string ScriptS7Diagnostics::blockInfo(int t_block_type, uint16_t t_block_number) const {
    if (!conn_ || !conn_->client_.isConnected())
        return "Error: not connected";
    auto r = conn_->client_.getAgBlockInfo(t_block_type, t_block_number);
    if (r.hasError())
        return fmt::format("Error: {}", toString(r.error()));
    return shell::formatBlockInfo(r.value());
}

std::string ScriptS7Diagnostics::protection() const {
    if (!conn_ || !conn_->client_.isConnected())
        return "Error: not connected";
    auto r = conn_->client_.getProtection();
    if (r.hasError())
        return fmt::format("Error: {}", toString(r.error()));
    return fmt::format(
        "sch_schal={} sch_par={} sch_rel={} bart_sch={} anl_sch={}\n", r->sch_schal, r->sch_par, r->sch_rel, r->bart_sch, r->anl_sch);
}

} // namespace sgrn::s7shell::shell
