#pragma once

#include <sgrn/Result.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>

namespace sgrn::utils::crypto
{

/**
 * @brief Data Encryption Key (DEK) for AES-256-GCM.
 *
 * Move-only type containing 32 random bytes from OpenSSL CSPRNG.
 * Automatically zeroized with OPENSSL_cleanse on destruction.
 * Output streaming (operator<<) and fmt formatting are explicitly deleted
 * to prevent accidental logging or printing of key bytes.
 */
class Dek final {
public:
    static constexpr std::size_t kKeySize = 32; // 256 bits

    /// Create an uninitialized DEK
    Dek() noexcept;

    /// Destructor zeroizes key bytes using OPENSSL_cleanse
    ~Dek() noexcept;

    // Move-only: copying is deleted
    Dek(const Dek&) = delete;
    Dek& operator=(const Dek&) = delete;

    Dek(Dek&& t_other) noexcept;
    Dek& operator=(Dek&& t_other) noexcept;

    /// Generate a cryptographically secure 32-byte DEK
    static ::sgrn::Result<Dek> generate();

    /// Create a DEK from a 32-byte span
    static ::sgrn::Result<Dek> fromBytes(std::span<const uint8_t> t_bytes);

    /// Raw key data access (for cryptographic routines only)
    [[nodiscard]] const uint8_t* data() const noexcept {
        return m_key.data();
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return kKeySize;
    }
    [[nodiscard]] std::span<const uint8_t, kKeySize> span() const noexcept {
        return std::span<const uint8_t, kKeySize>(m_key.data(), kKeySize);
    }
    [[nodiscard]] bool isValid() const noexcept {
        return m_valid;
    }

    /// Explicitly zeroize the key buffer
    void zeroize() noexcept;

    /// Constant-time equality comparison using CRYPTO_memcmp
    [[nodiscard]] bool equals(const Dek& t_other) const noexcept;

    /// Create a deep clone (copy) of the key bytes (explicitly named)
    [[nodiscard]] ::sgrn::Result<Dek> clone() const;

    // Prohibit accidental printing or serialization
    friend std::ostream& operator<<(std::ostream&, const Dek&) = delete;

private:
    alignas(16) std::array<uint8_t, kKeySize> m_key{};
    bool m_valid{false};
};

} // namespace sgrn::utils::crypto
