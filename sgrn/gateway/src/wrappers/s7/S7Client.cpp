#include <sgrn/gateway/wrappers/s7/S7Client.hpp>
#include <sgrn/gateway/wrappers/s7/error.hpp>
#include <sgrn/utils/encoding.hpp>
#include <asio.hpp>

#include <algorithm>
#include <cstring>
#include <utility>

namespace sgrn::gateway::wrappers::s7
{

namespace
{
using sgrn::gateway::wrappers::s7::fromSnap7ErrorToS7Error;
using sgrn::gateway::wrappers::s7::S7Error;
using sgrn::utils::encoding::bcdToDec;
template <typename T>
std::string trimRight(const T& t_chars) {
    std::string tp_value(t_chars);
    while (!tp_value.empty() && (tp_value.back() == '\0' || tp_value.back() == ' ')) {
        tp_value.pop_back();
    }
    return tp_value;
}

static std::string resolveHostname(const std::string& t_host) {
    try {
        asio::io_context io;
        asio::ip::tcp::resolver resolver(io);
        asio::ip::tcp::resolver::results_type results = resolver.resolve(t_host, "");
        if (!results.empty()) {
            return results.begin()->endpoint().address().to_string();
        }
    } catch (...) {
        // Fallback to the original host string if resolution fails
    }
    return t_host;
}

// Calls fn(value), propagating any error; returns value on success.
template <typename T, typename Fn>
Result<T, S7Error> invokeValue(Fn&& t_fn) {
    T tp_value{};
    const auto status = t_fn(tp_value);
    if (status.hasError()) {
        return std::move(status).error();
    }
    return tp_value;
}

template <typename Fn>
Result<std::vector<uint8_t>, S7Error> invokeSizedBuffer(int t_size, Fn&& t_fn) {
    std::vector<uint8_t> tp_buffer(static_cast<size_t>(std::max(t_size, 0)), 0);
    const auto status = t_fn(tp_buffer.data(), t_size);
    if (status.hasError()) {
        return std::move(status).error();
    }
    return tp_buffer;
}

PlcStatus plcStatusFromRaw(int t_raw_status) {
    switch (t_raw_status) {
        case S7CpuStatusRun:
            return PlcStatus::Run;
        case S7CpuStatusStop:
            return PlcStatus::Stop;
        default:
            return PlcStatus::Unknown;
    }
}

} // namespace

S7Client::S7Client()
    : client_(std::make_unique<TS7Client>()) {
}

S7Client::~S7Client() {
    disconnect();
}

S7Client::S7Client(S7Client&& t_other) noexcept
    : client_(std::move(t_other.client_))
    , last_error_(t_other.last_error_)
    , connection_info_data_(std::move(t_other.connection_info_data_)) {
    t_other.last_error_ = 0;
}

S7Client& S7Client::operator=(S7Client&& t_other) noexcept {
    if (this == &t_other) {
        return *this;
    }

    disconnect();
    client_ = std::move(t_other.client_);
    last_error_ = t_other.last_error_;
    connection_info_data_ = std::move(t_other.connection_info_data_);
    t_other.last_error_ = 0;
    return *this;
}

Result<void, S7Error> S7Client::makeStatus(int t_error_code) const {
    if (t_error_code == 0) {
        return {};
    }
    return fromSnap7ErrorToS7Error(t_error_code);
}

Result<void, S7Error> S7Client::requireClient() const {
    if (client_) {
        return {};
    }
    return S7Error::NotConnected;
}

Result<void, S7Error> S7Client::validateRange(int t_start, int t_size_or_amount, const char* tp_label) const {
    if (t_start < 0 || t_size_or_amount < 0) {
        return S7Error::InvalidParam;
    }
    return {};
}

Result<void, S7Error> S7Client::validateBuffer(const void* tp_buffer, int t_size_or_amount, const char* tp_label) const {
    if (t_size_or_amount > 0 && tp_buffer == nullptr) {
        return S7Error::InvalidParam;
    }
    return {};
}

Result<void, S7Error> S7Client::connect() {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }

    last_error_ = client_->Connect();
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::connect(const std::string& t_ip, int t_rack, int t_slot, uint16_t t_connection_type, uint16_t t_port) {
    return connect(ConnectionInfo{.ip = t_ip, .rack = t_rack, .slot = t_slot, .connection_type = t_connection_type, .port = t_port});
}

Result<void, S7Error> S7Client::connect(const ConnectionInfo& t_connection_info) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    if (t_connection_info.ip.empty()) {
        return S7Error::InvalidParam;
    }
    if (t_connection_info.rack < 0 || t_connection_info.slot < 0) {
        return S7Error::InvalidParam;
    }

    connection_info_data_ = t_connection_info;
    last_error_ = client_->SetConnectionType(t_connection_info.connection_type);
    if (last_error_ != 0) {
        return makeStatus(last_error_);
    }
    uint16_t remote_port = t_connection_info.port;
    last_error_ = client_->SetParam(p_u16_RemotePort, &remote_port);
    if (last_error_ != 0) {
        return makeStatus(last_error_);
    }
    std::string resolved_ip = resolveHostname(t_connection_info.ip);
    last_error_ = client_->ConnectTo(resolved_ip.c_str(), t_connection_info.rack, t_connection_info.slot);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::setConnectionParams(const std::string& t_ip, uint16_t t_local_tsap, uint16_t t_remote_tsap) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    if (t_ip.empty()) {
        return S7Error::InvalidParam;
    }

    connection_info_data_.ip = t_ip;
    std::string resolved_ip = resolveHostname(t_ip);
    last_error_ = client_->SetConnectionParams(resolved_ip.c_str(), t_local_tsap, t_remote_tsap);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::connectWithTsap(const std::string& t_ip, uint16_t t_local_tsap, uint16_t t_remote_tsap) {
    auto status = setConnectionParams(t_ip, t_local_tsap, t_remote_tsap);
    if (status.hasError()) {
        return status;
    }
    return connect();
}

Result<void, S7Error> S7Client::setConnectionType(uint16_t t_connection_type) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }

    connection_info_data_.connection_type = t_connection_type;
    last_error_ = client_->SetConnectionType(t_connection_type);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::disconnect() {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    if (!client_->Connected()) {
        last_error_ = 0;
        return {};
    }

    last_error_ = client_->Disconnect();
    return makeStatus(last_error_);
}

bool S7Client::isConnected() const {
    return client_ && client_->Connected();
}

Result<void, S7Error> S7Client::getParam(int t_param_number, void* tp_value) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    if (tp_value == nullptr) {
        return S7Error::InvalidParam;
    }

    last_error_ = client_->GetParam(t_param_number, tp_value);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::setParam(int t_param_number, void* tp_value) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    if (tp_value == nullptr) {
        return S7Error::InvalidParam;
    }

    last_error_ = client_->SetParam(t_param_number, tp_value);
    return makeStatus(last_error_);
}

Result<std::vector<uint8_t>, S7Error> S7Client::readArea(int t_area, uint16_t t_db_number, int t_start, int t_amount, int t_word_len) {
    return invokeSizedBuffer(
        t_amount, [&](uint8_t* tp_buffer, int t_size) { return readArea(t_area, t_db_number, t_start, t_size, t_word_len, tp_buffer); });
}

Result<void, S7Error> S7Client::readArea(int t_area, uint16_t t_db_number, int t_start, int t_amount, int t_word_len, uint8_t* tp_buffer) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    const auto range_status = validateRange(t_start, t_amount, "Offset and amount");
    if (range_status.hasError()) {
        return range_status;
    }
    const auto buffer_status = validateBuffer(tp_buffer, t_amount, "Read area");
    if (buffer_status.hasError()) {
        return buffer_status;
    }
    last_error_ = t_amount == 0 ? 0 : client_->ReadArea(t_area, t_db_number, t_start, t_amount, t_word_len, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::writeArea(
    int t_area, uint16_t t_db_number, int t_start, int t_amount, int t_word_len, const uint8_t* tp_buffer) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    const auto range_status = validateRange(t_start, t_amount, "Offset and amount");
    if (range_status.hasError()) {
        return range_status;
    }
    const auto buffer_status = validateBuffer(tp_buffer, t_amount, "Write area");
    if (buffer_status.hasError()) {
        return buffer_status;
    }

    last_error_ =
        t_amount == 0 ? 0 : client_->WriteArea(t_area, t_db_number, t_start, t_amount, t_word_len, const_cast<uint8_t*>(tp_buffer));
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::readMultiVars(S7DataItem* tp_items, int t_item_count) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    if (t_item_count < 0 || t_item_count > MaxVars) {
        return S7Error::InvalidParam;
    }
    if (t_item_count > 0 && tp_items == nullptr) {
        return S7Error::InvalidParam;
    }

    last_error_ = t_item_count == 0 ? 0 : client_->ReadMultiVars(tp_items, t_item_count);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::readMultiVars(std::vector<S7DataItem>& tp_items) {
    return readMultiVars(tp_items.data(), static_cast<int>(tp_items.size()));
}

Result<void, S7Error> S7Client::writeMultiVars(S7DataItem* tp_items, int t_item_count) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    if (t_item_count < 0 || t_item_count > MaxVars) {
        return S7Error::InvalidParam;
    }
    if (t_item_count > 0 && tp_items == nullptr) {
        return S7Error::InvalidParam;
    }

    last_error_ = t_item_count == 0 ? 0 : client_->WriteMultiVars(tp_items, t_item_count);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::writeMultiVars(std::vector<S7DataItem>& tp_items) {
    return writeMultiVars(tp_items.data(), static_cast<int>(tp_items.size()));
}

Result<std::vector<uint8_t>, S7Error> S7Client::readDB(uint16_t t_db_number, int t_start, int t_size) {
    return invokeSizedBuffer(
        t_size, [&](uint8_t* tp_buffer, int t_buffer_size) { return readDB(t_db_number, t_start, t_buffer_size, tp_buffer); });
}

Result<void, S7Error> S7Client::readDB(uint16_t t_db_number, int t_start, int t_size, uint8_t* tp_buffer) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    if (t_db_number == 0) {
        return S7Error::InvalidParam;
    }
    const auto range_status = validateRange(t_start, t_size, "DB offset and size");
    if (range_status.hasError()) {
        return range_status;
    }
    const auto buffer_status = validateBuffer(tp_buffer, t_size, "DB read");
    if (buffer_status.hasError()) {
        return buffer_status;
    }

    last_error_ = t_size == 0 ? 0 : client_->DBRead(t_db_number, t_start, t_size, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::writeDB(uint16_t t_db_number, int t_start, int t_size, const uint8_t* tp_buffer) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    if (t_db_number == 0) {
        return S7Error::InvalidParam;
    }
    const auto range_status = validateRange(t_start, t_size, "DB offset and size");
    if (range_status.hasError()) {
        return range_status;
    }
    const auto buffer_status = validateBuffer(tp_buffer, t_size, "DB write");
    if (buffer_status.hasError()) {
        return buffer_status;
    }
    last_error_ = t_size == 0 ? 0 : client_->DBWrite(t_db_number, t_start, t_size, const_cast<uint8_t*>(tp_buffer));
    return makeStatus(last_error_);
}

Result<std::vector<uint8_t>, S7Error> S7Client::readMB(int t_start, int t_size) {
    return invokeSizedBuffer(t_size, [&](uint8_t* tp_buffer, int t_buffer_size) { return readMB(t_start, t_buffer_size, tp_buffer); });
}

Result<void, S7Error> S7Client::readMB(int t_start, int t_size, uint8_t* tp_buffer) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    const auto range_status = validateRange(t_start, t_size, "MB offset and size");
    if (range_status.hasError()) {
        return range_status;
    }
    const auto buffer_status = validateBuffer(tp_buffer, t_size, "MB read");
    if (buffer_status.hasError()) {
        return buffer_status;
    }

    last_error_ = t_size == 0 ? 0 : client_->MBRead(t_start, t_size, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::writeMB(int t_start, int t_size, const uint8_t* tp_buffer) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    const auto range_status = validateRange(t_start, t_size, "MB offset and size");
    if (range_status.hasError()) {
        return range_status;
    }
    const auto buffer_status = validateBuffer(tp_buffer, t_size, "MB write");
    if (buffer_status.hasError()) {
        return buffer_status;
    }
    last_error_ = t_size == 0 ? 0 : client_->MBWrite(t_start, t_size, const_cast<uint8_t*>(tp_buffer));
    return makeStatus(last_error_);
}

Result<std::vector<uint8_t>, S7Error> S7Client::readEB(int t_start, int t_size) {
    return invokeSizedBuffer(t_size, [&](uint8_t* tp_buffer, int t_buffer_size) { return readEB(t_start, t_buffer_size, tp_buffer); });
}

Result<void, S7Error> S7Client::readEB(int t_start, int t_size, uint8_t* tp_buffer) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    const auto range_status = validateRange(t_start, t_size, "EB offset and size");
    if (range_status.hasError()) {
        return range_status;
    }
    const auto buffer_status = validateBuffer(tp_buffer, t_size, "EB read");
    if (buffer_status.hasError()) {
        return buffer_status;
    }

    last_error_ = t_size == 0 ? 0 : client_->EBRead(t_start, t_size, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::writeEB(int t_start, int t_size, const uint8_t* tp_buffer) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    const auto range_status = validateRange(t_start, t_size, "EB offset and size");
    if (range_status.hasError()) {
        return range_status;
    }
    const auto buffer_status = validateBuffer(tp_buffer, t_size, "EB write");
    if (buffer_status.hasError()) {
        return buffer_status;
    }

    last_error_ = t_size == 0 ? 0 : client_->EBWrite(t_start, t_size, const_cast<uint8_t*>(tp_buffer));
    return makeStatus(last_error_);
}

Result<std::vector<uint8_t>, S7Error> S7Client::readAB(int t_start, int t_size) {
    return invokeSizedBuffer(t_size, [&](uint8_t* tp_buffer, int t_buffer_size) { return readAB(t_start, t_buffer_size, tp_buffer); });
}

Result<void, S7Error> S7Client::readAB(int t_start, int t_size, uint8_t* tp_buffer) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    const auto range_status = validateRange(t_start, t_size, "AB offset and size");
    if (range_status.hasError()) {
        return range_status;
    }
    const auto buffer_status = validateBuffer(tp_buffer, t_size, "AB read");
    if (buffer_status.hasError()) {
        return buffer_status;
    }

    last_error_ = t_size == 0 ? 0 : client_->ABRead(t_start, t_size, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::writeAB(int t_start, int t_size, const uint8_t* tp_buffer) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    const auto range_status = validateRange(t_start, t_size, "AB offset and size");
    if (range_status.hasError()) {
        return range_status;
    }
    const auto buffer_status = validateBuffer(tp_buffer, t_size, "AB write");
    if (buffer_status.hasError()) {
        return buffer_status;
    }

    last_error_ = t_size == 0 ? 0 : client_->ABWrite(t_start, t_size, const_cast<uint8_t*>(tp_buffer));
    return makeStatus(last_error_);
}

Result<std::vector<uint8_t>, S7Error> S7Client::readTM(int t_start, int t_amount) {
    return invokeSizedBuffer(
        t_amount * static_cast<int>(sizeof(uint16_t)), [&](uint8_t* tp_buffer, int) { return readTM(t_start, t_amount, tp_buffer); });
}

Result<void, S7Error> S7Client::readTM(int t_start, int t_amount, uint8_t* tp_buffer) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    const auto range_status = validateRange(t_start, t_amount, "TM offset and amount");
    if (range_status.hasError()) {
        return range_status;
    }
    const auto buffer_status = validateBuffer(tp_buffer, t_amount, "TM read");
    if (buffer_status.hasError()) {
        return buffer_status;
    }

    last_error_ = t_amount == 0 ? 0 : client_->TMRead(t_start, t_amount, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::writeTM(int t_start, int t_amount, const uint8_t* tp_buffer) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    const auto range_status = validateRange(t_start, t_amount, "TM offset and amount");
    if (range_status.hasError()) {
        return range_status;
    }
    const auto buffer_status = validateBuffer(tp_buffer, t_amount, "TM write");
    if (buffer_status.hasError()) {
        return buffer_status;
    }

    last_error_ = t_amount == 0 ? 0 : client_->TMWrite(t_start, t_amount, const_cast<uint8_t*>(tp_buffer));
    return makeStatus(last_error_);
}

Result<std::vector<uint8_t>, S7Error> S7Client::readCT(int t_start, int t_amount) {
    return invokeSizedBuffer(
        t_amount * static_cast<int>(sizeof(uint16_t)), [&](uint8_t* tp_buffer, int) { return readCT(t_start, t_amount, tp_buffer); });
}

Result<void, S7Error> S7Client::readCT(int t_start, int t_amount, uint8_t* tp_buffer) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    const auto range_status = validateRange(t_start, t_amount, "CT offset and amount");
    if (range_status.hasError()) {
        return range_status;
    }
    const auto buffer_status = validateBuffer(tp_buffer, t_amount, "CT read");
    if (buffer_status.hasError()) {
        return buffer_status;
    }

    last_error_ = t_amount == 0 ? 0 : client_->CTRead(t_start, t_amount, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::writeCT(int t_start, int t_amount, const uint8_t* tp_buffer) {
    const auto client_status = requireClient();
    if (client_status.hasError()) {
        return client_status;
    }
    const auto range_status = validateRange(t_start, t_amount, "CT offset and amount");
    if (range_status.hasError()) {
        return range_status;
    }
    const auto buffer_status = validateBuffer(tp_buffer, t_amount, "CT write");
    if (buffer_status.hasError()) {
        return buffer_status;
    }

    last_error_ = t_amount == 0 ? 0 : client_->CTWrite(t_start, t_amount, const_cast<uint8_t*>(tp_buffer));
    return makeStatus(last_error_);
}

Result<BlockCounts, S7Error> S7Client::listBlocks() {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status.error();
    }

    TS7BlocksList raw{};
    last_error_ = client_->ListBlocks(&raw);
    if (last_error_ != 0) {
        return S7Error::InvalidParam;
    }

    return BlockCounts{
        .ob_count = raw.OBCount,
        .fb_count = raw.FBCount,
        .fc_count = raw.FCCount,
        .sfb_count = raw.SFBCount,
        .sfc_count = raw.SFCCount,
        .db_count = static_cast<uint16_t>(raw.DBCount),
        .sdb_count = raw.SDBCount,
    };
}

Result<S7BlockInfo, S7Error> S7Client::getAgBlockInfo(int t_block_type, uint16_t t_block_number) {
    return invokeValue<S7BlockInfo>([&](S7BlockInfo& t_info) {
        if (const auto client_status = requireClient(); !client_status) {
            return client_status;
        }
        last_error_ = client_->GetAgBlockInfo(t_block_type, t_block_number, &t_info);
        return makeStatus(last_error_);
    });
}

Result<S7BlockInfo, S7Error> S7Client::getPgBlockInfo(const void* tp_block_data, int t_size) {
    return invokeValue<S7BlockInfo>([&](S7BlockInfo& t_info) {
        if (const auto client_status = requireClient(); !client_status) {
            return client_status;
        }
        if (const auto buffer_status = validateBuffer(tp_block_data, t_size, "PG block"); !buffer_status) {
            return buffer_status;
        }
        last_error_ = client_->GetPgBlockInfo(const_cast<void*>(tp_block_data), &t_info, t_size);
        return makeStatus(last_error_);
    });
}

Result<std::vector<uint16_t>, S7Error> S7Client::listBlocksOfType(int t_block_type, int t_max_items) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status.error();
    }
    if (t_max_items <= 0) {
        return S7Error::InvalidParam;
    }
    TS7BlocksOfType raw{};
    int items_count = std::min(t_max_items, static_cast<int>(sizeof(raw) / sizeof(raw[0])));
    last_error_ = client_->ListBlocksOfType(t_block_type, &raw, &items_count);
    if (last_error_ != 0) {
        return S7Error::InvalidParam;
    }

    std::vector<uint16_t> values;
    values.reserve(static_cast<size_t>(std::max(items_count, 0)));
    for (int t_index = 0; t_index < items_count; ++t_index) {
        values.push_back(static_cast<uint16_t>(raw[t_index]));
    }
    return values;
}

Result<std::vector<uint8_t>, S7Error> S7Client::upload(int t_block_type, uint16_t t_block_number, int t_buffer_size) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status.error();
    }
    if (t_buffer_size <= 0) {
        return S7Error::InvalidParam;
    }

    std::vector<uint8_t> tp_buffer(static_cast<size_t>(t_buffer_size), 0);
    int t_size = t_buffer_size;
    last_error_ = client_->Upload(t_block_type, t_block_number, tp_buffer.data(), &t_size);
    if (last_error_ != 0) {
        return S7Error::InvalidParam;
    }
    tp_buffer.resize(static_cast<size_t>(std::max(t_size, 0)));
    return tp_buffer;
}

Result<std::vector<uint8_t>, S7Error> S7Client::fullUpload(int t_block_type, uint16_t t_block_number, int t_buffer_size) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status.error();
    }
    if (t_buffer_size <= 0) {
        return S7Error::InvalidParam;
    }

    std::vector<uint8_t> tp_buffer(static_cast<size_t>(t_buffer_size), 0);
    int t_size = t_buffer_size;
    last_error_ = client_->FullUpload(t_block_type, t_block_number, tp_buffer.data(), &t_size);
    if (last_error_ != 0) {
        return S7Error::InvalidParam;
    }
    tp_buffer.resize(static_cast<size_t>(std::max(t_size, 0)));
    return tp_buffer;
}

Result<void, S7Error> S7Client::download(uint16_t t_block_number, const uint8_t* tp_buffer, int t_size) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (const auto buffer_status = validateBuffer(tp_buffer, t_size, "Download"); !buffer_status) {
        return buffer_status;
    }

    last_error_ = client_->Download(t_block_number, const_cast<uint8_t*>(tp_buffer), t_size);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::deleteBlock(int t_block_type, uint16_t t_block_number) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }

    last_error_ = client_->Delete(t_block_type, t_block_number);
    return makeStatus(last_error_);
}

Result<std::vector<uint8_t>, S7Error> S7Client::dbGet(uint16_t t_db_number, int t_buffer_size) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status.error();
    }
    if (t_buffer_size <= 0) {
        return S7Error::InvalidParam;
    }

    std::vector<uint8_t> tp_buffer(static_cast<size_t>(t_buffer_size), 0);
    int t_size = t_buffer_size;
    last_error_ = client_->DBGet(t_db_number, tp_buffer.data(), &t_size);
    if (last_error_ != 0) {
        return S7Error::InvalidParam;
    }
    tp_buffer.resize(static_cast<size_t>(std::max(t_size, 0)));
    return tp_buffer;
}

Result<void, S7Error> S7Client::dbFill(uint16_t t_db_number, int t_fill_char) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (t_db_number == 0) {
        return Result<void, S7Error>::Error(S7Error::InvalidParam);
    }

    last_error_ = client_->DBFill(t_db_number, t_fill_char);
    return makeStatus(last_error_);
}

Result<std::tm, S7Error> S7Client::getPlcDateTime() {
    return invokeValue<std::tm>([&](std::tm& tp_value) {
        if (const auto client_status = requireClient(); !client_status) {
            return client_status;
        }
        last_error_ = client_->GetPlcDateTime(&tp_value);
        return makeStatus(last_error_);
    });
}

Result<void, S7Error> S7Client::setPlcDateTime(const std::tm& t_date_time) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }

    std::tm copy = t_date_time;
    last_error_ = client_->SetPlcDateTime(&copy);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::setPlcSystemDateTime() {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    last_error_ = client_->SetPlcSystemDateTime();
    return makeStatus(last_error_);
}

Result<OrderCodeInfo, S7Error> S7Client::getOrderCode() {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status.error();
    }

    TS7OrderCode raw{};
    last_error_ = client_->GetOrderCode(&raw);
    if (last_error_ != 0) {
        return S7Error::InvalidParam;
    }

    return OrderCodeInfo{
        .code = trimRight(raw.Code),
        .version_major = raw.V1,
        .version_minor = raw.V2,
        .version_patch = raw.V3,
    };
}

Result<CpuInfo, S7Error> S7Client::getCpuInfo() {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status.error();
    }

    TS7CpuInfo raw{};
    last_error_ = client_->GetCpuInfo(&raw);
    if (last_error_ != 0) {
        return S7Error::InvalidParam;
    }

    return CpuInfo{
        .module_type_name = trimRight(raw.ModuleTypeName),
        .serial_number = trimRight(raw.SerialNumber),
        .as_name = trimRight(raw.ASName),
        .copyright = trimRight(raw.Copyright),
        .module_name = trimRight(raw.ModuleName),
    };
}

Result<CpInfo, S7Error> S7Client::getCpInfo() {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status.error();
    }

    TS7CpInfo raw{};
    last_error_ = client_->GetCpInfo(&raw);
    if (last_error_ != 0) {
        return S7Error::InvalidParam;
    }

    return CpInfo{
        .max_pdu_length = raw.MaxPduLengt,
        .max_connections = raw.MaxConnections,
        .max_mpi_rate = raw.MaxMpiRate,
        .max_bus_rate = raw.MaxBusRate,
    };
}

Result<S7Szl, S7Error> S7Client::readSzl(int t_id, int t_index, int t_buffer_size) {
    return invokeValue<S7Szl>([&](S7Szl& tp_value) {
        if (const auto client_status = requireClient(); !client_status) {
            return client_status;
        }
        int t_size = std::min(t_buffer_size, static_cast<int>(sizeof(S7Szl)));
        last_error_ = client_->ReadSZL(t_id, t_index, &tp_value, &t_size);
        return makeStatus(last_error_);
    });
}

Result<S7SzlList, S7Error> S7Client::readSzlList(int t_item_capacity) {
    return invokeValue<S7SzlList>([&](S7SzlList& tp_value) {
        if (const auto client_status = requireClient(); !client_status) {
            return client_status;
        }
        int count = t_item_capacity;
        last_error_ = client_->ReadSZLList(&tp_value, &count);
        return makeStatus(last_error_);
    });
}

Result<PlcStatus, S7Error> S7Client::getPlcStatus() {
    if (const auto client_status = requireClient(); !client_status) {
        return Error(client_status.error());
    }

    const int t_raw_status = client_->PlcStatus();
    if (t_raw_status != S7CpuStatusRun && t_raw_status != S7CpuStatusStop) {
        last_error_ = t_raw_status;
        return S7Error::InvalidParam;
    }

    last_error_ = 0;
    return plcStatusFromRaw(t_raw_status);
}

Result<void, S7Error> S7Client::plcHotStart() {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    last_error_ = client_->PlcHotStart();
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::plcColdStart() {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    last_error_ = client_->PlcColdStart();
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::plcStop() {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    last_error_ = client_->PlcStop();
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::copyRamToRom(int t_timeout_ms) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    last_error_ = client_->CopyRamToRom(t_timeout_ms);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::compress(int t_timeout_ms) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    last_error_ = client_->Compress(t_timeout_ms);
    return makeStatus(last_error_);
}

Result<S7Protection, S7Error> S7Client::getProtection() {
    return invokeValue<S7Protection>([&](S7Protection& tp_value) {
        if (const auto client_status = requireClient(); !client_status) {
            return client_status;
        }
        last_error_ = client_->GetProtection(&tp_value);
        return makeStatus(last_error_);
    });
}

Result<void, S7Error> S7Client::setSessionPassword(const std::string& t_password) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }

    std::vector<char> mutable_password(t_password.begin(), t_password.end());
    mutable_password.push_back('\0');
    last_error_ = client_->SetSessionPassword(mutable_password.data());
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::clearSessionPassword() {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    last_error_ = client_->ClearSessionPassword();
    return makeStatus(last_error_);
}

Result<int, S7Error> S7Client::getExecTime() {
    return invokeValue<int>([&](int& tp_value) {
        if (const auto client_status = requireClient(); !client_status) {
            return client_status;
        }
        tp_value = client_->ExecTime();
        if (tp_value < 0) {
            last_error_ = tp_value;
            return makeStatus(last_error_);
        }
        last_error_ = 0;
        return makeStatus(0);
    });
}

Result<int, S7Error> S7Client::getLastError() {
    return invokeValue<int>([&](int& tp_value) {
        if (const auto client_status = requireClient(); !client_status) {
            return client_status;
        }
        tp_value = client_->LastError();
        if (tp_value < 0) {
            last_error_ = tp_value;
            return makeStatus(last_error_);
        }
        last_error_ = tp_value;
        return makeStatus(0);
    });
}

Result<PduLengthInfo, S7Error> S7Client::getPduLength() {
    return invokeValue<PduLengthInfo>([&](PduLengthInfo& tp_value) {
        if (const auto client_status = requireClient(); !client_status) {
            return client_status;
        }
        const int requested = client_->PDURequested();
        if (requested < 0) {
            last_error_ = requested;
            return makeStatus(last_error_);
        }
        const int negotiated = client_->PDULength();
        if (negotiated < 0) {
            last_error_ = negotiated;
            return makeStatus(last_error_);
        }
        tp_value.requested = requested;
        tp_value.negotiated = negotiated;
        last_error_ = 0;
        return makeStatus(0);
    });
}

Result<int, S7Error> S7Client::getPduRequested() {
    auto pdu = getPduLength();
    if (!pdu) {
        return Error(pdu.error());
    }
    return pdu->requested;
}

Result<int, S7Error> S7Client::getNegotiatedPduLength() {
    auto pdu = getPduLength();
    if (!pdu) {
        return Error(pdu.error());
    }
    return pdu->negotiated;
}

std::string S7Client::getLastErrorText() const {
    return errorText(last_error_);
}

std::string S7Client::errorText(int t_error_code) {
    return std::string(CliErrorText(t_error_code).c_str());
}

Result<void, S7Error> S7Client::setAsyncCallback(S7CompletionCallback t_callback, void* tp_user_ptr) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    last_error_ = client_->SetAsCallback(t_callback, tp_user_ptr);
    return makeStatus(last_error_);
}

Result<AsyncCompletionInfo, S7Error> S7Client::checkAsyncCompletion() {
    if (const auto client_status = requireClient(); !client_status) {
        return Error(client_status.error());
    }

    int operation_result = 0;
    const bool completed = client_->CheckAsCompletion(&operation_result);
    last_error_ = 0;
    return AsyncCompletionInfo{.completed = completed, .operation_result = operation_result};
}

Result<void, S7Error> S7Client::waitAsyncCompletion(uint32_t t_timeout_ms) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    last_error_ = client_->WaitAsCompletion(t_timeout_ms);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncReadArea(
    int t_area, uint16_t t_db_number, int t_start, int t_amount, int t_word_len, uint8_t* tp_buffer) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (const auto range_status = validateRange(t_start, t_amount, "Async area offset and amount"); !range_status) {
        return range_status;
    }
    if (const auto buffer_status = validateBuffer(tp_buffer, t_amount, "Async read area"); !buffer_status) {
        return buffer_status;
    }
    last_error_ = client_->AsReadArea(t_area, t_db_number, t_start, t_amount, t_word_len, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncWriteArea(
    int t_area, uint16_t t_db_number, int t_start, int t_amount, int t_word_len, uint8_t* tp_buffer) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (const auto range_status = validateRange(t_start, t_amount, "Async area offset and amount"); !range_status) {
        return range_status;
    }
    if (const auto buffer_status = validateBuffer(tp_buffer, t_amount, "Async write area"); !buffer_status) {
        return buffer_status;
    }
    last_error_ = client_->AsWriteArea(t_area, t_db_number, t_start, t_amount, t_word_len, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncReadDB(uint16_t t_db_number, int t_start, int t_size, uint8_t* tp_buffer) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (const auto buffer_status = validateBuffer(tp_buffer, t_size, "Async DB read"); !buffer_status) {
        return buffer_status;
    }
    last_error_ = client_->AsDBRead(t_db_number, t_start, t_size, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncWriteDB(uint16_t t_db_number, int t_start, int t_size, uint8_t* tp_buffer) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (const auto buffer_status = validateBuffer(tp_buffer, t_size, "Async DB write"); !buffer_status) {
        return buffer_status;
    }
    last_error_ = client_->AsDBWrite(t_db_number, t_start, t_size, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncReadMB(int t_start, int t_size, uint8_t* tp_buffer) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (const auto buffer_status = validateBuffer(tp_buffer, t_size, "Async MB read"); !buffer_status) {
        return buffer_status;
    }
    last_error_ = client_->AsMBRead(t_start, t_size, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncWriteMB(int t_start, int t_size, uint8_t* tp_buffer) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (const auto buffer_status = validateBuffer(tp_buffer, t_size, "Async MB write"); !buffer_status) {
        return buffer_status;
    }
    last_error_ = client_->AsMBWrite(t_start, t_size, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncReadEB(int t_start, int t_size, uint8_t* tp_buffer) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (const auto buffer_status = validateBuffer(tp_buffer, t_size, "Async EB read"); !buffer_status) {
        return buffer_status;
    }
    last_error_ = client_->AsEBRead(t_start, t_size, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncWriteEB(int t_start, int t_size, uint8_t* tp_buffer) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (const auto buffer_status = validateBuffer(tp_buffer, t_size, "Async EB write"); !buffer_status) {
        return buffer_status;
    }
    last_error_ = client_->AsEBWrite(t_start, t_size, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncReadAB(int t_start, int t_size, uint8_t* tp_buffer) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (const auto buffer_status = validateBuffer(tp_buffer, t_size, "Async AB read"); !buffer_status) {
        return buffer_status;
    }
    last_error_ = client_->AsABRead(t_start, t_size, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncWriteAB(int t_start, int t_size, uint8_t* tp_buffer) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (const auto buffer_status = validateBuffer(tp_buffer, t_size, "Async AB write"); !buffer_status) {
        return buffer_status;
    }
    last_error_ = client_->AsABWrite(t_start, t_size, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncReadTM(int t_start, int t_amount, uint8_t* tp_buffer) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (const auto buffer_status = validateBuffer(tp_buffer, t_amount, "Async TM read"); !buffer_status) {
        return buffer_status;
    }
    last_error_ = client_->AsTMRead(t_start, t_amount, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncWriteTM(int t_start, int t_amount, uint8_t* tp_buffer) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (const auto buffer_status = validateBuffer(tp_buffer, t_amount, "Async TM write"); !buffer_status) {
        return buffer_status;
    }
    last_error_ = client_->AsTMWrite(t_start, t_amount, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncReadCT(int t_start, int t_amount, uint8_t* tp_buffer) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (const auto buffer_status = validateBuffer(tp_buffer, t_amount, "Async CT read"); !buffer_status) {
        return buffer_status;
    }
    last_error_ = client_->AsCTRead(t_start, t_amount, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncWriteCT(int t_start, int t_amount, uint8_t* tp_buffer) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (const auto buffer_status = validateBuffer(tp_buffer, t_amount, "Async CT write"); !buffer_status) {
        return buffer_status;
    }
    last_error_ = client_->AsCTWrite(t_start, t_amount, tp_buffer);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncListBlocksOfType(int t_block_type, TS7BlocksOfType* tp_blocks, int* tp_item_count) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (tp_blocks == nullptr || tp_item_count == nullptr) {
        return S7Error::InvalidParam;
    }
    last_error_ = client_->AsListBlocksOfType(t_block_type, tp_blocks, tp_item_count);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncReadSzl(int t_id, int t_index, S7Szl* tp_szl, int* t_size) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (tp_szl == nullptr || t_size == nullptr) {
        return S7Error::InvalidParam;
    }
    last_error_ = client_->AsReadSZL(t_id, t_index, tp_szl, t_size);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncReadSzlList(S7SzlList* tp_list, int* tp_item_count) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (tp_list == nullptr || tp_item_count == nullptr) {
        return Result<void, S7Error>::Error(S7Error::InvalidParam);
    }
    last_error_ = client_->AsReadSZLList(tp_list, tp_item_count);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncUpload(int t_block_type, uint16_t t_block_number, uint8_t* tp_buffer, int* t_size) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (tp_buffer == nullptr || t_size == nullptr) {
        return Result<void, S7Error>::Error(S7Error::InvalidParam);
    }
    last_error_ = client_->AsUpload(t_block_type, t_block_number, tp_buffer, t_size);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncFullUpload(int t_block_type, uint16_t t_block_number, uint8_t* tp_buffer, int* t_size) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (tp_buffer == nullptr || t_size == nullptr) {
        return Result<void, S7Error>::Error(S7Error::InvalidParam);
    }
    last_error_ = client_->AsFullUpload(t_block_type, t_block_number, tp_buffer, t_size);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncDownload(uint16_t t_block_number, uint8_t* tp_buffer, int t_size) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (const auto buffer_status = validateBuffer(tp_buffer, t_size, "Async download"); !buffer_status) {
        return buffer_status;
    }
    last_error_ = client_->AsDownload(t_block_number, tp_buffer, t_size);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncCopyRamToRom(int t_timeout_ms) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    last_error_ = client_->AsCopyRamToRom(t_timeout_ms);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncCompress(int t_timeout_ms) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    last_error_ = client_->AsCompress(t_timeout_ms);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncDBGet(uint16_t t_db_number, uint8_t* tp_buffer, int* t_size) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    if (tp_buffer == nullptr || t_size == nullptr) {
        return Error(S7Error::InvalidParam);
    }
    last_error_ = client_->AsDBGet(t_db_number, tp_buffer, t_size);
    return makeStatus(last_error_);
}

Result<void, S7Error> S7Client::asyncDBFill(uint16_t t_db_number, int t_fill_char) {
    if (const auto client_status = requireClient(); !client_status) {
        return client_status;
    }
    last_error_ = client_->AsDBFill(t_db_number, t_fill_char);
    return makeStatus(last_error_);
}

Result<std::vector<S7DiagnosticBufferEntry>, S7Error> S7Client::readDiagnosticBuffer() {
    // SZL ID 0x00A0 = Diagnostic buffer
    auto szl_res = readSzl(0x00A0, 0x0000, 0x4000);
    if (!szl_res) {
        return Result<std::vector<S7DiagnosticBufferEntry>, S7Error>::Error(szl_res.error());
    }

    const auto& t_szl = *szl_res;
    const int entry_len = t_szl.Header.LENTHDR;
    const int entry_count = t_szl.Header.N_DR;

    if (entry_len < 20) {
        return S7Error::InvalidParam;
    }

    std::vector<S7DiagnosticBufferEntry> entries;
    entries.reserve(static_cast<size_t>(entry_count));

    for (int i = 0; i < entry_count; ++i) {
        const uint8_t* p_ptr = t_szl.Data + (i * entry_len);
        S7DiagnosticBufferEntry item;

        // Event ID (Big Endian word at 0)
        item.event_id = (static_cast<uint16_t>(p_ptr[0]) << 8) | p_ptr[1];
        item.priority = p_ptr[2];
        item.ob_number = p_ptr[3];
        item.reserved = (static_cast<uint16_t>(p_ptr[4]) << 8) | p_ptr[5];
        item.info1 = (static_cast<uint16_t>(p_ptr[6]) << 8) | p_ptr[7];
        item.info2 = (static_cast<uint32_t>(p_ptr[8]) << 24) | (static_cast<uint32_t>(p_ptr[9]) << 16) |
                     (static_cast<uint32_t>(p_ptr[10]) << 8) | p_ptr[11];

        // Timestamp (8 bytes BCD starting at offset 12)
        const uint8_t* p_ts = p_ptr + 12;
        int year = bcdToDec(p_ts[0]);
        if (year < 90)
            year += 2000;
        else
            year += 1900;
        int month = bcdToDec(p_ts[1]);
        int day = bcdToDec(p_ts[2]);
        int hour = bcdToDec(p_ts[3]);
        int min = bcdToDec(p_ts[4]);
        int sec = bcdToDec(p_ts[5]);
        int msec = (bcdToDec(p_ts[6]) * 10) + (p_ts[7] >> 4);

        item.timestamp = fmt::format("{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}", year, month, day, hour, min, sec, msec);
        entries.push_back(std::move(item));
    }
    return entries;
}

} // namespace sgrn::gateway::wrappers::s7
