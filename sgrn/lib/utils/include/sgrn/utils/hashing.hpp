#pragma once
#include <sgrn/Result.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/encoding.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef DEBUG_UTIL_HASHING
#define DEBUG_LOG(msg, ...) SGRN_DEBUG("UtilHashing", msg __VA_OPT__(, ) __VA_ARGS__)
#define INFO_LOG(msg, ...) SGRN_INFO("UtilHashing", msg __VA_OPT__(, ) __VA_ARGS__)
#define WARN_LOG(msg, ...) SGRN_WARN("UtilHashing", msg __VA_OPT__(, ) __VA_ARGS__)
#define ERROR_LOG(msg, ...) SGRN_ERROR("UtilHashing", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define DEBUG_LOG(...) ((void)0)
#define INFO_LOG(...) ((void)0)
#define WARN_LOG(...) ((void)0)
#define ERROR_LOG(...) ((void)0)
#endif

namespace sgrn::utils
{

// Drogon/PostgreSQL expects 'char' for BYTEA
using Bytes = std::vector<char>;

// =======================================================
//  Utility Helpers
// =======================================================

// Helper: Convert Poco's unsigned buffer to our signed buffer
inline Bytes toBytes(const std::vector<unsigned char>& t_src) {
    return Bytes(t_src.begin(), t_src.end());
}

inline std::string getDigestHex(const Bytes& t_buffer) {
    return encoding::toHex(reinterpret_cast<const unsigned char*>(t_buffer.data()), t_buffer.size());
}

inline std::string generateSalt(size_t t_length = 16) {
    static thread_local std::mt19937 generator{std::random_device{}()};
    std::string salt_str;
    salt_str.reserve(t_length * 2);

    const char hex_chars[] = "0123456789abcdef";
    std::uniform_int_distribution<int> distribution(0, 15);

    for (size_t i = 0; i < t_length; ++i) {
        int val = distribution(generator);
        salt_str += hex_chars[val];
        val = distribution(generator);
        salt_str += hex_chars[val];
    }
    return salt_str;
}

// =======================================================
//  PBKDF2 Password Hashing
// =======================================================

inline std::string internalPbkdf2(const std::string& t_password, const std::string& t_salt, uint32_t t_iterations) {
    constexpr int key_length = 64;

    Bytes out_buffer(key_length);

    if (PKCS5_PBKDF2_HMAC(t_password.c_str(), t_password.length(), reinterpret_cast<const unsigned char*>(t_salt.c_str()), t_salt.length(),
            t_iterations, EVP_sha512(), key_length, reinterpret_cast<unsigned char*>(out_buffer.data())) != 1) {
        ERROR_LOG("OpenSSL PBKDF2 hashing failed");
        throw std::runtime_error("OpenSSL PBKDF2 hashing failed");
    }

    return getDigestHex(out_buffer);
}

// =======================================================
// 3️⃣ RAII Wrapper for EVP_MD_CTX
// =======================================================

class DigestContext {
public:
    DigestContext()
        : ctx_(EVP_MD_CTX_new()) {
        if (!ctx_) {
            ERROR_LOG("EVP_MD_CTX_new failed");
            throw std::runtime_error("EVP_MD_CTX_new failed");
        }
    }

    ~DigestContext() {
        EVP_MD_CTX_free(ctx_);
    }

    DigestContext(const DigestContext&) = delete;
    DigestContext& operator=(const DigestContext&) = delete;

    EVP_MD_CTX* get() noexcept {
        return ctx_;
    }

private:
    EVP_MD_CTX* ctx_;
};

// =======================================================
//  Digest & HMAC Wrappers
// =======================================================
enum class HashEncoding { hex, base64, base64url, raw };

namespace detail
{

inline ::sgrn::Result<std::string> encodeHash(const unsigned char* tp_data, unsigned int t_len, HashEncoding t_enc) {
    switch (t_enc) {
        case HashEncoding::base64:
            return ::sgrn::utils::encoding::toBase64(tp_data, t_len);
        case HashEncoding::base64url:
            return ::sgrn::utils::encoding::toBase64Url(tp_data, t_len);
        case HashEncoding::hex:
            return ::sgrn::utils::encoding::toHex(tp_data, t_len);
        case HashEncoding::raw:
            return std::string(reinterpret_cast<const char*>(tp_data), t_len);
    }
    return Error("Unknown hash encoding");
}

inline ::sgrn::Result<std::string> computeDigestData(const EVP_MD* tp_md, std::string_view t_data, HashEncoding t_enc) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
    unsigned int len = 0;
    if (EVP_Digest(t_data.data(), t_data.size(), hash.data(), &len, tp_md, nullptr) != 1) {
        ERROR_LOG("EVP_Digest failed");
        return Error("Digest computation failed");
    }
    return encodeHash(hash.data(), len, t_enc);
}

inline ::sgrn::Result<std::string> computeDigestFile(const EVP_MD* tp_md, const std::filesystem::path& t_path, HashEncoding t_enc) {
    std::ifstream file(t_path, std::ios::binary);
    if (!file) {
        return Error("Cannot open file: " + t_path.string());
    }
    DigestContext ctx;

    if (EVP_DigestInit_ex(ctx.get(), tp_md, nullptr) != 1) {
        ERROR_LOG("Digest init failed for file: {}", t_path.string());
        return Error("Digest init failed");
    }

    constexpr std::size_t kBufferSize = 16 * 1024;
    std::array<char, kBufferSize> buffer{};

    while (file.good()) {
        file.read(buffer.data(), buffer.size());
        std::streamsize read = file.gcount();
        if (read > 0) {
            RETURN_ERROR_IF(EVP_DigestUpdate(ctx.get(), buffer.data(), static_cast<std::size_t>(read)) != 1, "Digest update failed");
        }
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
    unsigned int len = 0;
    if (EVP_DigestFinal_ex(ctx.get(), hash.data(), &len) != 1) {
        return Error("Digest final failed");
    }
    return encodeHash(hash.data(), len, t_enc);
}

} // namespace detail

inline ::sgrn::Result<std::string> computeSha512Data(std::string_view t_data, HashEncoding t_enc) {
    return detail::computeDigestData(EVP_sha512(), t_data, t_enc);
}

inline ::sgrn::Result<std::string> computeSha512File(const std::filesystem::path& t_path, HashEncoding t_enc) {
    return detail::computeDigestFile(EVP_sha512(), t_path, t_enc);
}

// =======================================================
//  SHA-256, MD5 & HMAC-SHA256 (protocol-mandated only)
// =======================================================
// SGRN content hashes use SHA-512 (see computeSha512* above). SHA-256 and MD5
// are kept only for protocols that define them (JWT HS256, webhook HMAC,
// AWS SigV4, S3 Content-MD5) and for verifying the tamper-evident audit chain.

inline ::sgrn::Result<std::string> computeSha256Data(std::string_view t_data, HashEncoding t_enc) {
    return detail::computeDigestData(EVP_sha256(), t_data, t_enc);
}

inline ::sgrn::Result<std::string> computeSha256File(const std::filesystem::path& t_path, HashEncoding t_enc) {
    return detail::computeDigestFile(EVP_sha256(), t_path, t_enc);
}

inline ::sgrn::Result<std::string> computeMd5Data(std::string_view t_data, HashEncoding t_enc) {
    return detail::computeDigestData(EVP_md5(), t_data, t_enc);
}

inline ::sgrn::Result<std::string> computeHmacSha256(std::string_view t_key, std::string_view t_data, HashEncoding t_enc) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> out{};
    unsigned int len = 0;
    if (HMAC(EVP_sha256(), t_key.data(), static_cast<int>(t_key.size()), reinterpret_cast<const unsigned char*>(t_data.data()),
            t_data.size(), out.data(), &len) == nullptr) {
        ERROR_LOG("OpenSSL HMAC(SHA-256) failed");
        return Error("HMAC(SHA-256) failed");
    }
    return detail::encodeHash(out.data(), len, t_enc);
}

// =======================================================
//  Streaming SHA-512 Hasher
// =======================================================

class Sha512Stream {
public:
    Sha512Stream() {
        ctx_ = EVP_MD_CTX_new();
        EVP_DigestInit_ex(ctx_, EVP_sha512(), nullptr);
    }

    ~Sha512Stream() {
        if (ctx_ != nullptr) {
            EVP_MD_CTX_free(ctx_);
        }
    }

    Sha512Stream(const Sha512Stream&) = delete;
    Sha512Stream& operator=(const Sha512Stream&) = delete;

    void update(const char* t_data, size_t t_len) {
        EVP_DigestUpdate(ctx_, t_data, t_len);
    }

    void update(const uint8_t* t_data, size_t t_len) {
        EVP_DigestUpdate(ctx_, t_data, t_len);
    }

    void update(std::string_view t_data) {
        EVP_DigestUpdate(ctx_, t_data.data(), t_data.size());
    }

    std::string hex() {
        unsigned char md[EVP_MAX_MD_SIZE] = {};
        unsigned int len = 0;
        EVP_DigestFinal_ex(ctx_, md, &len);
        return sgrn::utils::encoding::toHex(md, len);
    }

private:
    EVP_MD_CTX* ctx_ = nullptr;
};

} // namespace sgrn::utils

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
