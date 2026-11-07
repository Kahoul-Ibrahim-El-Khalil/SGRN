#include <sgrn/utils/crypto.hpp>
#include <sgrn/utils/encoding.hpp>

#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace sgrn::utils::crypto
{

// ── Key Generators ──────────────────────────────────────────────────────────

::sgrn::Result<std::vector<uint8_t>> generateRandomBytes(std::size_t count) {
    if (count == 0) {
        return std::vector<uint8_t>{};
    }

    std::vector<uint8_t> bytes(count);
    if (RAND_bytes(bytes.data(), static_cast<int>(count)) != 1) {
        return ::sgrn::Result<std::vector<uint8_t>>::Error("OpenSSL RAND_bytes failed to generate secure random bytes");
    }
    return bytes;
}

::sgrn::Result<std::string> generateRandomKeyHex(std::size_t byte_count) {
    auto bytes_res = generateRandomBytes(byte_count);
    if (bytes_res.hasError()) {
        return ::sgrn::Result<std::string>::Error(bytes_res.error());
    }
    return sgrn::utils::encoding::toHex(bytes_res.value().data(), bytes_res.value().size());
}

::sgrn::Result<Ed25519KeyPair> generateEd25519KeyPair() {
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!pctx) {
        return ::sgrn::Result<Ed25519KeyPair>::Error("Failed to allocate EVP_PKEY_CTX for Ed25519");
    }

    EVP_PKEY* pkey = nullptr;
    if (EVP_PKEY_keygen_init(pctx) <= 0 || EVP_PKEY_keygen(pctx, &pkey) <= 0 || !pkey) {
        EVP_PKEY_CTX_free(pctx);
        return ::sgrn::Result<Ed25519KeyPair>::Error("Failed to generate Ed25519 key pair");
    }
    EVP_PKEY_CTX_free(pctx);

    size_t priv_len = 0;
    size_t pub_len = 0;
    EVP_PKEY_get_raw_private_key(pkey, nullptr, &priv_len);
    EVP_PKEY_get_raw_public_key(pkey, nullptr, &pub_len);

    std::vector<uint8_t> priv_bytes(priv_len);
    std::vector<uint8_t> pub_bytes(pub_len);

    if (EVP_PKEY_get_raw_private_key(pkey, priv_bytes.data(), &priv_len) != 1 ||
        EVP_PKEY_get_raw_public_key(pkey, pub_bytes.data(), &pub_len) != 1) {
        EVP_PKEY_free(pkey);
        return ::sgrn::Result<Ed25519KeyPair>::Error("Failed to extract raw Ed25519 keys");
    }

    Ed25519KeyPair kp;
    kp.private_key_hex = sgrn::utils::encoding::toHex(priv_bytes.data(), priv_len);
    kp.public_key_hex = sgrn::utils::encoding::toHex(pub_bytes.data(), pub_len);
    kp.private_key_bytes = std::move(priv_bytes);
    kp.public_key_bytes = std::move(pub_bytes);

    EVP_PKEY_free(pkey);
    return kp;
}

bool generateEd25519KeyPair(std::string& out_priv_hex, std::string& out_pub_hex) {
    auto kp_res = generateEd25519KeyPair();
    if (kp_res.hasError()) {
        return false;
    }
    out_priv_hex = std::move(kp_res.value().private_key_hex);
    out_pub_hex = std::move(kp_res.value().public_key_hex);
    return true;
}

// ── Ed25519 Sign & Verify ───────────────────────────────────────────────────

::sgrn::Result<std::string> signEd25519(std::string_view message, std::string_view priv_hex) {
    auto priv_bytes = sgrn::utils::encoding::fromHex(priv_hex);
    if (!priv_bytes.has_value() || priv_bytes->empty()) {
        return ::sgrn::Result<std::string>::Error("Invalid hex encoded private key");
    }

    EVP_PKEY* pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, priv_bytes->data(), priv_bytes->size());
    if (!pkey) {
        return ::sgrn::Result<std::string>::Error("Failed to initialize EVP_PKEY from raw Ed25519 private key");
    }

    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);
        return ::sgrn::Result<std::string>::Error("Failed to allocate EVP_MD_CTX for Ed25519 signing");
    }

    if (EVP_DigestSignInit(md_ctx, nullptr, nullptr, nullptr, pkey) <= 0) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        return ::sgrn::Result<std::string>::Error("EVP_DigestSignInit failed");
    }

    size_t sig_len = 0;
    if (EVP_DigestSign(md_ctx, nullptr, &sig_len, reinterpret_cast<const unsigned char*>(message.data()), message.size()) <= 0) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        return ::sgrn::Result<std::string>::Error("EVP_DigestSign failed to determine signature length");
    }

    std::vector<unsigned char> sig(sig_len);
    if (EVP_DigestSign(md_ctx, sig.data(), &sig_len, reinterpret_cast<const unsigned char*>(message.data()), message.size()) <= 0) {
        EVP_MD_CTX_free(md_ctx);
        EVP_PKEY_free(pkey);
        return ::sgrn::Result<std::string>::Error("EVP_DigestSign failed to produce signature");
    }

    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);

    return sgrn::utils::encoding::toHex(sig.data(), sig_len);
}

bool signEd25519(const std::string& message, const std::string& priv_hex, std::string& out_sig_hex) {
    auto sig_res = signEd25519(message, priv_hex);
    if (sig_res.hasError()) {
        return false;
    }
    out_sig_hex = std::move(sig_res.value());
    return true;
}

bool verifyEd25519(std::string_view message, std::string_view sig_hex, std::string_view pub_hex) {
    auto pub_bytes = sgrn::utils::encoding::fromHex(pub_hex);
    auto sig_bytes = sgrn::utils::encoding::fromHex(sig_hex);
    if (!pub_bytes.has_value() || !sig_bytes.has_value() || pub_bytes->empty() || sig_bytes->empty()) {
        return false;
    }

    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, pub_bytes->data(), pub_bytes->size());
    if (!pkey) {
        return false;
    }

    EVP_MD_CTX* md_ctx = EVP_MD_CTX_new();
    if (!md_ctx) {
        EVP_PKEY_free(pkey);
        return false;
    }

    bool ok = false;
    if (EVP_DigestVerifyInit(md_ctx, nullptr, nullptr, nullptr, pkey) > 0) {
        int vres = EVP_DigestVerify(
            md_ctx, sig_bytes->data(), sig_bytes->size(), reinterpret_cast<const unsigned char*>(message.data()), message.size());
        ok = (vres == 1);
    }

    EVP_MD_CTX_free(md_ctx);
    EVP_PKEY_free(pkey);
    return ok;
}

} // namespace sgrn::utils::crypto
