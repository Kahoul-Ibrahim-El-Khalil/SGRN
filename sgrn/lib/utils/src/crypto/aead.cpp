#include "sgrn/utils/crypto/aead.hpp"

#include <sgrn/Result.hpp>
#include <cstring>
#include <memory>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace sgrn::utils::crypto
{

using sgrn::Result;
namespace
{
struct EvpCipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX* ctx) const {
        if (ctx) {
            EVP_CIPHER_CTX_free(ctx);
        }
    }
};
using EvpCipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, EvpCipherCtxDeleter>;
} // namespace

std::string cryptoErrorToString(CryptoError t_err) {
    switch (t_err) {
        case CryptoError::OK:
            return "OK";
        case CryptoError::INVALID_KEY:
            return "INVALID_KEY";
        case CryptoError::INVALID_NONCE:
            return "INVALID_NONCE";
        case CryptoError::TAG_MISMATCH:
            return "TAG_MISMATCH";
        case CryptoError::TRUNCATED_STREAM:
            return "TRUNCATED_STREAM";
        case CryptoError::UNEXPECTED_TRAILING_DATA:
            return "UNEXPECTED_TRAILING_DATA";
        case CryptoError::CORRUPTED_HEADER:
            return "CORRUPTED_HEADER";
        case CryptoError::CHUNK_OUT_OF_ORDER:
            return "CHUNK_OUT_OF_ORDER";
        case CryptoError::WRONG_KEY_ID:
            return "WRONG_KEY_ID";
        case CryptoError::DEK_SHREDDED:
            return "DEK_SHREDDED";
        case CryptoError::RANGE_OUT_OF_BOUNDS:
            return "RANGE_OUT_OF_BOUNDS";
        case CryptoError::INTERNAL_ERROR:
            return "INTERNAL_ERROR";
    }
    return "UNKNOWN";
}

CryptoException::CryptoException(CryptoError t_err, const std::string& t_msg)
    : std::runtime_error(t_msg)
    , m_error(t_err) {
}

Result<std::array<uint8_t, kNoncePrefixSize>> generateNoncePrefix() {
    std::array<uint8_t, kNoncePrefixSize> prefix{};
    RETURN_ERROR_IF(RAND_bytes(prefix.data(), static_cast<int>(prefix.size())) != 1, "Failed to generate random nonce prefix");
    return prefix;
}

std::array<uint8_t, kNonceSize> buildNonce(std::span<const uint8_t, kNoncePrefixSize> t_prefix, uint32_t t_chunk_index) {
    std::array<uint8_t, kNonceSize> nonce{};
    std::memcpy(nonce.data(), t_prefix.data(), kNoncePrefixSize);

    // Big-endian 32-bit chunk counter
    nonce[8] = static_cast<uint8_t>((t_chunk_index >> 24) & 0xFF);
    nonce[9] = static_cast<uint8_t>((t_chunk_index >> 16) & 0xFF);
    nonce[10] = static_cast<uint8_t>((t_chunk_index >> 8) & 0xFF);
    nonce[11] = static_cast<uint8_t>(t_chunk_index & 0xFF);

    return nonce;
}

std::vector<uint8_t> buildChunkAad(std::string_view t_key_id, uint32_t t_chunk_index, uint8_t t_flags, int64_t t_total_plaintext_size) {
    std::vector<uint8_t> aad;
    aad.reserve(t_key_id.size() + 4 + 1 + 8);

    // 1. key_id
    aad.insert(aad.end(), t_key_id.begin(), t_key_id.end());

    // 2. chunk_index (4 bytes big-endian)
    aad.push_back(static_cast<uint8_t>((t_chunk_index >> 24) & 0xFF));
    aad.push_back(static_cast<uint8_t>((t_chunk_index >> 16) & 0xFF));
    aad.push_back(static_cast<uint8_t>((t_chunk_index >> 8) & 0xFF));
    aad.push_back(static_cast<uint8_t>(t_chunk_index & 0xFF));

    // 3. flags (1 byte)
    aad.push_back(t_flags);

    // 4. total_plaintext_size (8 bytes big-endian)
    const uint64_t sz = static_cast<uint64_t>(t_total_plaintext_size);
    for (int i = 7; i >= 0; --i) {
        aad.push_back(static_cast<uint8_t>((sz >> (i * 8)) & 0xFF));
    }

    return aad;
}

Result<void> encryptChunkGcm(std::span<const uint8_t> t_plaintext, const Dek& t_dek, std::span<const uint8_t, kNonceSize> t_nonce,
    std::span<const uint8_t> t_aad, std::vector<uint8_t>& t_out_ciphertext, std::array<uint8_t, kTagSize>& t_out_tag) {

    RETURN_ERROR_IF(!t_dek.isValid(), "Invalid or uninitialized DEK");

    EvpCipherCtxPtr ctx(EVP_CIPHER_CTX_new());
    RETURN_ERROR_IF(!ctx, "Failed to create EVP cipher context");

    RETURN_ERROR_IF(
        EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1, "Failed to initialize AES-256-GCM cipher");

    RETURN_ERROR_IF(
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonceSize), nullptr) != 1, "Failed to set GCM IV length");

    RETURN_ERROR_IF(EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, t_dek.data(), t_nonce.data()) != 1,
        "Failed to set DEK and nonce in cipher context");

    int len = 0;
    // Set AAD
    if (!t_aad.empty()) {
        RETURN_ERROR_IF(
            EVP_EncryptUpdate(ctx.get(), nullptr, &len, t_aad.data(), static_cast<int>(t_aad.size())) != 1, "Failed to set AAD");
    }

    t_out_ciphertext.resize(t_plaintext.size());
    if (!t_plaintext.empty()) {
        RETURN_ERROR_IF(
            EVP_EncryptUpdate(ctx.get(), t_out_ciphertext.data(), &len, t_plaintext.data(), static_cast<int>(t_plaintext.size())) != 1,
            "Failed to encrypt plaintext chunk");
    }

    int final_len = 0;
    RETURN_ERROR_IF(EVP_EncryptFinal_ex(ctx.get(), t_out_ciphertext.data() + len, &final_len) != 1, "Failed to finalize encryption chunk");

    RETURN_ERROR_IF(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, static_cast<int>(kTagSize), t_out_tag.data()) != 1,
        "Failed to obtain GCM tag");

    return {};
}

Result<void> decryptChunkGcm(std::span<const uint8_t> t_ciphertext, const Dek& t_dek, std::span<const uint8_t, kNonceSize> t_nonce,
    std::span<const uint8_t> t_aad, std::span<const uint8_t, kTagSize> t_tag, std::vector<uint8_t>& t_out_plaintext) {

    RETURN_ERROR_IF(!t_dek.isValid(), "Invalid or uninitialized DEK");

    EvpCipherCtxPtr ctx(EVP_CIPHER_CTX_new());
    RETURN_ERROR_IF(!ctx, "Failed to create EVP cipher context");

    const bool initalize_cipher_failed = EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1;
    RETURN_ERROR_IF(initalize_cipher_failed, "Failed to initialize AES-256-GCM cipher");

    RETURN_ERROR_IF(
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(kNonceSize), nullptr) != 1, "Failed to set GCM IV length");

    RETURN_ERROR_IF(EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, t_dek.data(), t_nonce.data()) != 1,
        "Failed to set DEK and nonce in cipher context");

    int len = 0;
    // Set AAD
    if (!t_aad.empty()) {
        RETURN_ERROR_IF(
            EVP_DecryptUpdate(ctx.get(), nullptr, &len, t_aad.data(), static_cast<int>(t_aad.size())) != 1, "Failed to process AAD");
    }

    t_out_plaintext.resize(t_ciphertext.size());
    if (!t_ciphertext.empty()) {
        RETURN_ERROR_IF(
            EVP_DecryptUpdate(ctx.get(), t_out_plaintext.data(), &len, t_ciphertext.data(), static_cast<int>(t_ciphertext.size())) != 1,
            "Failed to decrypt ciphertext chunk");
    }

    // Set expected GCM tag
    RETURN_ERROR_IF(
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, static_cast<int>(kTagSize), const_cast<uint8_t*>(t_tag.data())) != 1,
        "Failed to set expected GCM tag");

    int final_len = 0;
    const int ret = EVP_DecryptFinal_ex(ctx.get(), t_out_plaintext.data() + len, &final_len);

    RETURN_ERROR_IF(ret <= 0, "AEAD tag verification failed: ciphertext or AAD has been tampered with");

    return {};
}

} // namespace sgrn::utils::crypto
