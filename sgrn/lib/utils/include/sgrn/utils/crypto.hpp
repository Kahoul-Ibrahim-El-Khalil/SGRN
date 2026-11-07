#pragma once

#include <sgrn/Result.hpp>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "sgrn/utils/crypto/aead.hpp"
#include "sgrn/utils/crypto/dek.hpp"
#include "sgrn/utils/crypto/key-provider.hpp"
#include "sgrn/utils/crypto/stream.hpp"

namespace sgrn::utils::crypto
{

struct Ed25519KeyPair {
    std::string private_key_hex;
    std::string public_key_hex;
    std::vector<uint8_t> private_key_bytes;
    std::vector<uint8_t> public_key_bytes;
};

// ── Key Generators ──────────────────────────────────────────────────────────

/**
 * @brief Generate a cryptographically secure random byte buffer using OpenSSL RAND_bytes.
 * @param count Number of random bytes to generate.
 * @return Vector of random bytes on success, or error string.
 */
::sgrn::Result<std::vector<uint8_t>> generateRandomBytes(std::size_t count);

/**
 * @brief Generate a cryptographically secure random key encoded in lowercase hexadecimal.
 * @param byte_count Number of random bytes (e.g. 32 for a 256-bit key).
 * @return Hexadecimal key string on success, or error string.
 */
::sgrn::Result<std::string> generateRandomKeyHex(std::size_t byte_count = 32);

/**
 * @brief Generate an Ed25519 key pair using OpenSSL EVP raw key API.
 * @return Ed25519KeyPair on success, or error string.
 */
::sgrn::Result<Ed25519KeyPair> generateEd25519KeyPair();

/**
 * @brief Overload for outputting Ed25519 key pair directly as hex strings.
 * @param out_priv_hex Output string for 32-byte private key (64 hex characters).
 * @param out_pub_hex Output string for 32-byte public key (64 hex characters).
 * @return true on success, false on failure.
 */
bool generateEd25519KeyPair(std::string& out_priv_hex, std::string& out_pub_hex);

// ── Ed25519 Sign & Verify ───────────────────────────────────────────────────

/**
 * @brief Sign message bytes using an Ed25519 raw private key (hex encoded).
 * @param message Message to sign.
 * @param priv_hex 64-character hex encoded Ed25519 private key.
 * @return 64-byte signature encoded as 128-char hex string, or error string.
 */
::sgrn::Result<std::string> signEd25519(std::string_view message, std::string_view priv_hex);

/**
 * @brief Overload for signing with output string parameter.
 * @param message Message to sign.
 * @param priv_hex 64-character hex encoded Ed25519 private key.
 * @param out_sig_hex Output string for 64-byte signature (128 hex characters).
 * @return true on success, false on failure.
 */
bool signEd25519(const std::string& message, const std::string& priv_hex, std::string& out_sig_hex);

/**
 * @brief Verify an Ed25519 signature over a message using a public key (hex encoded).
 * @param message Message that was signed.
 * @param sig_hex 128-character hex encoded signature.
 * @param pub_hex 64-character hex encoded Ed25519 public key.
 * @return true if valid, false otherwise.
 */
bool verifyEd25519(std::string_view message, std::string_view sig_hex, std::string_view pub_hex);

} // namespace sgrn::utils::crypto
