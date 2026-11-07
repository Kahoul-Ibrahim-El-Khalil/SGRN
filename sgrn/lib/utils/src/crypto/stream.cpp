#include "sgrn/utils/crypto/stream.hpp"

#include <fmt/format.h>
#include <cstring>
#include <iostream>

namespace sgrn::utils::crypto
{

namespace
{
constexpr std::size_t kChunkHeaderSize = 4 + 4 + 1 + kTagSize; // 25 bytes

struct ChunkFrameHeader {
    uint32_t chunk_idx{0};
    uint32_t payload_len{0};
    uint8_t flags{0};
    std::array<uint8_t, kTagSize> tag{};

    [[nodiscard]] bool isFinal() const noexcept {
        return (flags & kChunkFlagFinal) != 0;
    }

    void writeTo(std::vector<uint8_t>& t_out) const {
        t_out.push_back(static_cast<uint8_t>((chunk_idx >> 24) & 0xFF));
        t_out.push_back(static_cast<uint8_t>((chunk_idx >> 16) & 0xFF));
        t_out.push_back(static_cast<uint8_t>((chunk_idx >> 8) & 0xFF));
        t_out.push_back(static_cast<uint8_t>(chunk_idx & 0xFF));

        t_out.push_back(static_cast<uint8_t>((payload_len >> 24) & 0xFF));
        t_out.push_back(static_cast<uint8_t>((payload_len >> 16) & 0xFF));
        t_out.push_back(static_cast<uint8_t>((payload_len >> 8) & 0xFF));
        t_out.push_back(static_cast<uint8_t>(payload_len & 0xFF));

        t_out.push_back(flags);
        t_out.insert(t_out.end(), tag.begin(), tag.end());
    }

    void writeTo(uint8_t t_out[kChunkHeaderSize]) const {
        t_out[0] = static_cast<uint8_t>((chunk_idx >> 24) & 0xFF);
        t_out[1] = static_cast<uint8_t>((chunk_idx >> 16) & 0xFF);
        t_out[2] = static_cast<uint8_t>((chunk_idx >> 8) & 0xFF);
        t_out[3] = static_cast<uint8_t>(chunk_idx & 0xFF);

        t_out[4] = static_cast<uint8_t>((payload_len >> 24) & 0xFF);
        t_out[5] = static_cast<uint8_t>((payload_len >> 16) & 0xFF);
        t_out[6] = static_cast<uint8_t>((payload_len >> 8) & 0xFF);
        t_out[7] = static_cast<uint8_t>(payload_len & 0xFF);

        t_out[8] = flags;
        std::memcpy(t_out + 9, tag.data(), kTagSize);
    }

    static Result<ChunkFrameHeader> deserialize(std::span<const uint8_t, kChunkHeaderSize> t_bytes) {
        ChunkFrameHeader frame;
        frame.chunk_idx = (static_cast<uint32_t>(t_bytes[0]) << 24) | (static_cast<uint32_t>(t_bytes[1]) << 16) |
                          (static_cast<uint32_t>(t_bytes[2]) << 8) | static_cast<uint32_t>(t_bytes[3]);
        frame.payload_len = (static_cast<uint32_t>(t_bytes[4]) << 24) | (static_cast<uint32_t>(t_bytes[5]) << 16) |
                            (static_cast<uint32_t>(t_bytes[6]) << 8) | static_cast<uint32_t>(t_bytes[7]);
        frame.flags = t_bytes[8];
        std::memcpy(frame.tag.data(), t_bytes.data() + 9, kTagSize);
        return frame;
    }
};

struct EncryptChunkParams {
    const StreamHeader& header;
    const Dek& dek;
    uint32_t chunk_idx{0};
    bool is_final{false};
    std::span<const uint8_t> plaintext;
};

struct EncryptedChunk {
    ChunkFrameHeader frame;
    std::vector<uint8_t> ciphertext;
};

Result<EncryptedChunk> encryptChunk(const EncryptChunkParams& t_params) {
    const uint8_t flags = t_params.is_final ? kChunkFlagFinal : 0;
    auto nonce = buildNonce(t_params.header.nonce_prefix, t_params.chunk_idx);
    auto aad = buildChunkAad(t_params.header.key_id, t_params.chunk_idx, flags, t_params.header.total_plaintext_size);

    EncryptedChunk result;
    result.frame.chunk_idx = t_params.chunk_idx;
    result.frame.flags = flags;

    auto enc_res = encryptChunkGcm(t_params.plaintext, t_params.dek, nonce, aad, result.ciphertext, result.frame.tag);
    SGRN_RETURN_ERROR_IF(enc_res.hasError(), fmt::format("Encryption failure: {}", enc_res.error()));

    result.frame.payload_len = static_cast<uint32_t>(result.ciphertext.size());
    return result;
}

struct DecryptChunkParams {
    const StreamHeader& header;
    const Dek& dek;
    const ChunkFrameHeader& frame;
    std::span<const uint8_t> ciphertext;
};

Result<void> decryptChunk(const DecryptChunkParams& t_params, std::vector<uint8_t>& t_out_plain) {
    auto nonce = buildNonce(t_params.header.nonce_prefix, t_params.frame.chunk_idx);
    auto aad = buildChunkAad(t_params.header.key_id, t_params.frame.chunk_idx, t_params.frame.flags, t_params.header.total_plaintext_size);

    auto dec_res = decryptChunkGcm(t_params.ciphertext, t_params.dek, nonce, aad, t_params.frame.tag, t_out_plain);
    SGRN_RETURN_ERROR_IF(dec_res.hasError(), fmt::format("AEAD tag verification failed for chunk {}", t_params.frame.chunk_idx));
    return {};
}

} // namespace

std::vector<uint8_t> StreamHeader::serialize() const {
    std::vector<uint8_t> out;
    out.reserve(kStreamMagic.size() + 4 + kNoncePrefixSize + 8 + 2 + key_id.size());

    // 1. Magic (8B)
    out.insert(out.end(), kStreamMagic.begin(), kStreamMagic.end());

    // 2. Chunk size (4B big-endian)
    const uint32_t csz = static_cast<uint32_t>(chunk_size);
    out.push_back(static_cast<uint8_t>((csz >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((csz >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((csz >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(csz & 0xFF));

    // 3. Nonce prefix (8B)
    out.insert(out.end(), nonce_prefix.begin(), nonce_prefix.end());

    // 4. Total plaintext size (8B big-endian)
    const uint64_t psz = static_cast<uint64_t>(total_plaintext_size);
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<uint8_t>((psz >> (i * 8)) & 0xFF));
    }

    // 5. Key ID (2B length + UTF-8 bytes)
    const uint16_t kid_len = static_cast<uint16_t>(key_id.size());
    out.push_back(static_cast<uint8_t>((kid_len >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(kid_len & 0xFF));
    out.insert(out.end(), key_id.begin(), key_id.end());

    return out;
}

Result<StreamHeader> StreamHeader::deserialize(std::span<const uint8_t> t_bytes, std::size_t& t_out_header_bytes_read) {
    constexpr std::size_t kMinHeaderSize = 8 + 4 + 8 + 8 + 2; // 30 bytes
    SGRN_RETURN_ERROR_IF(t_bytes.size() < kMinHeaderSize, "Truncated stream header: buffer too small");

    // 1. Verify Magic
    SGRN_RETURN_ERROR_IF(
        std::memcmp(t_bytes.data(), kStreamMagic.data(), kStreamMagic.size()) != 0, "Corrupted stream header: magic bytes mismatch");
    std::size_t pos = 8;

    // 2. Chunk size
    const uint32_t csz = (static_cast<uint32_t>(t_bytes[pos]) << 24) | (static_cast<uint32_t>(t_bytes[pos + 1]) << 16) |
                         (static_cast<uint32_t>(t_bytes[pos + 2]) << 8) | static_cast<uint32_t>(t_bytes[pos + 3]);
    pos += 4;
    SGRN_RETURN_ERROR_IF(csz < 64 || csz > 64 * 1024 * 1024, "Corrupted stream header: invalid chunk size");

    // 3. Nonce prefix
    std::array<uint8_t, kNoncePrefixSize> prefix{};
    std::memcpy(prefix.data(), t_bytes.data() + pos, kNoncePrefixSize);
    pos += kNoncePrefixSize;

    // 4. Total plaintext size
    uint64_t psz = 0;
    for (int i = 0; i < 8; ++i) {
        psz = (psz << 8) | static_cast<uint64_t>(t_bytes[pos + i]);
    }
    pos += 8;

    // 5. Key ID
    const uint16_t kid_len = (static_cast<uint16_t>(t_bytes[pos]) << 8) | static_cast<uint16_t>(t_bytes[pos + 1]);
    pos += 2;
    SGRN_RETURN_ERROR_IF(t_bytes.size() < pos + kid_len, "Truncated stream header: incomplete key_id");

    std::string kid(reinterpret_cast<const char*>(t_bytes.data() + pos), kid_len);
    pos += kid_len;

    t_out_header_bytes_read = pos;

    StreamHeader header;
    header.chunk_size = csz;
    header.nonce_prefix = prefix;
    header.total_plaintext_size = static_cast<int64_t>(psz);
    header.key_id = std::move(kid);

    return header;
}

Result<std::vector<uint8_t>> encryptBuffer(
    std::span<const uint8_t> t_plaintext, const Dek& t_dek, std::string_view t_key_id, std::size_t t_chunk_size) {
    SGRN_RETURN_ERROR_IF(!t_dek.isValid(), "Cannot encrypt: DEK is invalid or uninitialized");

    auto prefix_res = generateNoncePrefix();
    SGRN_RETURN_ERROR_IF(prefix_res.hasError(), prefix_res.error());

    StreamHeader header;
    header.chunk_size = t_chunk_size;
    header.nonce_prefix = prefix_res.value();
    header.total_plaintext_size = static_cast<int64_t>(t_plaintext.size());
    header.key_id = std::string(t_key_id);

    std::vector<uint8_t> out = header.serialize();

    const std::size_t total_len = t_plaintext.size();
    std::size_t offset = 0;
    uint32_t chunk_idx = 0;

    do {
        const std::size_t remaining = total_len - offset;
        const std::size_t this_chunk_len = (remaining > t_chunk_size) ? t_chunk_size : remaining;
        const bool is_final = (offset + this_chunk_len == total_len);

        auto enc_res = encryptChunk(EncryptChunkParams{
            .header = header,
            .dek = t_dek,
            .chunk_idx = chunk_idx,
            .is_final = is_final,
            .plaintext = t_plaintext.subspan(offset, this_chunk_len),
        });
        SGRN_RETURN_ERROR_IF(enc_res.hasError(), enc_res.error());

        const auto& enc_chunk = enc_res.value();
        enc_chunk.frame.writeTo(out);
        out.insert(out.end(), enc_chunk.ciphertext.begin(), enc_chunk.ciphertext.end());

        offset += this_chunk_len;
        ++chunk_idx;
    } while (offset < total_len);

    return out;
}

Result<std::vector<uint8_t>> decryptBuffer(std::span<const uint8_t> t_ciphertext, const Dek& t_dek, std::string_view t_expected_key_id) {
    SGRN_RETURN_ERROR_IF(!t_dek.isValid(), "Cannot decrypt: DEK is invalid or uninitialized");

    std::size_t header_len = 0;
    auto header_res = StreamHeader::deserialize(t_ciphertext, header_len);
    SGRN_RETURN_ERROR_IF(header_res.hasError(), fmt::format("Corrupted header: {}", header_res.error()));
    const auto& header = header_res.value();

    SGRN_RETURN_ERROR_IF(!t_expected_key_id.empty() && header.key_id != t_expected_key_id, "Key ID mismatch in stream header");

    std::vector<uint8_t> plaintext;
    if (header.total_plaintext_size >= 0) {
        plaintext.reserve(static_cast<std::size_t>(header.total_plaintext_size));
    }

    std::size_t pos = header_len;
    uint32_t expected_chunk_idx = 0;
    bool found_final = false;

    while (pos < t_ciphertext.size()) {
        SGRN_RETURN_ERROR_IF(found_final, "Unexpected trailing data after final chunk");
        SGRN_RETURN_ERROR_IF(t_ciphertext.size() - pos < kChunkHeaderSize, "Truncated stream: incomplete chunk header");

        auto frame_res =
            ChunkFrameHeader::deserialize(std::span<const uint8_t, kChunkHeaderSize>(t_ciphertext.data() + pos, kChunkHeaderSize));
        SGRN_RETURN_ERROR_IF(frame_res.hasError(), frame_res.error());
        const auto& frame = frame_res.value();
        pos += kChunkHeaderSize;

        SGRN_RETURN_ERROR_IF(frame.chunk_idx != expected_chunk_idx, "Chunk out of order");
        SGRN_RETURN_ERROR_IF(t_ciphertext.size() - pos < frame.payload_len, "Truncated stream: incomplete chunk payload");

        auto chunk_cipher = t_ciphertext.subspan(pos, frame.payload_len);
        pos += frame.payload_len;

        std::vector<uint8_t> chunk_plain;
        auto dec_res = decryptChunk(
            DecryptChunkParams{
                .header = header,
                .dek = t_dek,
                .frame = frame,
                .ciphertext = chunk_cipher,
            },
            chunk_plain);
        SGRN_RETURN_ERROR_IF(dec_res.hasError(), dec_res.error());

        plaintext.insert(plaintext.end(), chunk_plain.begin(), chunk_plain.end());
        if (frame.isFinal()) {
            found_final = true;
        }
        ++expected_chunk_idx;
    }

    SGRN_RETURN_ERROR_IF(!found_final, "Truncated stream: missing final chunk");

    return plaintext;
}

Result<std::vector<uint8_t>> decryptRange(
    std::span<const uint8_t> t_ciphertext, const Dek& t_dek, uint64_t t_offset, uint64_t t_length, std::string_view t_expected_key_id) {
    if (t_length == 0) {
        return std::vector<uint8_t>{};
    }

    SGRN_RETURN_ERROR_IF(!t_dek.isValid(), "Cannot decrypt: DEK is invalid or uninitialized");

    std::size_t header_len = 0;
    auto header_res = StreamHeader::deserialize(t_ciphertext, header_len);
    SGRN_RETURN_ERROR_IF(header_res.hasError(), fmt::format("Corrupted header: {}", header_res.error()));
    const auto& header = header_res.value();

    SGRN_RETURN_ERROR_IF(!t_expected_key_id.empty() && header.key_id != t_expected_key_id, "Key ID mismatch in stream header");

    uint64_t effective_length = t_length;
    if (header.total_plaintext_size >= 0) {
        const uint64_t total_sz = static_cast<uint64_t>(header.total_plaintext_size);
        SGRN_RETURN_ERROR_IF(t_offset >= total_sz, "Range offset out of bounds");
        if (t_offset + effective_length > total_sz) {
            effective_length = total_sz - t_offset;
        }
    }

    const std::size_t chunk_size = header.chunk_size;
    const uint32_t first_chunk = static_cast<uint32_t>(t_offset / chunk_size);
    const uint32_t last_chunk = static_cast<uint32_t>((t_offset + effective_length - 1) / chunk_size);

    const std::size_t normal_chunk_frame_size = kChunkHeaderSize + chunk_size;

    std::vector<uint8_t> range_result;
    range_result.reserve(static_cast<std::size_t>(effective_length));

    for (uint32_t chunk_idx = first_chunk; chunk_idx <= last_chunk; ++chunk_idx) {
        const std::size_t chunk_pos = header_len + static_cast<std::size_t>(chunk_idx) * normal_chunk_frame_size;
        SGRN_RETURN_ERROR_IF(chunk_pos >= t_ciphertext.size() || t_ciphertext.size() - chunk_pos < kChunkHeaderSize,
            fmt::format("Truncated stream while seeking chunk {}", chunk_idx));

        auto frame_res =
            ChunkFrameHeader::deserialize(std::span<const uint8_t, kChunkHeaderSize>(t_ciphertext.data() + chunk_pos, kChunkHeaderSize));
        SGRN_RETURN_ERROR_IF(frame_res.hasError(), frame_res.error());
        const auto& frame = frame_res.value();

        SGRN_RETURN_ERROR_IF(frame.chunk_idx != chunk_idx, "Chunk index mismatch during random seek");

        const std::size_t payload_pos = chunk_pos + kChunkHeaderSize;
        SGRN_RETURN_ERROR_IF(t_ciphertext.size() - payload_pos < frame.payload_len, "Truncated chunk payload during random seek");

        auto chunk_cipher = t_ciphertext.subspan(payload_pos, frame.payload_len);

        std::vector<uint8_t> chunk_plain;
        auto dec_res = decryptChunk(
            DecryptChunkParams{
                .header = header,
                .dek = t_dek,
                .frame = frame,
                .ciphertext = chunk_cipher,
            },
            chunk_plain);
        SGRN_RETURN_ERROR_IF(dec_res.hasError(), dec_res.error());

        const uint64_t chunk_start_offset = static_cast<uint64_t>(chunk_idx) * chunk_size;
        const uint64_t chunk_end_offset = chunk_start_offset + chunk_plain.size();

        const uint64_t slice_start = (t_offset > chunk_start_offset) ? (t_offset - chunk_start_offset) : 0;
        const uint64_t target_end = t_offset + effective_length;
        const uint64_t slice_end = (target_end < chunk_end_offset) ? (target_end - chunk_start_offset) : chunk_plain.size();

        if (slice_start < slice_end && slice_start < chunk_plain.size()) {
            range_result.insert(range_result.end(), chunk_plain.begin() + slice_start, chunk_plain.begin() + slice_end);
        }
    }

    return range_result;
}

Result<uint64_t> encryptStream(
    std::istream& t_in_stream, std::ostream& t_out_stream, const Dek& t_dek, std::string_view t_key_id, std::size_t t_chunk_size) {
    SGRN_RETURN_ERROR_IF(!t_dek.isValid(), "Cannot encrypt: DEK is invalid or uninitialized");

    auto prefix_res = generateNoncePrefix();
    SGRN_RETURN_ERROR_IF(prefix_res.hasError(), prefix_res.error());

    // Streaming mode: total plaintext size may be unknown (-1)
    StreamHeader header;
    header.chunk_size = t_chunk_size;
    header.nonce_prefix = prefix_res.value();
    header.total_plaintext_size = -1;
    header.key_id = std::string(t_key_id);

    const auto header_bytes = header.serialize();
    t_out_stream.write(reinterpret_cast<const char*>(header_bytes.data()), static_cast<std::streamsize>(header_bytes.size()));
    SGRN_RETURN_ERROR_IF(!t_out_stream, "Failed to write stream header to output");

    std::vector<uint8_t> buffer(t_chunk_size);
    uint32_t chunk_idx = 0;
    uint64_t total_written = header_bytes.size();

    while (t_in_stream) {
        t_in_stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(t_chunk_size));
        const std::streamsize bytes_read = t_in_stream.gcount();
        if (bytes_read <= 0 && chunk_idx > 0) {
            break;
        }

        // Check if this is the final chunk (peek or EOF)
        const bool is_final = (bytes_read < static_cast<std::streamsize>(t_chunk_size)) || (t_in_stream.peek() == EOF);

        auto enc_res = encryptChunk(EncryptChunkParams{
            .header = header,
            .dek = t_dek,
            .chunk_idx = chunk_idx,
            .is_final = is_final,
            .plaintext = std::span<const uint8_t>(buffer.data(), static_cast<std::size_t>(bytes_read)),
        });
        SGRN_RETURN_ERROR_IF(enc_res.hasError(), fmt::format("Streaming encryption failure: {}", enc_res.error()));

        const auto& enc_chunk = enc_res.value();
        uint8_t frame_hdr[kChunkHeaderSize];
        enc_chunk.frame.writeTo(frame_hdr);

        t_out_stream.write(reinterpret_cast<const char*>(frame_hdr), kChunkHeaderSize);
        if (!enc_chunk.ciphertext.empty()) {
            t_out_stream.write(
                reinterpret_cast<const char*>(enc_chunk.ciphertext.data()), static_cast<std::streamsize>(enc_chunk.ciphertext.size()));
        }
        total_written += kChunkHeaderSize + enc_chunk.ciphertext.size();

        ++chunk_idx;
        if (is_final) {
            break;
        }
    }

    return total_written;
}

Result<uint64_t> decryptStream(
    std::istream& t_in_stream, std::ostream& t_out_stream, const Dek& t_dek, std::string_view t_expected_key_id) {
    SGRN_RETURN_ERROR_IF(!t_dek.isValid(), "Cannot decrypt: DEK is invalid or uninitialized");

    // Read fixed 30 bytes of header first
    constexpr std::size_t kFixedHeaderSize = 30;
    std::vector<uint8_t> header_buf(kFixedHeaderSize);
    t_in_stream.read(reinterpret_cast<char*>(header_buf.data()), kFixedHeaderSize);
    SGRN_RETURN_ERROR_IF(t_in_stream.gcount() < static_cast<std::streamsize>(kFixedHeaderSize), "Truncated stream: incomplete header");

    const uint16_t kid_len = (static_cast<uint16_t>(header_buf[28]) << 8) | static_cast<uint16_t>(header_buf[29]);
    std::vector<uint8_t> kid_buf(kid_len);
    if (kid_len > 0) {
        t_in_stream.read(reinterpret_cast<char*>(kid_buf.data()), kid_len);
        SGRN_RETURN_ERROR_IF(t_in_stream.gcount() < static_cast<std::streamsize>(kid_len), "Truncated stream: incomplete key ID in header");
        header_buf.insert(header_buf.end(), kid_buf.begin(), kid_buf.end());
    }

    std::size_t header_len = 0;
    auto header_res = StreamHeader::deserialize(header_buf, header_len);
    SGRN_RETURN_ERROR_IF(header_res.hasError(), fmt::format("Corrupted header: {}", header_res.error()));
    const auto& header = header_res.value();

    SGRN_RETURN_ERROR_IF(!t_expected_key_id.empty() && header.key_id != t_expected_key_id, "Key ID mismatch in stream header");

    uint32_t expected_chunk_idx = 0;
    uint64_t total_decrypted = 0;
    bool found_final = false;

    uint8_t frame_buf[kChunkHeaderSize];
    while (t_in_stream.read(reinterpret_cast<char*>(frame_buf), kChunkHeaderSize)) {
        SGRN_RETURN_ERROR_IF(found_final, "Unexpected trailing data after final chunk");

        auto frame_res = ChunkFrameHeader::deserialize(std::span<const uint8_t, kChunkHeaderSize>(frame_buf, kChunkHeaderSize));
        SGRN_RETURN_ERROR_IF(frame_res.hasError(), frame_res.error());
        const auto& frame = frame_res.value();

        SGRN_RETURN_ERROR_IF(frame.chunk_idx != expected_chunk_idx,
            fmt::format("Chunk out of order: expected {} but got {}", expected_chunk_idx, frame.chunk_idx));

        std::vector<uint8_t> chunk_cipher(frame.payload_len);
        if (frame.payload_len > 0) {
            t_in_stream.read(reinterpret_cast<char*>(chunk_cipher.data()), frame.payload_len);
            SGRN_RETURN_ERROR_IF(
                t_in_stream.gcount() < static_cast<std::streamsize>(frame.payload_len), "Truncated stream: incomplete chunk payload");
        }

        std::vector<uint8_t> chunk_plain;
        auto dec_res = decryptChunk(
            DecryptChunkParams{
                .header = header,
                .dek = t_dek,
                .frame = frame,
                .ciphertext = chunk_cipher,
            },
            chunk_plain);
        SGRN_RETURN_ERROR_IF(dec_res.hasError(), dec_res.error());

        if (!chunk_plain.empty()) {
            t_out_stream.write(reinterpret_cast<const char*>(chunk_plain.data()), static_cast<std::streamsize>(chunk_plain.size()));
            total_decrypted += chunk_plain.size();
        }

        if (frame.isFinal()) {
            found_final = true;
        }
        ++expected_chunk_idx;
    }

    SGRN_RETURN_ERROR_IF(!found_final, "Truncated stream: missing final chunk");

    return total_decrypted;
}

} // namespace sgrn::utils::crypto
