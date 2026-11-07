#pragma once

#include "sgrn/utils/crypto/dek.hpp"
#include <sgrn/Result.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sgrn::utils::crypto
{

enum class CryptoError {
    OK = 0,
    INVALID_KEY,
    INVALID_NONCE,
    TAG_MISMATCH,
    TRUNCATED_STREAM,
    UNEXPECTED_TRAILING_DATA,
    CORRUPTED_HEADER,
    CHUNK_OUT_OF_ORDER,
    WRONG_KEY_ID,
    DEK_SHREDDED,
    RANGE_OUT_OF_BOUNDS,
    INTERNAL_ERROR
};

[[nodiscard]] std::string cryptoErrorToString(CryptoError t_err);

class CryptoException : public std::runtime_error {
public:
    CryptoException(CryptoError t_err, const std::string& t_msg);
    [[nodiscard]] CryptoError errorCode() const noexcept {
        return m_error;
    }

private:
    CryptoError m_error;
};

// ── Constants ────────────────────────────────────────────────────────────────
inline constexpr std::size_t kDefaultChunkSize = 1024 * 1024; // 1 MiB
inline constexpr std::size_t kNoncePrefixSize = 8;            // 64 bits random per file
inline constexpr std::size_t kNonceCounterSize = 4;           // 32 bits chunk counter
inline constexpr std::size_t kNonceSize = 12;                 // 96 bits standard AES-GCM IV
inline constexpr std::size_t kTagSize = 16;                   // 128 bits GCM tag
inline constexpr std::array<uint8_t, 8> kStreamMagic = {'S', 'G', 'R', 'N', 'E', 'N', 'C', '1'};

// Chunk flags
inline constexpr uint8_t kChunkFlagFinal = 0x01;

// ── Nonce & AAD Utilities ────────────────────────────────────────────────────

/**
 * @brief Generate a cryptographically secure 64-bit nonce prefix.
 */
::sgrn::Result<std::array<uint8_t, kNoncePrefixSize>> generateNoncePrefix();

/**
 * @brief Construct a 12-byte AES-GCM nonce: 8-byte prefix || 4-byte big-endian chunk counter.
 */
std::array<uint8_t, kNonceSize> buildNonce(std::span<const uint8_t, kNoncePrefixSize> t_prefix, uint32_t t_chunk_index);

/**
 * @brief Build canonical Additional Authenticated Data (AAD) for a chunk.
 * Canonical representation: key_id || chunk_index (4B BE) || flags (1B) || total_plaintext_size (8B BE).
 */
std::vector<uint8_t> buildChunkAad(std::string_view t_key_id, uint32_t t_chunk_index, uint8_t t_flags, int64_t t_total_plaintext_size = -1);

// ── Single Chunk AEAD Operations ─────────────────────────────────────────────

/**
 * @brief Encrypt a single data chunk using AES-256-GCM.
 */
::sgrn::Result<void> encryptChunkGcm(std::span<const uint8_t> t_plaintext, const Dek& t_dek, std::span<const uint8_t, kNonceSize> t_nonce,
    std::span<const uint8_t> t_aad, std::vector<uint8_t>& t_out_ciphertext, std::array<uint8_t, kTagSize>& t_out_tag);

/**
 * @brief Decrypt and verify a single data chunk using AES-256-GCM.
 */
::sgrn::Result<void> decryptChunkGcm(std::span<const uint8_t> t_ciphertext, const Dek& t_dek, std::span<const uint8_t, kNonceSize> t_nonce,
    std::span<const uint8_t> t_aad, std::span<const uint8_t, kTagSize> t_tag, std::vector<uint8_t>& t_out_plaintext);

} // namespace sgrn::utils::crypto
