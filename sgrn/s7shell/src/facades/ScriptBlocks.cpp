#include <sgrn/s7shell/connection/S7Connection.hpp>
#include <sgrn/s7shell/facades/ScriptBlocks.hpp>
#include <sgrn/s7shell/utils/json_helpers.hpp>

#include <fmt/color.h>
#include <fmt/format.h>
#include <sgrn/scl/utils.hpp>
#include <algorithm>

namespace sgrn::s7shell::shell
{

ScriptS7Blocks::ScriptS7Blocks(ScriptS7Connection* tp_conn)
    : conn_(tp_conn) {
}

void ScriptS7Blocks::addRef() {
    ++ref_count_;
}

void ScriptS7Blocks::release() {
    if (--ref_count_ == 0)
        delete this;
}

std::string ScriptS7Blocks::upload(int t_block_type, uint16_t t_block_number, int t_max_size) {
    if (!conn_ || !conn_->client_.isConnected())
        return {};
    const auto res = conn_->client_.upload(t_block_type, t_block_number, t_max_size);
    if (res.hasError()) {
        shell::logError(res.error(), "upload");
        return {};
    }
    return shell::bytesToHex(res.value());
}

std::string ScriptS7Blocks::fullUpload(int t_block_type, uint16_t t_block_number, int t_max_size) {
    if (!conn_ || !conn_->client_.isConnected())
        return {};
    const auto res = conn_->client_.fullUpload(t_block_type, t_block_number, t_max_size);
    if (res.hasError()) {
        shell::logError(res.error(), "fullUpload");
        return {};
    }
    return shell::bytesToHex(res.value());
}

void ScriptS7Blocks::download(uint16_t t_block_number, const std::string& t_hex) {
    if (!conn_ || !conn_->client_.isConnected())
        return;
    const auto bytes = ::sgrn::scl::parseHexBytes(t_hex);
    if (bytes.hasError()) {
        shell::logError(bytes.error(), "download");
        return;
    }
    (void)shell::ok(conn_->client_.download(t_block_number, bytes.value().data(), static_cast<int>(bytes.value().size())), "download");
}

void ScriptS7Blocks::deleteBlock(int t_block_type, uint16_t t_block_number) {
    if (!conn_)
        return;
    (void)shell::ok(conn_->client_.deleteBlock(t_block_type, t_block_number), "deleteBlock");
}

std::string ScriptS7Blocks::dbGet(uint16_t t_db_number, int t_max_size) {
    if (!conn_ || !conn_->client_.isConnected())
        return {};
    const auto res = conn_->client_.dbGet(t_db_number, t_max_size);
    if (res.hasError()) {
        shell::logError(res.error(), "dbGet");
        return {};
    }
    return shell::bytesToHex(res.value());
}

void ScriptS7Blocks::dbFill(uint16_t t_db_number, int t_fill_char) {
    if (!conn_)
        return;
    (void)shell::ok(conn_->client_.dbFill(t_db_number, t_fill_char), "dbFill");
}

std::string ScriptS7Blocks::pgBlockInfo(const std::string& t_hex) const {
    if (!conn_)
        return "SchemaError: no connection";
    const auto bytes = ::sgrn::scl::parseHexBytes(t_hex);
    if (bytes.hasError())
        return fmt::format("SchemaError: {}", toString(bytes.error()));
    const auto res = conn_->client_.getPgBlockInfo(bytes.value().data(), static_cast<int>(bytes.value().size()));
    if (res.hasError())
        return fmt::format("SchemaError: {}", toString(res.error()));
    return shell::formatBlockInfo(res.value());
}

bool ScriptS7Blocks::saveHex(const std::string& t_path, const std::string& t_hex) const {
    return shell::writeHexFile(t_path, t_hex);
}

std::string ScriptS7Blocks::loadHex(const std::string& t_path) const {
    const auto content = shell::readHexFile(t_path);
    if (!content)
        return {};
    const auto bytes = ::sgrn::scl::parseHexBytes(*content);
    if (bytes.hasError())
        return {};
    return shell::bytesToHex(bytes.value());
}

bool ScriptS7Blocks::uploadToFile(int t_block_type, uint16_t t_block_number, const std::string& t_path, bool t_full, int t_max_size) {
    if (!conn_ || !conn_->client_.isConnected())
        return false;
    const auto res = t_full ? conn_->client_.fullUpload(t_block_type, t_block_number, t_max_size)
                            : conn_->client_.upload(t_block_type, t_block_number, t_max_size);
    if (res.hasError()) {
        shell::logError(res.error(), t_full ? "fullUpload" : "upload");
        return false;
    }
    return shell::writeBinaryFile(t_path, res.value());
}

bool ScriptS7Blocks::downloadFromFile(uint16_t t_block_number, const std::string& t_path) {
    if (!conn_ || !conn_->client_.isConnected())
        return false;
    std::vector<uint8_t> data;
    if (!shell::readBinaryFile(t_path, data)) {
        fmt::print(stderr, fg(fmt::color::red), "downloadFromFile: cannot read {}\n", t_path);
        return false;
    }
    return shell::ok(conn_->client_.download(t_block_number, data.data(), static_cast<int>(data.size())), "download");
}

bool ScriptS7Blocks::dbGetToFile(uint16_t t_db_number, const std::string& t_path, int t_max_size) {
    if (!conn_ || !conn_->client_.isConnected())
        return false;
    const auto res = conn_->client_.dbGet(t_db_number, t_max_size);
    if (res.hasError()) {
        shell::logError(res.error(), "dbGet");
        return false;
    }
    return shell::writeBinaryFile(t_path, res.value());
}

bool ScriptS7Blocks::dbDownloadFromFile(uint16_t t_block_number, const std::string& t_path) {
    return downloadFromFile(t_block_number, t_path);
}

} // namespace sgrn::s7shell::shell
