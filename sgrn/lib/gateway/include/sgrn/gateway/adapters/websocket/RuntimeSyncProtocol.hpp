#pragma once

// Versioned, transport-neutral state synchronization frames.
//
// This is deliberately separate from the binary WAL format and from the
// legacy raw DB subscription frame.  A RuntimeSync frame is bidirectional:
// it carries a sequence number and one or more byte ranges that can be
// applied atomically by a runtime endpoint.

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace sgrn::gateway::adapters::websocket::runtime_sync
{

inline constexpr char kMagic[] = {'S', 'G', 'R', 'W'};
inline constexpr uint16_t kVersion = 1;
inline constexpr size_t kHeaderSize = 28;
inline constexpr size_t kRecordHeaderSize = 10;
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

inline void put16(std::string& out, uint16_t value) {
    out.push_back(static_cast<char>(value & 0xff));
    out.push_back(static_cast<char>((value >> 8) & 0xff));
}

inline void put32(std::string& out, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
}

inline void put64(std::string& out, uint64_t value) {
    for (unsigned i = 0; i < 8; ++i)
        out.push_back(static_cast<char>((value >> (i * 8)) & 0xff));
}

inline bool read16(const std::string& in, size_t& pos, uint16_t& value) {
    if (pos + 2 > in.size())
        return false;
    value = static_cast<uint16_t>(static_cast<uint8_t>(in[pos])) | static_cast<uint16_t>(static_cast<uint8_t>(in[pos + 1])) << 8;
    pos += 2;
    return true;
}

inline bool read32(const std::string& in, size_t& pos, uint32_t& value) {
    if (pos + 4 > in.size())
        return false;
    value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<uint32_t>(static_cast<uint8_t>(in[pos + i])) << (i * 8);
    pos += 4;
    return true;
}

inline bool read64(const std::string& in, size_t& pos, uint64_t& value) {
    if (pos + 8 > in.size())
        return false;
    value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(static_cast<uint8_t>(in[pos + i])) << (i * 8);
    pos += 8;
    return true;
}

inline std::string encode(const Frame& frame) {
    size_t size = kHeaderSize;
    for (const auto& record : frame.records)
        size += kRecordHeaderSize + record.bytes.size();
    if (size > kMaxFrameSize || frame.records.size() > std::numeric_limits<uint32_t>::max())
        return {};

    std::string out;
    out.reserve(size);
    out.append(kMagic, sizeof(kMagic));
    put16(out, kVersion);
    out.push_back(static_cast<char>(frame.kind));
    out.push_back(0); // flags, reserved for compression/semantic modes
    put64(out, frame.sequence);
    put64(out, frame.timestamp_ms);
    put32(out, static_cast<uint32_t>(frame.records.size()));
    for (const auto& record : frame.records) {
        if (record.bytes.size() > std::numeric_limits<uint32_t>::max())
            return {};
        put16(out, record.db);
        put32(out, record.offset);
        put32(out, static_cast<uint32_t>(record.bytes.size()));
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
    uint32_t count = 0;
    if (!read16(input, pos, version) || pos + 2 > input.size())
        return fail("truncated runtime-sync header");
    kind = static_cast<uint8_t>(input[pos++]);
    ++pos; // flags, reserved
    if (!read64(input, pos, frame.sequence) || !read64(input, pos, frame.timestamp_ms) || !read32(input, pos, count))
        return fail("truncated runtime-sync header");
    if (version != kVersion || kind < static_cast<uint8_t>(Kind::Delta) || kind > static_cast<uint8_t>(Kind::Ack))
        return fail("unsupported runtime-sync frame");
    if (count > (input.size() - pos) / kRecordHeaderSize)
        return fail("runtime-sync record count exceeds frame");
    frame.kind = static_cast<Kind>(kind);
    frame.records.clear();
    frame.records.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t db = 0;
        uint32_t offset = 0;
        uint32_t length = 0;
        if (!read16(input, pos, db) || !read32(input, pos, offset) || !read32(input, pos, length))
            return fail("truncated runtime-sync record");
        if (length > input.size() - pos)
            return fail("runtime-sync record exceeds frame");
        Record record;
        record.db = db;
        record.offset = offset;
        record.bytes.assign(
            reinterpret_cast<const uint8_t*>(input.data() + pos), reinterpret_cast<const uint8_t*>(input.data() + pos + length));
        pos += length;
        frame.records.push_back(std::move(record));
    }
    if (pos != input.size())
        return fail("trailing runtime-sync bytes");
    return true;
}

inline bool isFrame(const std::string& input) {
    return input.size() >= sizeof(kMagic) && std::memcmp(input.data(), kMagic, sizeof(kMagic)) == 0;
}

} // namespace sgrn::gateway::adapters::websocket::runtime_sync
