#pragma once
#include <sgrn/Result.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/encoding.hpp>
#include <array>
#include <filesystem>
#include <fstream>
#include <openssl/evp.h>
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
// 1️⃣ Utility Helpers
// =======================================================

// Helper: Convert Poco's unsigned buffer to our signed buffer
inline Bytes toBytes(const std::vector<unsigned char>& t_src) {
    return Bytes(t_src.begin(), t_src.end());
}

inline std::string getDigestHex(const Bytes& t_buffer) {
    static const char hex_lookup[] = "0123456789abcdef";

    std::string hex;
    hex.reserve(t_buffer.size() * 2);

    for (char c : t_buffer) {
        unsigned char uc = static_cast<unsigned char>(c);
        hex.push_back(hex_lookup[uc >> 4]);
        hex.push_back(hex_lookup[uc & 0x0F]);
    }
    return hex;
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
// 2️⃣ PBKDF2 Password Hashing
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
// 5️⃣ SHA512 Convenience Wrappers (Base64 - Primary)
// =======================================================
enum class HashEncoding { hex, base64, base64url };

inline ::sgrn::Result<std::string> computeSha512Data(std::string_view t_data, HashEncoding t_enc) {
    DigestContext ctx_;
    if (EVP_DigestInit_ex(ctx_.get(), EVP_sha512(), nullptr) != 1 || EVP_DigestUpdate(ctx_.get(), t_data.data(), t_data.size()) != 1) {
        ERROR_LOG("Digest init/update failed");
        return ::sgrn::Result<std::string>::Error("Digest init/update failed");
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
    unsigned int len = 0;

    if (EVP_DigestFinal_ex(ctx_.get(), hash.data(), &len) != 1) {
        return ::sgrn::Result<std::string>::Error("Digest final failed");
    }

    switch (t_enc) {
        case sgrn::utils::HashEncoding::base64:
            return ::sgrn::utils::encoding::toBase64(hash.data(), len);
        case sgrn::utils::HashEncoding::base64url:
            return ::sgrn::utils::encoding::toBase64Url(hash.data(), len);
        case sgrn::utils::HashEncoding::hex:
            return sgrn::utils::encoding::toHex(hash.data(), len);
    }
    return ::sgrn::Result<std::string>::Error("Unknown hash encoding");
}

inline ::sgrn::Result<std::string> computeSha512File(const std::filesystem::path& t_path, HashEncoding t_enc) {
    std::ifstream file(t_path, std::ios::binary);
    if (!file) {
        return ::sgrn::Result<std::string>::Error("Cannot open file: " + t_path.string());
    }
    DigestContext ctx_;

    if (EVP_DigestInit_ex(ctx_.get(), EVP_sha512(), nullptr) != 1) {
        ERROR_LOG("Digest init failed for file: {}", t_path.string());
        return ::sgrn::Result<std::string>::Error("Digest init failed");
    }

    constexpr std::size_t buffer_size = 16 * 1024;
    std::array<char, buffer_size> t_buffer{};

    while (file.good()) {
        file.read(t_buffer.data(), t_buffer.size());
        std::streamsize read = file.gcount();

        if (read > 0) {
            if (EVP_DigestUpdate(ctx_.get(), t_buffer.data(), static_cast<std::size_t>(read)) != 1) {
                return ::sgrn::Result<std::string>::Error("Digest update failed");
            }
        }
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
    unsigned int len = 0;

    if (EVP_DigestFinal_ex(ctx_.get(), hash.data(), &len) != 1) {
        return ::sgrn::Result<std::string>::Error("Digest final failed");
    }
    switch (t_enc) {
        case sgrn::utils::HashEncoding::base64:
            return ::sgrn::utils::encoding::toBase64(hash.data(), len);
        case sgrn::utils::HashEncoding::base64url:
            return ::sgrn::utils::encoding::toBase64Url(hash.data(), len);
        case sgrn::utils::HashEncoding::hex:
            return sgrn::utils::encoding::toHex(hash.data(), len);
    }
    return ::sgrn::Result<std::string>::Error("Unknown hash encoding");
}
} // namespace sgrn::utils

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
