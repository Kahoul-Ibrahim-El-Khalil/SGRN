#include <sgrn/s7shell/connection/S7Connection.hpp>
#include <sgrn/s7shell/facades/ScriptAsync.hpp>
#include <sgrn/s7shell/utils/json_helpers.hpp>

#include <sgrn/scl/utils.hpp>
#include <sgrn/utils/encoding.hpp>
#include <algorithm>

namespace sgrn::s7shell::shell
{

ScriptS7Async::ScriptS7Async(ScriptS7Connection* tp_conn)
    : conn_(tp_conn) {
}

void ScriptS7Async::addRef() {
    ++ref_count_;
}

void ScriptS7Async::release() {
    if (--ref_count_ == 0)
        delete this;
}

void ScriptS7Async::reset() {
    active_ = false;
    buffer_.clear();
    io_size_ = 0;
    last_op_error_ = 0;
}

bool ScriptS7Async::beginReadArea(int t_area, uint16_t t_db, int t_start, int t_size, int t_word_len) {
    reset();
    if (!conn_ || !conn_->client_.isConnected() || t_size <= 0)
        return false;
    buffer_.assign(static_cast<size_t>(t_size), 0);
    io_size_ = t_size;
    const auto res = conn_->client_.asyncReadArea(t_area, t_db, t_start, t_size, t_word_len, buffer_.data());
    if (res.hasError()) {
        shell::logError(res.error(), "asyncReadArea");
        return false;
    }
    active_ = true;
    return true;
}

bool ScriptS7Async::beginReadDB(uint16_t t_db, int t_start, int t_size) {
    reset();
    if (!conn_ || !conn_->client_.isConnected() || t_size <= 0)
        return false;
    buffer_.assign(static_cast<size_t>(t_size), 0);
    io_size_ = t_size;
    const auto res = conn_->client_.asyncReadDB(t_db, t_start, t_size, buffer_.data());
    if (res.hasError()) {
        shell::logError(res.error(), "asyncReadDB");
        return false;
    }
    active_ = true;
    return true;
}

bool ScriptS7Async::beginFullUpload(int t_block_type, uint16_t t_block_number, int t_max_size) {
    reset();
    if (!conn_ || !conn_->client_.isConnected() || t_max_size <= 0)
        return false;
    buffer_.assign(static_cast<size_t>(t_max_size), 0);
    io_size_ = t_max_size;
    const auto res = conn_->client_.asyncFullUpload(t_block_type, t_block_number, buffer_.data(), &io_size_);
    if (res.hasError()) {
        shell::logError(res.error(), "asyncFullUpload");
        return false;
    }
    active_ = true;
    return true;
}

bool ScriptS7Async::beginDownload(uint16_t t_block_number, const std::string& t_hex) {
    reset();
    if (!conn_ || !conn_->client_.isConnected())
        return false;
    const auto bytes = ::sgrn::scl::parseHexBytes(t_hex);
    if (bytes.hasError()) {
        shell::logError(bytes.error(), "asyncDownload");
        return false;
    }
    buffer_ = std::move(bytes.value());
    io_size_ = static_cast<int>(buffer_.size());
    const auto res = conn_->client_.asyncDownload(t_block_number, buffer_.data(), io_size_);
    if (res.hasError()) {
        shell::logError(res.error(), "asyncDownload");
        return false;
    }
    active_ = true;
    return true;
}

bool ScriptS7Async::isDone() {
    if (!active_ || !conn_)
        return true;
    const auto info = conn_->client_.checkAsyncCompletion();
    if (info.hasError()) {
        active_ = false;
        return true;
    }
    if (info->completed) {
        last_op_error_ = info->operation_result;
        active_ = false;
        return true;
    }
    return false;
}

bool ScriptS7Async::wait(int t_timeout_ms) {
    if (!active_ || !conn_)
        return true;
    const auto res = conn_->client_.waitAsyncCompletion(static_cast<uint32_t>(t_timeout_ms));
    if (res.hasError()) {
        shell::logError(res.error(), "waitAsyncCompletion");
        return false;
    }
    const auto info = conn_->client_.checkAsyncCompletion();
    if (info.hasError())
        return false;
    last_op_error_ = info->operation_result;
    active_ = false;
    return info->operation_result == 0;
}

int ScriptS7Async::getLastOpError() const {
    return last_op_error_;
}

std::string ScriptS7Async::resultHex() const {
    if (buffer_.empty())
        return {};
    const size_t len = io_size_ > 0 ? std::min(buffer_.size(), static_cast<size_t>(io_size_)) : buffer_.size();
    return sgrn::utils::encoding::toHex(buffer_.data(), len);
}

} // namespace sgrn::s7shell::shell
