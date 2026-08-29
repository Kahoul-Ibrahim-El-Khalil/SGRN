#pragma once

#include <sgrn/gateway/wrappers/s7/error.hpp>
#include <sgrn/scl/types.hpp>
#include <snap7.h>

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

namespace sgrn::gateway::wrappers::s7
{

using ::sgrn::gateway::wrappers::s7::S7Error;

using S7DataItem = TS7DataItem;
using S7BlockInfo = TS7BlockInfo;
using S7OrderCode = TS7OrderCode;
using S7CpuRawInfo = TS7CpuInfo;
using S7CpRawInfo = TS7CpInfo;
using S7Protection = TS7Protection;
using S7Szl = TS7SZL;
using S7SzlList = TS7SZLList;
using S7CompletionCallback = pfn_CliCompletion;

enum class PlcStatus { Unknown, Run, Stop };

struct ConnectionInfo {
    std::string ip;
    int rack{0};
    int slot{1};
    uint16_t connection_type{CONNTYPE_PG};
    uint16_t port{102};
};

struct BlockCounts {
    int ob_count{0};
    int fb_count{0};
    int fc_count{0};
    int sfb_count{0};
    int sfc_count{0};
    uint16_t db_count{0};
    int sdb_count{0};
};

struct OrderCodeInfo {
    std::string code;
    int version_major{0};
    int version_minor{0};
    int version_patch{0};
};

struct CpuInfo {
    std::string module_type_name;
    std::string serial_number;
    std::string as_name;
    std::string copyright;
    std::string module_name;
};

struct CpInfo {
    int max_pdu_length{0};
    int max_connections{0};
    int max_mpi_rate{0};
    int max_bus_rate{0};
};

struct PduLengthInfo {
    int requested{0};
    int negotiated{0};
};

struct AsyncCompletionInfo {
    bool completed{false};
    int operation_result{0};
};

struct S7DiagnosticBufferEntry {
    uint16_t event_id;
    uint8_t priority;
    uint8_t ob_number;
    uint16_t reserved;
    uint16_t info1;
    uint32_t info2;
    std::string timestamp;
};

class S7Client {
public:
    S7Client();
    virtual ~S7Client();

    S7Client(const S7Client&) = delete;
    S7Client& operator=(const S7Client&) = delete;
    S7Client(S7Client&& t_other) noexcept;
    S7Client& operator=(S7Client&& t_other) noexcept;

    // Connection and lifecycle ------------------------------------------------
    sgrn::Result<void, S7Error> connect();
    sgrn::Result<void, S7Error> connect(
        const std::string& t_ip, int t_rack, int t_slot, uint16_t t_connection_type = CONNTYPE_PG, uint16_t t_port = 102);
    sgrn::Result<void, S7Error> connect(const ConnectionInfo& t_connection_info);
    sgrn::Result<void, S7Error> setConnectionParams(const std::string& t_ip, uint16_t t_local_tsap, uint16_t t_remote_tsap);
    sgrn::Result<void, S7Error> connectWithTsap(const std::string& t_ip, uint16_t t_local_tsap, uint16_t t_remote_tsap);
    sgrn::Result<void, S7Error> setConnectionType(uint16_t t_connection_type);
    sgrn::Result<void, S7Error> disconnect();
    bool isConnected() const;

    // Client parameters -------------------------------------------------------
    sgrn::Result<void, S7Error> getParam(int t_param_number, void* tp_value);
    sgrn::Result<void, S7Error> setParam(int t_param_number, void* tp_value);

    template <typename T>
    sgrn::Result<T, S7Error> getParamValue(int t_param_number) {
        T t_value{};
        auto t_status = getParam(t_param_number, &t_value);
        if (t_status)
            return t_value;
        return t_status.error();
    }

    template <typename T>
    sgrn::Result<void, S7Error> setParamValue(int t_param_number, const T& t_value) {
        T copy = t_value;
        return setParam(t_param_number, &copy);
    }

    // Main area I/O -----------------------------------------------------------
    sgrn::Result<std::vector<uint8_t>, S7Error> readArea(
        int t_area, uint16_t t_db_number, int t_start, int t_amount, int t_word_len = S7WLByte);
    sgrn::Result<void, S7Error> readArea(int t_area, uint16_t t_db_number, int t_start, int t_amount, int t_word_len, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> writeArea(
        int t_area, uint16_t t_db_number, int t_start, int t_amount, int t_word_len, const uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> readMultiVars(S7DataItem* tp_items, int t_item_count);
    sgrn::Result<void, S7Error> readMultiVars(std::vector<S7DataItem>& t_items);
    sgrn::Result<void, S7Error> writeMultiVars(S7DataItem* tp_items, int t_item_count);
    sgrn::Result<void, S7Error> writeMultiVars(std::vector<S7DataItem>& t_items);

    // Lean area I/O -----------------------------------------------------------
    sgrn::Result<std::vector<uint8_t>, S7Error> readDB(uint16_t t_db_number, int t_start, int t_size);
    sgrn::Result<void, S7Error> readDB(uint16_t t_db_number, int t_start, int t_size, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> writeDB(uint16_t t_db_number, int t_start, int t_size, const uint8_t* tp_buffer);

    sgrn::Result<std::vector<uint8_t>, S7Error> readMB(int t_start, int t_size);
    sgrn::Result<void, S7Error> readMB(int t_start, int t_size, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> writeMB(int t_start, int t_size, const uint8_t* tp_buffer);

    sgrn::Result<std::vector<uint8_t>, S7Error> readEB(int t_start, int t_size);
    sgrn::Result<void, S7Error> readEB(int t_start, int t_size, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> writeEB(int t_start, int t_size, const uint8_t* tp_buffer);

    sgrn::Result<std::vector<uint8_t>, S7Error> readAB(int t_start, int t_size);
    sgrn::Result<void, S7Error> readAB(int t_start, int t_size, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> writeAB(int t_start, int t_size, const uint8_t* tp_buffer);

    sgrn::Result<std::vector<uint8_t>, S7Error> readTM(int t_start, int t_amount);
    sgrn::Result<void, S7Error> readTM(int t_start, int t_amount, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> writeTM(int t_start, int t_amount, const uint8_t* tp_buffer);

    sgrn::Result<std::vector<uint8_t>, S7Error> readCT(int t_start, int t_amount);
    sgrn::Result<void, S7Error> readCT(int t_start, int t_amount, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> writeCT(int t_start, int t_amount, const uint8_t* tp_buffer);

    // Directory and block metadata -------------------------------------------
    sgrn::Result<BlockCounts, S7Error> listBlocks();
    sgrn::Result<S7BlockInfo, S7Error> getAgBlockInfo(int t_block_type, uint16_t t_block_number);
    sgrn::Result<S7BlockInfo, S7Error> getPgBlockInfo(const void* tp_block_data, int t_size);
    sgrn::Result<std::vector<uint16_t>, S7Error> listBlocksOfType(int t_block_type, int t_max_items = 8192);

    // Block transfer ----------------------------------------------------------
    sgrn::Result<std::vector<uint8_t>, S7Error> upload(int t_block_type, uint16_t t_block_number, int t_buffer_size = 65536);
    sgrn::Result<std::vector<uint8_t>, S7Error> fullUpload(int t_block_type, uint16_t t_block_number, int t_buffer_size = 65536);
    sgrn::Result<void, S7Error> download(uint16_t t_block_number, const uint8_t* tp_buffer, int t_size);
    sgrn::Result<void, S7Error> deleteBlock(int t_block_type, uint16_t t_block_number);
    sgrn::Result<std::vector<uint8_t>, S7Error> dbGet(uint16_t t_db_number, int t_buffer_size = 65536);
    sgrn::Result<void, S7Error> dbFill(uint16_t t_db_number, int t_fill_char);

    // Clock and time ----------------------------------------------------------
    sgrn::Result<std::tm, S7Error> getPlcDateTime();
    sgrn::Result<void, S7Error> setPlcDateTime(const std::tm& t_date_time);
    sgrn::Result<void, S7Error> setPlcSystemDateTime();

    // System information ------------------------------------------------------
    sgrn::Result<OrderCodeInfo, S7Error> getOrderCode();
    sgrn::Result<CpuInfo, S7Error> getCpuInfo();
    sgrn::Result<CpInfo, S7Error> getCpInfo();
    sgrn::Result<std::vector<S7DiagnosticBufferEntry>, S7Error> readDiagnosticBuffer();
    sgrn::Result<S7Szl, S7Error> readSzl(int t_id, int t_index, int t_buffer_size = sizeof(S7Szl));
    sgrn::Result<S7SzlList, S7Error> readSzlList(int t_item_capacity = static_cast<int>(sizeof(S7SzlList::List) / sizeof(word)));

    // PLC control and security -----------------------------------------------
    sgrn::Result<PlcStatus, S7Error> getPlcStatus();
    sgrn::Result<void, S7Error> plcHotStart();
    sgrn::Result<void, S7Error> plcColdStart();
    sgrn::Result<void, S7Error> plcStop();
    sgrn::Result<void, S7Error> copyRamToRom(int t_timeout_ms);
    sgrn::Result<void, S7Error> compress(int t_timeout_ms);
    sgrn::Result<S7Protection, S7Error> getProtection();
    sgrn::Result<void, S7Error> setSessionPassword(const std::string& t_password);
    sgrn::Result<void, S7Error> clearSessionPassword();

    // Low-level and misc ------------------------------------------------------
    sgrn::Result<int, S7Error> getExecTime();
    sgrn::Result<int, S7Error> getLastError();
    sgrn::Result<PduLengthInfo, S7Error> getPduLength();
    sgrn::Result<int, S7Error> getPduRequested();
    sgrn::Result<int, S7Error> getNegotiatedPduLength();
    std::string getLastErrorText() const;
    static std::string errorText(int t_error_code);
    int lastErrorCode() const {
        return last_error_;
    }
    const ConnectionInfo& getConnectionInfo() const {
        return connection_info_data_;
    }

    // Async API ---------------------------------------------------------------
    sgrn::Result<void, S7Error> setAsyncCallback(S7CompletionCallback t_callback, void* tp_user_ptr);
    sgrn::Result<AsyncCompletionInfo, S7Error> checkAsyncCompletion();
    sgrn::Result<void, S7Error> waitAsyncCompletion(uint32_t t_timeout_ms);

    sgrn::Result<void, S7Error> asyncReadArea(
        int t_area, uint16_t t_db_number, int t_start, int t_amount, int t_word_len, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> asyncWriteArea(
        int t_area, uint16_t t_db_number, int t_start, int t_amount, int t_word_len, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> asyncReadDB(uint16_t t_db_number, int t_start, int t_size, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> asyncWriteDB(uint16_t t_db_number, int t_start, int t_size, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> asyncReadMB(int t_start, int t_size, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> asyncWriteMB(int t_start, int t_size, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> asyncReadEB(int t_start, int t_size, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> asyncWriteEB(int t_start, int t_size, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> asyncReadAB(int t_start, int t_size, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> asyncWriteAB(int t_start, int t_size, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> asyncReadTM(int t_start, int t_amount, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> asyncWriteTM(int t_start, int t_amount, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> asyncReadCT(int t_start, int t_amount, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> asyncWriteCT(int t_start, int t_amount, uint8_t* tp_buffer);
    sgrn::Result<void, S7Error> asyncListBlocksOfType(int t_block_type, TS7BlocksOfType* tp_blocks, int* tp_item_count);
    sgrn::Result<void, S7Error> asyncReadSzl(int t_id, int t_index, S7Szl* tp_szl, int* tp_size);
    sgrn::Result<void, S7Error> asyncReadSzlList(S7SzlList* tp_list, int* tp_item_count);
    sgrn::Result<void, S7Error> asyncUpload(int t_block_type, uint16_t t_block_number, uint8_t* tp_buffer, int* tp_size);
    sgrn::Result<void, S7Error> asyncFullUpload(int t_block_type, uint16_t t_block_number, uint8_t* tp_buffer, int* tp_size);
    sgrn::Result<void, S7Error> asyncDownload(uint16_t t_block_number, uint8_t* tp_buffer, int t_size);
    sgrn::Result<void, S7Error> asyncCopyRamToRom(int t_timeout_ms);
    sgrn::Result<void, S7Error> asyncCompress(int t_timeout_ms);
    sgrn::Result<void, S7Error> asyncDBGet(uint16_t t_db_number, uint8_t* tp_buffer, int* tp_size);
    sgrn::Result<void, S7Error> asyncDBFill(uint16_t t_db_number, int t_fill_char);

private:
    sgrn::Result<void, S7Error> makeStatus(int t_error_code) const;
    sgrn::Result<void, S7Error> requireClient() const;
    sgrn::Result<void, S7Error> validateRange(int t_start, int t_size_or_amount, const char* tp_label) const;
    sgrn::Result<void, S7Error> validateBuffer(const void* tp_buffer, int t_size_or_amount, const char* tp_label) const;

    std::unique_ptr<TS7Client> client_;
    int last_error_{0};
    ConnectionInfo connection_info_data_{};
};

} // namespace sgrn::gateway::wrappers::s7

template <>
struct fmt::formatter<sgrn::gateway::wrappers::s7::PlcStatus> : formatter<std::string_view> {
    auto format(sgrn::gateway::wrappers::s7::PlcStatus t_status, format_context& t_ctx) const {
        std::string_view text = "Unknown";
        switch (t_status) {
            case sgrn::gateway::wrappers::s7::PlcStatus::Unknown:
                text = "Unknown";
                break;
            case sgrn::gateway::wrappers::s7::PlcStatus::Run:
                text = "Run";
                break;
            case sgrn::gateway::wrappers::s7::PlcStatus::Stop:
                text = "Stop";
                break;
        }
        return formatter<std::string_view>::format(text, t_ctx);
    }
};

template <>
struct fmt::formatter<sgrn::gateway::wrappers::s7::ConnectionInfo> : formatter<std::string_view> {
    auto format(const sgrn::gateway::wrappers::s7::ConnectionInfo& t_info, format_context& t_ctx) const {
        return formatter<std::string_view>::format(fmt::format("ConnectionInfo{{ip=\"{}\", rack={}, slot={}, connection_type={}}}",
                                                       t_info.ip, t_info.rack, t_info.slot, t_info.connection_type),
            t_ctx);
    }
};

template <>
struct fmt::formatter<sgrn::gateway::wrappers::s7::BlockCounts> : formatter<std::string_view> {
    auto format(const sgrn::gateway::wrappers::s7::BlockCounts& t_counts, format_context& t_ctx) const {
        return formatter<std::string_view>::format(
            fmt::format("BlockCounts{{ob={}, fb={}, fc={}, sfb={}, sfc={}, db={}, sdb={}}}", t_counts.ob_count, t_counts.fb_count,
                t_counts.fc_count, t_counts.sfb_count, t_counts.sfc_count, t_counts.db_count, t_counts.sdb_count),
            t_ctx);
    }
};

template <>
struct fmt::formatter<sgrn::gateway::wrappers::s7::OrderCodeInfo> : formatter<std::string_view> {
    auto format(const sgrn::gateway::wrappers::s7::OrderCodeInfo& t_info, format_context& t_ctx) const {
        return formatter<std::string_view>::format(fmt::format("OrderCodeInfo{{code=\"{}\", version={}.{}.{} }}", t_info.code,
                                                       t_info.version_major, t_info.version_minor, t_info.version_patch),
            t_ctx);
    }
};

template <>
struct fmt::formatter<sgrn::gateway::wrappers::s7::CpuInfo> : formatter<std::string_view> {
    auto format(const sgrn::gateway::wrappers::s7::CpuInfo& t_info, format_context& t_ctx) const {
        return formatter<std::string_view>::format(
            fmt::format("CpuInfo{{module_type_name=\"{}\", serial_number=\"{}\", as_name=\"{}\", copyright=\"{}\", module_name=\"{}\"}}",
                t_info.module_type_name, t_info.serial_number, t_info.as_name, t_info.copyright, t_info.module_name),
            t_ctx);
    }
};

template <>
struct fmt::formatter<sgrn::gateway::wrappers::s7::CpInfo> : formatter<std::string_view> {
    auto format(const sgrn::gateway::wrappers::s7::CpInfo& t_info, format_context& t_ctx) const {
        return formatter<std::string_view>::format(
            fmt::format("CpInfo{{max_pdu_length={}, max_connections={}, max_mpi_rate={}, max_bus_rate={}}}", t_info.max_pdu_length,
                t_info.max_connections, t_info.max_mpi_rate, t_info.max_bus_rate),
            t_ctx);
    }
};

template <>
struct fmt::formatter<sgrn::gateway::wrappers::s7::PduLengthInfo> : formatter<std::string_view> {
    auto format(const sgrn::gateway::wrappers::s7::PduLengthInfo& t_info, format_context& t_ctx) const {
        return formatter<std::string_view>::format(
            fmt::format("PduLengthInfo{{requested={}, negotiated={}}}", t_info.requested, t_info.negotiated), t_ctx);
    }
};

template <>
struct fmt::formatter<sgrn::gateway::wrappers::s7::AsyncCompletionInfo> : formatter<std::string_view> {
    auto format(const sgrn::gateway::wrappers::s7::AsyncCompletionInfo& t_info, format_context& t_ctx) const {
        return formatter<std::string_view>::format(
            fmt::format("AsyncCompletionInfo{{completed={}, operation_result={}}}", t_info.completed, t_info.operation_result), t_ctx);
    }
};

template <>
struct fmt::formatter<sgrn::gateway::wrappers::s7::S7Protection> : formatter<std::string_view> {
    auto format(const sgrn::gateway::wrappers::s7::S7Protection& t_info, format_context& t_ctx) const {
        return formatter<std::string_view>::format(
            fmt::format("S7Protection{{sch_schal={}, sch_par={}, sch_rel={}, bart_sch={}, anl_sch={}}}", t_info.sch_schal, t_info.sch_par,
                t_info.sch_rel, t_info.bart_sch, t_info.anl_sch),
            t_ctx);
    }
};

template <>
struct fmt::formatter<sgrn::gateway::wrappers::s7::S7BlockInfo> : formatter<std::string_view> {
    auto format(const sgrn::gateway::wrappers::s7::S7BlockInfo& t_info, format_context& t_ctx) const {
        return formatter<std::string_view>::format(
            fmt::format("S7BlockInfo{{type={}, number={}, lang={}, flags={}, mc7_size={}, load_size={}, local_data={}, sbb_length={}, "
                        "checksum={}, version={}, code_date=\"{}\", intf_date=\"{}\", author=\"{}\", family=\"{}\", header=\"{}\"}}",
                t_info.BlkType, t_info.BlkNumber, t_info.BlkLang, t_info.BlkFlags, t_info.MC7Size, t_info.LoadSize, t_info.LocalData,
                t_info.SBBLength, t_info.CheckSum, t_info.Version, t_info.CodeDate, t_info.IntfDate, t_info.Author, t_info.Family,
                t_info.Header),
            t_ctx);
    }
};
