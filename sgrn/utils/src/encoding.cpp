#include <sgrn/debug.hpp>
#include <sgrn/utils/encoding.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace sgrn::utils::encoding
{

// ─── Internal helpers ────────────────────────────────────────────────────────

namespace
{

// Encodes bytes using the given alphabet table.
// If t_omit_padding is true, no '=' characters are appended (URL‑safe style).
std::string encodeBase64Impl(const unsigned char* tp_data, std::size_t t_len, const char* t_table, bool t_omit_padding) {
    std::string out;
    out.reserve((t_len + 2) / 3 * 4);

    for (std::size_t i = 0; i < t_len; i += 3) {
        unsigned char c1 = tp_data[i];
        unsigned char c2 = (i + 1 < t_len) ? tp_data[i + 1] : 0;
        unsigned char c3 = (i + 2 < t_len) ? tp_data[i + 2] : 0;

        out.push_back(t_table[c1 >> 2]);
        out.push_back(t_table[((c1 & 0x03) << 4) | (c2 >> 4)]);

        if (i + 1 < t_len) {
            out.push_back(t_table[((c2 & 0x0F) << 2) | (c3 >> 6)]);
        } else if (!t_omit_padding) {
            out.push_back('=');
        }

        if (i + 2 < t_len) {
            out.push_back(t_table[c3 & 0x3F]);
        } else if (!t_omit_padding) {
            out.push_back('=');
        }
    }

    return out;
}

// Look‑up table for Base64 decoding.
// Values: -1 = invalid, -2 = whitespace (skip), 0‑63 = valid data.
static const int8_t base64DecodeLUT[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -2, -2, -2, -2, -2, -1, -1, //  0‑15
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // 16‑31
    -2, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, 62, -1, 63, // 32‑47  '+' '-'
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1, // 48‑63  '0'‑'9'
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,           // 64‑79  'A'‑'O'
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, 63, // 80‑95  'P'‑'Z' '_'
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, // 96‑111 'a'‑'o'
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1  // 112‑127 'p'‑'z'
};

// Decodes a single 4‑character Base64 chunk (including padding).
// Returns the number of decoded bytes (0‑3) and writes them to out.
std::size_t decodeBase64Chunk(const char* t_chunk, unsigned char* out) {
    int values[4];
    int valid = 0;

    for (int i = 0; i < 4; ++i) {
        unsigned char c = t_chunk[i];
        if (c >= 128)
            return 0;
        int8_t v = base64DecodeLUT[c];
        if (v == -2) { // whitespace inside chunk – should not happen, skip
            --i;
            continue;
        }
        if (v < 0) { // padding or invalid – stop after this
            break;
        }
        values[valid++] = v;
    }

    // No valid characters → nothing decoded
    if (valid == 0)
        return 0;

    // Combine bits: values are 6‑bit, we need 8‑bit bytes
    int accum =
        (values[0] << 18) | ((valid > 1) ? (values[1] << 12) : 0) | ((valid > 2) ? (values[2] << 6) : 0) | ((valid > 3) ? values[3] : 0);

    int out_idx = 0;
    if (valid > 1)
        out[out_idx++] = (accum >> 16) & 0xFF;
    if (valid > 2)
        out[out_idx++] = (accum >> 8) & 0xFF;
    if (valid > 3)
        out[out_idx++] = accum & 0xFF;
    return out_idx;
}

} // anonymous namespace

// ─── Public API ──────────────────────────────────────────────────────────────

std::string toBase64(const unsigned char* tp_data, std::size_t t_len) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    return encodeBase64Impl(tp_data, t_len, table, false);
}

std::string toBase64Url(const unsigned char* tp_data, std::size_t t_len) {
    static constexpr char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    return encodeBase64Impl(tp_data, t_len, table, true);
}

std::vector<uint8_t> fromBase64(const std::string& t_input) {
    std::vector<uint8_t> out;
    out.reserve((t_input.size() * 3) / 4 + 2);

    // Process the input in chunks of 4 characters
    std::size_t pos = 0;
    unsigned char chunk[4] = {};
    while (pos < t_input.size()) {
        // Collect up to 4 non‑whitespace characters
        int count = 0;
        while (count < 4 && pos < t_input.size()) {
            unsigned char c = t_input[pos++];
            if (c >= 128)
                continue;
            int8_t v = base64DecodeLUT[c];
            if (v == -2)
                continue; // whitespace – skip
            // Stop on '=' or invalid (decoding will handle it)
            if (v < 0)
                break;
            chunk[count++] = c;
        }
        if (count == 0)
            break;

        // Decode the chunk
        unsigned char decoded[3];
        std::size_t n = decodeBase64Chunk(reinterpret_cast<const char*>(chunk), decoded);
        out.insert(out.end(), decoded, decoded + n);

        // If we hit padding or end, we're done
        if (count < 4)
            break;
    }

    return out;
}

std::string toHex(const unsigned char* tp_data, std::size_t t_len) {
    static constexpr char lut[] = "0123456789abcdef";

    std::string out;
    out.resize(t_len * 2);

    for (std::size_t i = 0; i < t_len; ++i) {
        out[2 * i] = lut[tp_data[i] >> 4];
        out[2 * i + 1] = lut[tp_data[i] & 0x0F];
    }

    return out;
}

int bcdToDec(uint8_t t_bcd) {
    return ((t_bcd >> 4) * 10) + (t_bcd & 0x0F);
}

uint8_t decToBcd(int t_dec) {
    return static_cast<uint8_t>(((t_dec / 10) << 4) | (t_dec % 10));
}

} // namespace sgrn::utils::encoding
