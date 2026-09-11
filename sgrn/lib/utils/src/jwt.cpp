#include <sgrn/utils/jwt.hpp>

#include <fmt/core.h>
#include <sgrn/debug.hpp>
#include <sgrn/utils/encoding.hpp>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <vector>

namespace sgrn::utils::jwt
{
using sgrn::Result;
static std::string base64UrlDecode(std::string_view t_input) {
    std::string base64 = std::string(t_input);
    // Replace URL-safe chars with standard base64 chars
    for (char& c : base64) {
        if (c == '-')
            c = '+';
        else if (c == '_')
            c = '/';
    }

    // Add padding if necessary
    while (base64.length() % 4 != 0) {
        base64 += '=';
    }

    // A simple base64 decoder using OpenSSL EVP_DecodeBlock
    // Max size of decoded output is 3/4 the size of input
    std::vector<unsigned char> decoded(base64.length() * 3 / 4 + 1); // +1 because EVP_DecodeBlock may decode extra zero bytes

    // Pass string length instead of relying on null terminator
    int decoded_len = EVP_DecodeBlock(decoded.data(), reinterpret_cast<const unsigned char*>(base64.data()), base64.length());

    if (decoded_len < 0) {
        return ""; // Decode failed
    }

    // EVP_DecodeBlock calculates decoded_len assuming padding bytes are real data
    // We must manually adjust it based on the number of '=' padding characters
    if (base64.length() > 0 && base64[base64.length() - 1] == '=') {
        decoded_len--;
        if (base64.length() > 1 && base64[base64.length() - 2] == '=') {
            decoded_len--;
        }
    }

    return std::string(reinterpret_cast<const char*>(decoded.data()), decoded_len);
}

// Compute HMAC-SHA256 and return base64url encoded result
static std::string computeSignature(std::string_view t_payload, std::string_view t_secret) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    HMAC(EVP_sha256(), t_secret.data(), t_secret.length(), reinterpret_cast<const unsigned char*>(t_payload.data()), t_payload.length(),
        hash, &hash_len);

    return ::sgrn::utils::encoding::toBase64Url(hash, hash_len);
}

Result<std::string> generateToken(const Json::Value& t_claims, const std::string& t_secret) {
    if (t_secret.empty()) {
        return Result<std::string>::Error("JWT signature missing secret");
    }

    // 1. Header (HS256)
    Json::Value header;
    header["alg"] = "HS256";
    header["typ"] = "JWT";

    Json::StreamWriterBuilder builder;
    builder["indentation"] = ""; // compact representation
    std::string header_json = Json::writeString(builder, header);
    std::string header_b64 =
        ::sgrn::utils::encoding::toBase64Url(reinterpret_cast<const unsigned char*>(header_json.data()), header_json.length());

    // 2. Payload
    std::string payload_json = Json::writeString(builder, t_claims);
    std::string payload_b64 =
        ::sgrn::utils::encoding::toBase64Url(reinterpret_cast<const unsigned char*>(payload_json.data()), payload_json.length());

    // 3. Signature
    std::string message = header_b64 + "." + payload_b64;
    std::string signature = computeSignature(message, t_secret);

    std::string t_token = message + "." + signature;
    return t_token;
}

Result<Json::Value> verifyToken(const std::string& t_token, const std::string& t_secret) {
    if (t_secret.empty()) {
        return Result<Json::Value>::Error("JWT verification missing secret");
    }

    // JWT parts: [header].[payload].[signature]
    std::string::size_type dot1 = t_token.find('.');
    if (dot1 == std::string::npos) {
        return Result<Json::Value>::Error("JWT format invalid");
    }

    std::string::size_type dot2 = t_token.find('.', dot1 + 1);
    if (dot2 == std::string::npos) {
        return Result<Json::Value>::Error("JWT format invalid");
    }

    std::string_view header_b64(&t_token[0], dot1);
    std::string_view payload_b64(&t_token[dot1 + 1], dot2 - (dot1 + 1));
    std::string_view signature_b64(&t_token[dot2 + 1], t_token.length() - (dot2 + 1));

    // Verify signature
    std::string message = std::string(header_b64) + "." + std::string(payload_b64);
    std::string expected_signature = computeSignature(message, t_secret);

    // Constant-time compare
    if (signature_b64.length() != expected_signature.length()) {
        return Result<Json::Value>::Error("Invalid JWT signature");
    }

    int mismatch = 0;
    for (size_t i = 0; i < signature_b64.length(); ++i) {
        mismatch |= (signature_b64[i] ^ expected_signature[i]);
    }

    if (mismatch != 0) {
        return Result<Json::Value>::Error("Invalid JWT signature");
    }

    // Decode Payload
    std::string payload_json = base64UrlDecode(payload_b64);
    if (payload_json.empty()) {
        return Result<Json::Value>::Error("Failed to decode JWT payload");
    }

    // Parse Claims
    Json::Value t_claims;
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errs;

    if (!reader->parse(payload_json.data(), payload_json.data() + payload_json.length(), &t_claims, &errs)) {
        return Result<Json::Value>::Error("Failed to parse JWT payload");
    }

    return t_claims;
}

} // namespace sgrn::utils::jwt
