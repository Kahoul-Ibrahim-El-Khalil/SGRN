#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace sgrn::common::wal
{

/**
 * @brief Sentinel DB number marking a control/metadata frame (version 2+).
 */
inline constexpr uint16_t kControlFrameDbNum = 0xFFFF;

/**
 * @brief Sentinel DB number marking a delta frame (version 2+).
 */
inline constexpr uint16_t kDeltaFrameDbNum = 0xFFFE;

/**
 * @brief Sentinel DB number marking a binary-WAL anchor frame (version 3+).
 *
 * An anchor payload is `db_num:u16` (REAL target) + `crc32:u32` + the full
 * DB image. The CRC covers the image bytes and lets readers verify a
 * decode; anchors are also resync points.
 */
inline constexpr uint16_t kAnchorFrameDbNum = 0xFFFD;

/**
 * @brief CRC32 (IEEE, zlib-compatible) over raw bytes.
 */
inline uint32_t binaryWalCrc32(const uint8_t* t_data, size_t t_size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < t_size; ++i) {
        crc ^= static_cast<uint32_t>(t_data[i]);
        for (int k = 0; k < 8; ++k)
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : (crc >> 1);
    }
    return ~crc;
}

/**
 * @brief Current binary WAL version stamped in the archive header.
 *
 * v1 = full-image data frames only. v2 = v1 + delta frames. v3 = v2 +
 * anchor frames (verifiable full images). v4 = v3 + binary schema payload
 * in the header.
 */
inline constexpr uint16_t kBinaryWalVersion = 4;

/// Validated archive header.
struct BinaryWalHeader {
    uint16_t version = 0;
    uint32_t schema_len = 0;
    size_t frames_start = 10; ///< first byte after header + schema payload
};

/// Header validation outcome.
enum class BinaryHeaderStatus {
    kOk,
    kTooShort,   ///< fewer than 10 bytes or schema overruns the buffer
    kBadMagic,   ///< missing SGRN magic
    kBadVersion, ///< ver < 1 (v0 never shipped) — readers handle 1..kBinaryWalVersion
};

/// Validates magic + version + header bounds.
inline BinaryHeaderStatus checkBinaryWalHeader(const std::string& t_data, BinaryWalHeader& t_header) {
    if (t_data.size() < 10)
        return BinaryHeaderStatus::kTooShort;
    if (t_data[0] != 'S' || t_data[1] != 'G' || t_data[2] != 'R' || t_data[3] != 'N')
        return BinaryHeaderStatus::kBadMagic;
    uint16_t ver = 0;
    uint32_t schema_len = 0;
    std::memcpy(&ver, t_data.data() + 4, sizeof(ver));
    std::memcpy(&schema_len, t_data.data() + 6, sizeof(schema_len));
    if (10 + schema_len > t_data.size())
        return BinaryHeaderStatus::kTooShort;
    if (ver < 1 || ver > kBinaryWalVersion)
        return BinaryHeaderStatus::kBadVersion;
    t_header.version = ver;
    t_header.schema_len = schema_len;
    t_header.frames_start = 10 + schema_len;
    return BinaryHeaderStatus::kOk;
}

/// One decoded frame header. `payload` points into the source buffer.
struct BinaryFrame {
    int64_t ts = 0;
    uint16_t db = 0;
    uint32_t payload_len = 0;
    size_t header_start = 0; ///< offset of the 14-byte header (resync base)
    const uint8_t* payload = nullptr;
};

/// Frame decode outcome.
enum class BinaryFrameStatus {
    kOk,       ///< t_pos advanced past the payload; t_frame filled
    kEnd,      ///< fewer than 14 bytes remain (clean EOF); t_pos untouched
    kTruncated ///< header parsed but payload overruns (t_pos left at header)
};

/// Decodes the frame at t_pos. Never reads out of bounds.
inline BinaryFrameStatus decodeBinaryFrame(const std::string& t_data, size_t& t_pos, BinaryFrame& t_frame) {
    if (t_pos + 14 > t_data.size())
        return BinaryFrameStatus::kEnd;
    int64_t ts = 0;
    uint16_t db = 0;
    uint32_t len = 0;
    std::memcpy(&ts, t_data.data() + t_pos, sizeof(ts));
    std::memcpy(&db, t_data.data() + t_pos + 8, sizeof(db));
    std::memcpy(&len, t_data.data() + t_pos + 10, sizeof(len));
    if (t_pos + 14 + len > t_data.size())
        return BinaryFrameStatus::kTruncated;
    t_frame.ts = ts;
    t_frame.db = db;
    t_frame.payload_len = len;
    t_frame.header_start = t_pos;
    t_frame.payload = reinterpret_cast<const uint8_t*>(t_data.data()) + t_pos + 14;
    t_pos += 14 + len;
    return BinaryFrameStatus::kOk;
}

/// A validated delta run: [offset, offset+len) within the image.
struct BinaryDeltaRun {
    uint32_t offset = 0;
    uint32_t len = 0;
    uint32_t data_pos = 0;
};

/// Validates a delta payload (`db:u16` + runs) against an image size limit.
inline bool parseDeltaRuns(const uint8_t* t_payload, uint32_t t_len, size_t t_limit, uint16_t& t_db, std::vector<BinaryDeltaRun>& t_runs) {
    t_runs.clear();
    if (t_len < 2)
        return false;
    std::memcpy(&t_db, t_payload, sizeof(t_db));
    size_t run_pos = 2;
    while (run_pos + 8 <= t_len) {
        uint32_t run_off = 0;
        uint32_t run_len = 0;
        std::memcpy(&run_off, t_payload + run_pos, sizeof(run_off));
        std::memcpy(&run_len, t_payload + run_pos + sizeof(run_off), sizeof(run_len));
        run_pos += 8;
        if (run_pos + run_len > t_len || static_cast<size_t>(run_off) + run_len > t_limit)
            return false;
        t_runs.push_back(BinaryDeltaRun{run_off, run_len, static_cast<uint32_t>(run_pos)});
        run_pos += run_len;
    }
    return run_pos == t_len && !t_runs.empty();
}

/// Verifies an anchor payload (`db:u16` + `crc32:u32` + image).
inline bool verifyAnchorFrame(const uint8_t* t_payload, uint32_t t_len, uint16_t& t_db, const uint8_t*& t_image, size_t& t_image_len) {
    if (t_len < 6)
        return false;
    uint32_t crc = 0;
    std::memcpy(&t_db, t_payload, sizeof(t_db));
    std::memcpy(&crc, t_payload + sizeof(t_db), sizeof(crc));
    t_image = t_payload + 6;
    t_image_len = t_len - 6;
    return binaryWalCrc32(t_image, t_image_len) == crc;
}

/// Scans forward for the next verifiable anchor frame.
inline std::optional<size_t> findNextAnchorFrame(const std::string& t_data, size_t t_from) {
    const size_t n = t_data.size();
    for (size_t p = t_from; p + 14 <= n; ++p) {
        uint16_t db = 0;
        std::memcpy(&db, t_data.data() + p + 8, sizeof(db));
        if (db != kAnchorFrameDbNum)
            continue;
        uint32_t len = 0;
        std::memcpy(&len, t_data.data() + p + 10, sizeof(len));
        if (p + 14 + len > n)
            continue;
        if (len < 6)
            continue;
        uint32_t crc = 0;
        std::memcpy(&crc, t_data.data() + p + 14 + 2, sizeof(crc));
        const auto* img = reinterpret_cast<const uint8_t*>(t_data.data()) + p + 14 + 6;
        const size_t img_len = len - 6;
        if (binaryWalCrc32(img, img_len) == crc)
            return p;
    }
    return std::nullopt;
}

} // namespace sgrn::common::wal
