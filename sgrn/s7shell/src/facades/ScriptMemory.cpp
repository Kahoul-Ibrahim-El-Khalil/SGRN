#include <sgrn/s7shell/connection/S7Connection.hpp>
#include <sgrn/s7shell/facades/ScriptMemory.hpp>
#include <sgrn/s7shell/utils/json_helpers.hpp>

#include <fmt/color.h>
#include <fmt/format.h>
#include <sgrn/gateway/twin/twin.hpp>
#include <sgrn/scl/utils.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/strings.hpp>
#include <algorithm>
#include <set>

namespace sgrn::s7shell::shell
{

using namespace ::sgrn::scl;

struct AreaIoParams {
    int area{0};
    uint16_t db{0};
    int start{0};
    int amount{0};
    int word_len{S7WLByte};
};

inline AreaIoParams paramsFromPlcAddress(const PlcAddress& t_a, int t_size_override = 0) {
    AreaIoParams p;
    p.area = t_a.area;
    p.db = t_a.db_number;
    p.word_len = t_a.word_len;
    if (t_a.word_len == S7WLBit) {
        p.start = t_a.byte_offset * 8 + t_a.bit_index;
        p.amount = 1;
    } else {
        p.start = t_a.byte_offset;
        p.amount = t_size_override > 0 ? t_size_override : t_a.byte_count;
    }
    return p;
}

inline AreaIoParams paramsFromTagAddress(const TagAddress& t_a, int t_size_override = 0) {
    AreaIoParams p;
    p.area = t_a.area;
    p.db = static_cast<uint16_t>(t_a.db_number);
    p.word_len = t_a.word_len;
    if (t_a.word_len == S7WLBit) {
        p.start = t_a.start_byte * 8 + t_a.bit_offset;
        p.amount = 1;
    } else {
        p.start = t_a.start_byte;
        p.amount = t_size_override > 0 ? t_size_override : t_a.byte_count;
    }
    return p;
}

inline AreaIoParams paramsFromDbRaw(const PlcDbRawAddr& t_a, int t_size) {
    AreaIoParams p;
    p.area = S7AreaDB;
    p.db = t_a.db_number;
    p.start = t_a.offset;
    p.word_len = S7WLByte;
    p.amount = t_size > 0 ? t_size : 1;
    return p;
}

struct ResolvedSymbolicTag {
    std::string name;
    std::string source;
    std::string address_label;
    std::string type_name;
    DataType type{DataType::Bool};
    AreaIoParams io;
};

inline std::optional<ResolvedSymbolicTag> resolveSymbolicTag(ScriptS7Connection* tp_conn, const std::string& t_name) {
    if (!tp_conn)
        return std::nullopt;

    if (tp_conn->tag_table_) {
        const auto desc = tp_conn->tag_table_->describeTag(t_name);
        if (!desc.hasError()) {
            ResolvedSymbolicTag r;
            r.name = desc.value().name;
            r.source = "tag_table";
            r.type_name = desc.value().type_name;
            r.type = desc.value().type;
            const auto& t_a = desc.value().addr;
            r.address_label = fmt::format("area=0x{:02X} db={} byte={} bit={}", t_a.area, t_a.db_number, t_a.start_byte, t_a.bit_offset);
            r.io = paramsFromTagAddress(t_a);
            return r;
        }
    }

    const auto tag_res = tp_conn->schema_.getTag(t_name);
    if (!tag_res.hasError()) {
        const PlcTag* p_tag = tag_res.value();
        ResolvedSymbolicTag r;
        r.name = p_tag->name;
        r.source = "schema";
        r.type_name = p_tag->type_str;
        r.type = p_tag->type;
        r.address_label = p_tag->addr.label.empty() ? t_name : p_tag->addr.label;
        r.io = paramsFromPlcAddress(p_tag->addr);
        return r;
    }

    return std::nullopt;
}

inline std::optional<AreaIoParams> resolveAddressSpec(const std::string& t_addr, int t_size_override) {
    const std::string tok = sgrn::utils::strings::trim(t_addr);
    if (tok.empty())
        return std::nullopt;

    if (auto db_raw = parsePlcDbRawAddr(tok))
        return paramsFromDbRaw(*db_raw, t_size_override);

    if (auto plc = ::sgrn::scl::parsePlcAddress(tok))
        return paramsFromPlcAddress(*plc, t_size_override);

    return std::nullopt;
}

ScriptS7Memory::ScriptS7Memory(ScriptS7Connection* tp_conn)
    : conn_(tp_conn) {
}

void ScriptS7Memory::addRef() {
    ++ref_count_;
}

void ScriptS7Memory::release() {
    if (--ref_count_ == 0)
        delete this;
}

std::string ScriptS7Memory::readArea(int t_area, uint16_t t_db, int t_start, int t_size, int t_word_len) {
    if (!conn_ || !conn_->client_.isConnected())
        return {};
    if (t_size <= 0)
        return {};
    const auto res = conn_->client_.readArea(t_area, t_db, t_start, t_size, t_word_len);
    if (res.hasError()) {
        shell::logError(res.error(), "readArea");
        return {};
    }
    return shell::bytesToHex(res.value());
}

void ScriptS7Memory::writeArea(int t_area, uint16_t t_db, int t_start, const std::string& t_hex, int t_word_len) {
    if (!conn_ || !conn_->client_.isConnected())
        return;
    const auto bytes = ::sgrn::scl::parseHexBytes(t_hex);
    if (bytes.hasError()) {
        shell::logError(bytes.error(), "writeArea");
        return;
    }
    const int amount = (t_word_len == S7WLBit) ? 1 : static_cast<int>(bytes.value().size());
    (void)shell::ok(conn_->client_.writeArea(t_area, t_db, t_start, amount, t_word_len, bytes.value().data()), "writeArea");
}

void ScriptS7Memory::writeAreaInt(int t_area, uint16_t t_db, int t_start, int64_t t_value, int t_size_bytes, int t_word_len) {
    if (!conn_ || !conn_->client_.isConnected())
        return;
    if (t_size_bytes < 1 || t_size_bytes > 8) {
        fmt::print(stderr, fg(fmt::color::red), "writeAreaInt: size_bytes must be 1-8\n");
        return;
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(t_size_bytes));
    for (int i = t_size_bytes - 1; i >= 0; --i) {
        bytes[static_cast<size_t>(i)] = static_cast<uint8_t>(t_value & 0xFF);
        t_value >>= 8;
    }
    const int amount = (t_word_len == S7WLBit) ? 1 : t_size_bytes;
    (void)shell::ok(conn_->client_.writeArea(t_area, t_db, t_start, amount, t_word_len, bytes.data()), "writeAreaInt");
}

void ScriptS7Memory::writeAreaByte(int t_area, uint16_t t_db, int t_start, int t_value) {
    writeAreaInt(t_area, t_db, t_start, t_value, 1);
}

std::string ScriptS7Memory::readAddress(const std::string& t_addr, int t_size) {
    if (!conn_ || !conn_->client_.isConnected())
        return {};
    const auto spec = resolveAddressSpec(t_addr, t_size);
    if (!spec)
        return {};
    return readArea(spec->area, spec->db, spec->start, spec->amount, spec->word_len);
}

void ScriptS7Memory::writeAddress(const std::string& t_addr, const std::string& t_hex) {
    if (!conn_ || !conn_->client_.isConnected())
        return;
    const auto spec = resolveAddressSpec(t_addr, 0);
    if (!spec)
        return;
    writeArea(spec->area, spec->db, spec->start, t_hex, spec->word_len);
}

std::string ScriptS7Memory::readTag(const std::string& t_name) {
    if (!conn_ || !conn_->client_.isConnected())
        return {};
    const auto p_tag = resolveSymbolicTag(conn_, t_name);
    if (!p_tag)
        return {};
    return readArea(p_tag->io.area, p_tag->io.db, p_tag->io.start, p_tag->io.amount, p_tag->io.word_len);
}

void ScriptS7Memory::writeTag(const std::string& t_name, const std::string& t_hex) {
    if (!conn_ || !conn_->client_.isConnected())
        return;
    const auto p_tag = resolveSymbolicTag(conn_, t_name);
    if (!p_tag)
        return;
    writeArea(p_tag->io.area, p_tag->io.db, p_tag->io.start, t_hex, p_tag->io.word_len);
}

std::string ScriptS7Memory::tagInfo(const std::string& t_name) const {
    const auto p_tag = resolveSymbolicTag(conn_, t_name);
    if (!p_tag)
        return fmt::format("unknown tag '{}'", t_name);
    return fmt::format("name={}\nsource={}\naddress={}\ntype={}\nbytes={}\narea=0x{:02X} db={} start={} word_len={}\n", p_tag->name,
        p_tag->source, p_tag->address_label, p_tag->type_name, p_tag->io.amount, p_tag->io.area, p_tag->io.db, p_tag->io.start,
        p_tag->io.word_len);
}

std::string ScriptS7Memory::decodeTag(const std::string& t_name, const std::string& t_hex) const {
    const auto p_tag = resolveSymbolicTag(conn_, t_name);
    if (!p_tag)
        return fmt::format("unknown tag '{}'", t_name);
    const auto bytes = ::sgrn::scl::parseHexBytes(t_hex);
    if (bytes.hasError())
        return fmt::format("invalid hex: {}", bytes.error().string());
    if (static_cast<int>(bytes.value().size()) != p_tag->io.amount)
        return fmt::format("hex length {} != tag span {} bytes", bytes.value().size(), p_tag->io.amount);
    DbField fd;
    fd.type = p_tag->type;
    fd.count = 1;
    return shell::valueOr(::sgrn::gateway::twin::decodeFieldAt(fd, bytes.value().data(), bytes.value().size()), std::string{"null"});
}

std::string ScriptS7Memory::readDB(uint16_t t_db, int t_start, int t_size) {
    if (!conn_ || !conn_->client_.isConnected() || t_size <= 0)
        return {};
    const auto res = conn_->client_.readDB(t_db, t_start, t_size);
    if (res.hasError()) {
        shell::logError(res.error(), "readDB");
        return {};
    }
    return shell::bytesToHex(res.value());
}

void ScriptS7Memory::writeDB(uint16_t t_db, int t_start, const std::string& t_hex) {
    if (!conn_ || !conn_->client_.isConnected())
        return;
    const auto bytes = ::sgrn::scl::parseHexBytes(t_hex);
    if (bytes.hasError()) {
        shell::logError(bytes.error(), "writeDB");
        return;
    }
    (void)shell::ok(conn_->client_.writeDB(t_db, t_start, static_cast<int>(bytes.value().size()), bytes.value().data()), "writeDB");
}

void ScriptS7Memory::writeDBInt(uint16_t t_db, int t_start, int64_t t_value, int t_size_bytes) {
    writeAreaInt(S7AreaDB, t_db, t_start, t_value, t_size_bytes);
}

std::string ScriptS7Memory::readMB(int t_start, int t_size) {
    if (!conn_ || !conn_->client_.isConnected() || t_size <= 0)
        return {};
    const auto res = conn_->client_.readMB(t_start, t_size);
    if (res.hasError()) {
        shell::logError(res.error(), "readMB");
        return {};
    }
    return shell::bytesToHex(res.value());
}

void ScriptS7Memory::writeMB(int t_start, const std::string& t_hex) {
    if (!conn_ || !conn_->client_.isConnected())
        return;
    const auto bytes = ::sgrn::scl::parseHexBytes(t_hex);
    if (bytes.hasError()) {
        shell::logError(bytes.error(), "writeMB");
        return;
    }
    (void)shell::ok(conn_->client_.writeMB(t_start, static_cast<int>(bytes.value().size()), bytes.value().data()), "writeMB");
}

void ScriptS7Memory::writeMBInt(int t_start, int64_t t_value, int t_size_bytes) {
    writeAreaInt(S7AreaMK, 0, t_start, t_value, t_size_bytes);
}

std::string ScriptS7Memory::readEB(int t_start, int t_size) {
    if (!conn_ || !conn_->client_.isConnected() || t_size <= 0)
        return {};
    const auto res = conn_->client_.readEB(t_start, t_size);
    if (res.hasError()) {
        shell::logError(res.error(), "readEB");
        return {};
    }
    return shell::bytesToHex(res.value());
}

void ScriptS7Memory::writeEB(int t_start, const std::string& t_hex) {
    if (!conn_ || !conn_->client_.isConnected())
        return;
    const auto bytes = ::sgrn::scl::parseHexBytes(t_hex);
    if (bytes.hasError()) {
        shell::logError(bytes.error(), "writeEB");
        return;
    }
    (void)shell::ok(conn_->client_.writeEB(t_start, static_cast<int>(bytes.value().size()), bytes.value().data()), "writeEB");
}

std::string ScriptS7Memory::readAB(int t_start, int t_size) {
    if (!conn_ || !conn_->client_.isConnected() || t_size <= 0)
        return {};
    const auto res = conn_->client_.readAB(t_start, t_size);
    if (res.hasError()) {
        shell::logError(res.error(), "readAB");
        return {};
    }
    return shell::bytesToHex(res.value());
}

void ScriptS7Memory::writeAB(int t_start, const std::string& t_hex) {
    if (!conn_ || !conn_->client_.isConnected())
        return;
    const auto bytes = ::sgrn::scl::parseHexBytes(t_hex);
    if (bytes.hasError()) {
        shell::logError(bytes.error(), "writeAB");
        return;
    }
    (void)shell::ok(conn_->client_.writeAB(t_start, static_cast<int>(bytes.value().size()), bytes.value().data()), "writeAB");
}

std::string ScriptS7Memory::readTM(int t_start, int t_count) {
    if (!conn_ || !conn_->client_.isConnected() || t_count <= 0)
        return {};
    const auto res = conn_->client_.readTM(t_start, t_count);
    if (res.hasError()) {
        shell::logError(res.error(), "readTM");
        return {};
    }
    return shell::bytesToHex(res.value());
}

void ScriptS7Memory::writeTM(int t_start, int t_count, const std::string& t_hex) {
    if (!conn_ || !conn_->client_.isConnected() || t_count <= 0)
        return;
    const auto bytes = ::sgrn::scl::parseHexBytes(t_hex);
    if (bytes.hasError()) {
        shell::logError(bytes.error(), "writeTM");
        return;
    }
    const int expected = t_count * static_cast<int>(sizeof(uint16_t));
    if (static_cast<int>(bytes.value().size()) != expected) {
        fmt::print(stderr, fg(fmt::color::red), "writeTM: hex size {} != expected {} bytes for {} timers\n", bytes.value().size(), expected,
            t_count);
        return;
    }
    (void)shell::ok(conn_->client_.writeTM(t_start, t_count, bytes.value().data()), "writeTM");
}

std::string ScriptS7Memory::readCT(int t_start, int t_count) {
    if (!conn_ || !conn_->client_.isConnected() || t_count <= 0)
        return {};
    const auto res = conn_->client_.readCT(t_start, t_count);
    if (res.hasError()) {
        shell::logError(res.error(), "readCT");
        return {};
    }
    return shell::bytesToHex(res.value());
}

void ScriptS7Memory::writeCT(int t_start, int t_count, const std::string& t_hex) {
    if (!conn_ || !conn_->client_.isConnected() || t_count <= 0)
        return;
    const auto bytes = ::sgrn::scl::parseHexBytes(t_hex);
    if (bytes.hasError()) {
        shell::logError(bytes.error(), "writeCT");
        return;
    }
    const int expected = t_count * static_cast<int>(sizeof(uint16_t));
    if (static_cast<int>(bytes.value().size()) != expected) {
        fmt::print(stderr, fg(fmt::color::red), "writeCT: hex size {} != expected {} bytes for {} counters\n", bytes.value().size(),
            expected, t_count);
        return;
    }
    (void)shell::ok(conn_->client_.writeCT(t_start, t_count, bytes.value().data()), "writeCT");
}

bool ScriptS7Memory::saveHexToFile(const std::string& t_path, const std::string& t_hex) const {
    return shell::writeHexFile(t_path, t_hex);
}

std::string ScriptS7Memory::loadHexFromFile(const std::string& t_path) const {
    const auto content = shell::readHexFile(t_path);
    if (!content)
        return {};
    const auto bytes = ::sgrn::scl::parseHexBytes(*content);
    if (bytes.hasError())
        return {};
    return shell::bytesToHex(bytes.value());
}

std::string ScriptS7Memory::listTags() const {
    std::set<std::string> names;
    if (!conn_)
        return "(no connection)";
    if (conn_->tag_table_) {
        for (const auto& n : conn_->tag_table_->tagNames())
            names.insert(n);
    }
    for (const auto& [n, _] : conn_->schema_.tags())
        names.insert(n);
    if (names.empty())
        return "(no symbolic tags loaded)";
    std::string out;
    for (const auto& n : names)
        out += n + '\n';
    return out;
}

} // namespace sgrn::s7shell::shell
