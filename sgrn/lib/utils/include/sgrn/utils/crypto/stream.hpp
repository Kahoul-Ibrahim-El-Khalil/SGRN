#pragma once

#include "sgrn/utils/crypto/aead.hpp"
#include "sgrn/utils/crypto/dek.hpp"
#include <sgrn/Result.hpp>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sgrn::utils::crypto
{

struct StreamHeader {
    std::size_t chunk_size{kDefaultChunkSize};
    std::array<uint8_t, kNoncePrefixSize> nonce_prefix{};
    int64_t total_plaintext_size{-1};
    std::string key_id;

    [[nodiscard]] std::vector<uint8_t> serialize() const;
    [[nodiscard]] static ::sgrn::Result<StreamHeader> deserialize(std::span<const uint8_t> t_bytes, std::size_t& t_out_header_bytes_read);
};

// ── In-Memory One-Shot Operations ────────────────────────────────────────────

/**
 * @brief Encrypt a complete plaintext buffer into chunked AEAD ciphertext format.
 */
::sgrn::Result<std::vector<uint8_t>> encryptBuffer(
    std::span<const uint8_t> t_plaintext, const Dek& t_dek, std::string_view t_key_id, std::size_t t_chunk_size = kDefaultChunkSize);

/**
 * @brief Decrypt and verify a complete chunked AEAD ciphertext buffer.
 */
::sgrn::Result<std::vector<uint8_t>> decryptBuffer(
    std::span<const uint8_t> t_ciphertext, const Dek& t_dek, std::string_view t_expected_key_id = "");

/**
 * @brief Random-access decryption of a byte sub-range [offset, offset + length).
 * Decrypts only the specific chunks overlapping the requested range.
 */
::sgrn::Result<std::vector<uint8_t>> decryptRange(
    std::span<const uint8_t> t_ciphertext, const Dek& t_dek, uint64_t t_offset, uint64_t t_length, std::string_view t_expected_key_id = "");

// ── Streaming Operations ─────────────────────────────────────────────────────

/**
 * @brief Stream encryption reading from std::istream and writing to std::ostream.
 */
::sgrn::Result<uint64_t> encryptStream(std::istream& t_in_stream, std::ostream& t_out_stream, const Dek& t_dek, std::string_view t_key_id,
    std::size_t t_chunk_size = kDefaultChunkSize);

/**
 * @brief Stream decryption reading from std::istream and writing to std::ostream.
 */
::sgrn::Result<uint64_t> decryptStream(
    std::istream& t_in_stream, std::ostream& t_out_stream, const Dek& t_dek, std::string_view t_expected_key_id = "");

} // namespace sgrn::utils::crypto
