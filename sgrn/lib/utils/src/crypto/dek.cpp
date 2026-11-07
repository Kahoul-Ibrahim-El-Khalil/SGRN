#include "sgrn/utils/crypto/dek.hpp"

#include <cstring>
#include <openssl/crypto.h>
#include <openssl/rand.h>

namespace sgrn::utils::crypto
{

Dek::Dek() noexcept
    : m_valid(false) {
    m_key.fill(0);
}

Dek::~Dek() noexcept {
    zeroize();
}

Dek::Dek(Dek&& t_other) noexcept
    : m_key(t_other.m_key)
    , m_valid(t_other.m_valid) {
    t_other.zeroize();
}

Dek& Dek::operator=(Dek&& t_other) noexcept {
    if (this != &t_other) {
        zeroize();
        m_key = t_other.m_key;
        m_valid = t_other.m_valid;
        t_other.zeroize();
    }
    return *this;
}

void Dek::zeroize() noexcept {
    OPENSSL_cleanse(m_key.data(), m_key.size());
    m_valid = false;
}

Result<Dek> Dek::generate() {
    Dek dek;
    SGRN_RETURN_ERROR_IF(RAND_bytes(dek.m_key.data(), static_cast<int>(kKeySize)) != 1, "Failed to generate random bytes for DEK");
    dek.m_valid = true;
    return dek;
}

Result<Dek> Dek::fromBytes(std::span<const uint8_t> t_bytes) {
    SGRN_RETURN_ERROR_IF(t_bytes.size() != kKeySize, "Invalid key size: expected 32 bytes");
    Dek dek;
    std::memcpy(dek.m_key.data(), t_bytes.data(), kKeySize);
    dek.m_valid = true;
    return dek;
}

bool Dek::equals(const Dek& t_other) const noexcept {
    if (!m_valid || !t_other.m_valid) {
        return m_valid == t_other.m_valid;
    }
    return CRYPTO_memcmp(m_key.data(), t_other.m_key.data(), kKeySize) == 0;
}

Result<Dek> Dek::clone() const {
    SGRN_RETURN_ERROR_IF(!m_valid, "Cannot clone uninitialized DEK");
    return fromBytes(span());
}

} // namespace sgrn::utils::crypto
