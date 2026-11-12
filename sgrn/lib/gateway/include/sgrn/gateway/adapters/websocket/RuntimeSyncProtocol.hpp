#pragma once

// Versioned, transport-neutral state synchronization frames.
//
// This is deliberately separate from the binary WAL format and from the
// legacy raw DB subscription frame.  A RuntimeSync frame is bidirectional:
// it carries a sequence number and one or more byte ranges that can be
// applied atomically by a runtime endpoint.

#include <sgrn/Result.hpp>                   // RETURN_IF validation macro
#include <sgrn/utils/json-serialization.hpp> // shared RuntimeSync JSON DTO reflection

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace sgrn::gateway::adapters::websocket::runtime_sync
{

inline constexpr char kMagic[] = {'S', 'G', 'R', 'W'};
inline constexpr uint16_t kVersion = 1;
inline constexpr size_t kMaxFrameSize = 16 * 1024 * 1024;

enum class Kind : uint8_t {
    Delta = 1,
    Write = 2,
    Ack = 3,
};

struct Record {
    uint16_t db{0};
    uint32_t offset{0};
    std::vector<uint8_t> bytes;
};

struct Frame {
    Kind kind{Kind::Delta};
    uint64_t sequence{0};
    uint64_t timestamp_ms{0};
    std::vector<Record> records;
};

// ─── Little-endian field I/O ────────────────────────────────────────────────
//
// Byte order is expressed exactly once (here) and never re-derived per field
// width; every frame field is a width-parameterized read/write of an
// unsigned integer. `std::unsigned_integral` makes mixed-signedness or
// wrong-width calls a compile-time error instead of a silent truncation.

template <std::unsigned_integral T>
inline void putLE(std::string& p_out, T t_value) {
    for (size_t i = 0; i < sizeof(T); ++i) // compile-time bound: always fully unrolled
        p_out.push_back(static_cast<char>((t_value >> (i * 8)) & 0xff));
}

template <std::unsigned_integral T>
inline bool readLE(const std::string& p_in, size_t& p_pos, T& p_value) {
    if (p_pos + sizeof(T) > p_in.size())
        return false;
    p_value = {};
    for (size_t i = 0; i < sizeof(T); ++i)
        p_value |= static_cast<T>(static_cast<uint8_t>(p_in[p_pos + i])) << (i * 8);
    p_pos += sizeof(T);
    return true;
}

namespace detail
{

// Fixed-size LE field packs: write/read a heterogeneous list of unsigned
// fields in declaration order. Fold expressions guarantee left-to-right
// field order; reads stop at the first truncated field (all-or-nothing per
// field), leaving the remaining references untouched.

template <std::unsigned_integral... Ts>
inline void putAllLE(std::string& p_out, Ts... p_values) {
    ((putLE(p_out, p_values)), ...);
}

template <std::unsigned_integral... Ts>
inline bool readAllLE(const std::string& p_in, size_t& p_pos, Ts&... p_values) {
    return (readLE(p_in, p_pos, p_values) && ...);
}

// Total byte size of a fixed field pack.
template <std::unsigned_integral... Ts>
inline constexpr size_t sumFieldBytes() {
    return (size_t{0} + ... + sizeof(Ts));
}

} // namespace detail

// Fixed header layout after the magic: version u16, kind u8, reserved-flags
// u8, sequence u64, timestamp_ms u64, record count u32 — all little-endian.
inline constexpr size_t kFixedFieldBytes = detail::sumFieldBytes<uint16_t, uint8_t, uint8_t, uint64_t, uint64_t, uint32_t>();
inline constexpr size_t kHeaderSize = sizeof(kMagic) + kFixedFieldBytes;

// Record header: db u16, offset u32, payload length u32 — little-endian.
inline constexpr size_t kRecordHeaderSize = detail::sumFieldBytes<uint16_t, uint32_t, uint32_t>();

// JSON representation of the same RuntimeSync write/ack contract. Keeping
// these reflected types next to the binary frame definition prevents the
// gateway and s7shell client from independently declaring the same wire
// fields. Optional record members let the receiver distinguish a missing
// field from a valid zero offset / DB / area.
namespace json
{

struct DbWriteRecord {
    std::optional<uint32_t> db;
    std::optional<uint64_t> offset;
    std::optional<uint64_t> size;
    std::optional<std::string> data;
    static constexpr auto glaze =
        glz::object("db", &DbWriteRecord::db, "offset", &DbWriteRecord::offset, "size", &DbWriteRecord::size, "data", &DbWriteRecord::data);
};

struct DbWriteEnvelope {
    std::string command;
    uint64_t sequence{0};
    std::vector<DbWriteRecord> updates;
    static constexpr auto glaze =
        glz::object("command", &DbWriteEnvelope::command, "sequence", &DbWriteEnvelope::sequence, "updates", &DbWriteEnvelope::updates);
};

struct AreaWriteRecord {
    std::optional<uint32_t> area;
    std::optional<uint64_t> offset;
    std::optional<uint64_t> size;
    std::optional<std::string> data;
    static constexpr auto glaze = glz::object(
        "area", &AreaWriteRecord::area, "offset", &AreaWriteRecord::offset, "size", &AreaWriteRecord::size, "data", &AreaWriteRecord::data);
};

struct AreaWriteEnvelope {
    std::string command;
    uint64_t sequence{0};
    std::vector<AreaWriteRecord> updates;
    static constexpr auto glaze = glz::object(
        "command", &AreaWriteEnvelope::command, "sequence", &AreaWriteEnvelope::sequence, "updates", &AreaWriteEnvelope::updates);
};

struct DictionaryLeaf {
    uint32_t id{0};
    std::string path;
    static constexpr auto glaze = glz::object("id", &DictionaryLeaf::id, "path", &DictionaryLeaf::path);
};

struct DictionaryFrame {
    std::string type;
    std::vector<DictionaryLeaf> leaves;
    static constexpr auto glaze = glz::object("type", &DictionaryFrame::type, "leaves", &DictionaryFrame::leaves);
};

struct WriteAck {
    std::string type;
    std::optional<uint64_t> sequence;
    std::optional<bool> ok;
    std::optional<std::string> error;
    static constexpr auto glaze =
        glz::object("type", &WriteAck::type, "sequence", &WriteAck::sequence, "ok", &WriteAck::ok, "error", &WriteAck::error);
};

struct TypeProbe {
    std::string type;
    static constexpr auto glaze = glz::object("type", &TypeProbe::type);
};

} // namespace json

// The wire layout is pinned here and by runtime_sync_protocol_test. These
// asserts make any accidental layout change a compile-time failure; a real
// layout change must pair with a kVersion bump and consumer updates.
static_assert(kHeaderSize == 28, "runtime-sync header layout changed — bump kVersion and update consumers");
static_assert(kRecordHeaderSize == 10, "runtime-sync record header layout changed — bump kVersion and update consumers");

inline std::string encode(const Frame& frame) {
    size_t size = kHeaderSize;
    for (const auto& record : frame.records)
        size += kRecordHeaderSize + record.bytes.size();
    if (size > kMaxFrameSize || frame.records.size() > std::numeric_limits<uint32_t>::max())
        return {};

    std::string out;
    out.reserve(size);
    out.append(kMagic, sizeof(kMagic));
    detail::putAllLE(out, kVersion, static_cast<uint8_t>(frame.kind), uint8_t{0} /* flags, reserved for
                                                               compression/semantic modes */
        ,
        frame.sequence, frame.timestamp_ms, static_cast<uint32_t>(frame.records.size()));
    for (const auto& record : frame.records) {
        if (record.bytes.size() > std::numeric_limits<uint32_t>::max())
            return {};
        detail::putAllLE(out, record.db, record.offset, static_cast<uint32_t>(record.bytes.size()));
        out.append(reinterpret_cast<const char*>(record.bytes.data()), record.bytes.size());
    }
    return out;
}

inline bool decode(const std::string& input, Frame& frame, std::string* error = nullptr) {
    auto fail = [&](const char* message) {
        if (error)
            *error = message;
        return false;
    };
    if (input.size() < kHeaderSize || input.size() > kMaxFrameSize)
        return fail("invalid runtime-sync frame size");
    if (std::memcmp(input.data(), kMagic, sizeof(kMagic)) != 0)
        return fail("invalid runtime-sync magic");

    size_t pos = sizeof(kMagic);
    uint16_t version = 0;
    uint8_t kind = 0;
    uint8_t reserved_flags = 0;
    uint64_t sequence = 0;
    uint64_t timestamp_ms = 0;
    uint32_t count = 0;
    if (!detail::readAllLE(input, pos, version, kind, reserved_flags, sequence, timestamp_ms, count))
        return fail("truncated runtime-sync header");
    if (version != kVersion || kind < static_cast<uint8_t>(Kind::Delta) || kind > static_cast<uint8_t>(Kind::Ack))
        return fail("unsupported runtime-sync frame");
    frame.kind = static_cast<Kind>(kind);
    frame.sequence = sequence;
    frame.timestamp_ms = timestamp_ms;
    frame.records.clear();

    RETURN_IF(count > (input.size() - pos) / kRecordHeaderSize, fail("runtime-sync record count exceeds frame"));

    frame.records.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Record record;
        uint32_t length = 0;

        RETURN_IF(!detail::readAllLE(input, pos, record.db, record.offset, length), fail("truncated runtime-sync record"));

        RETURN_IF(length > input.size() - pos, fail("runtime-sync record exceeds frame"));
        record.bytes.assign(
            reinterpret_cast<const uint8_t*>(input.data() + pos), reinterpret_cast<const uint8_t*>(input.data() + pos + length));
        pos += length;

        frame.records.push_back(std::move(record));
    }
    RETURN_IF(pos != input.size(), fail("trailing runtime-sync bytes"));
    return true;
}

inline bool isFrame(const std::string& input) {
    return input.size() >= sizeof(kMagic) && std::memcmp(input.data(), kMagic, sizeof(kMagic)) == 0;
}

} // namespace sgrn::gateway::adapters::websocket::runtime_sync
