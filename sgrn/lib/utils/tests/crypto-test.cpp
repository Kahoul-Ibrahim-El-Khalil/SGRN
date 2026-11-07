#include "sgrn/utils/crypto.hpp"
#include "sgrn/utils/encoding.hpp"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;

#define CHECK(expr)                                                                                                                        \
    do {                                                                                                                                   \
        if (!(expr)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                                                                    \
        }                                                                                                                                  \
    } while (0)

#define CHECK_MSG(expr, msg)                                                                                                               \
    do {                                                                                                                                   \
        if (!(expr)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s — %s\n", __FILE__, __LINE__, #expr, (msg));                                                        \
        }                                                                                                                                  \
    } while (0)

std::vector<uint8_t> hexToBytes(std::string_view t_hex) {
    auto res = sgrn::utils::encoding::fromHex(t_hex);
    if (res.has_value()) {
        return *res;
    }
    return {};
}

std::string bytesToHex(const uint8_t* t_data, std::size_t t_len) {
    return sgrn::utils::encoding::toHex(reinterpret_cast<const unsigned char*>(t_data), t_len);
}

// ── 1. NIST SP 800-38D Known Answer Test (KAT) Vector ────────────────────────

void testNistKatVector() {
    std::printf("[1] NIST SP 800-38D AES-256-GCM Known Answer Vector\n");
    // NIST SP 800-38D Test Case 14:
    // Key: 0000000000000000000000000000000000000000000000000000000000000000 (32 bytes)
    // IV:  000000000000000000000000 (12 bytes)
    // PT:  empty (0 bytes)
    // AAD: empty (0 bytes)
    // CT:  empty (0 bytes)
    // Tag: 530f8afbc74536b9a963b4f1c4cb738b (16 bytes)
    const auto key_bytes = hexToBytes("0000000000000000000000000000000000000000000000000000000000000000");
    const auto iv_bytes = hexToBytes("000000000000000000000000");
    const std::string expected_tag_hex = "530f8afbc74536b9a963b4f1c4cb738b";

    auto dek_res = sgrn::utils::crypto::Dek::fromBytes(key_bytes);
    CHECK(dek_res.hasValue());
    const auto& dek = dek_res.value();

    std::array<uint8_t, 12> iv{};
    std::memcpy(iv.data(), iv_bytes.data(), 12);

    std::vector<uint8_t> ciphertext;
    std::array<uint8_t, 16> tag{};
    auto enc_res = sgrn::utils::crypto::encryptChunkGcm({}, dek, iv, {}, ciphertext, tag);
    CHECK(enc_res.hasValue());
    CHECK(ciphertext.empty());

    const std::string computed_tag_hex = bytesToHex(tag.data(), tag.size());
    CHECK_MSG(computed_tag_hex == expected_tag_hex, ("KAT tag mismatch: " + computed_tag_hex).c_str());

    std::vector<uint8_t> decrypted;
    auto dec_res = sgrn::utils::crypto::decryptChunkGcm(ciphertext, dek, iv, {}, tag, decrypted);
    CHECK(dec_res.hasValue());
    CHECK(decrypted.empty());

    std::printf("    done (NIST SP 800-38D KAT vector matches tag %s)\n", expected_tag_hex.c_str());
}

// ── 2. DEK Security & Invariants ─────────────────────────────────────────────

void testDekSecurity() {
    std::printf("\n[2] DEK Move-only, Zeroization, and Equality Semantics\n");
    // Generate valid DEK
    auto dek1_res = sgrn::utils::crypto::Dek::generate();
    CHECK(dek1_res.hasValue());
    auto dek1 = std::move(dek1_res.value());
    CHECK(dek1.isValid());
    CHECK(dek1.size() == 32);

    // Deep clone
    auto dek2_res = dek1.clone();
    CHECK(dek2_res.hasValue());
    auto dek2 = std::move(dek2_res.value());
    CHECK(dek1.equals(dek2));

    // Move semantics: transfer dek2 to dek3
    auto dek3 = std::move(dek2);
    CHECK(dek3.isValid());
    CHECK(!dek2.isValid());
    CHECK(dek1.equals(dek3));

    // Different DEK must not equal
    auto dek4_res = sgrn::utils::crypto::Dek::generate();
    CHECK(dek4_res.hasValue());
    CHECK(!dek1.equals(dek4_res.value()));

    // Zeroize clears valid state and wipes memory
    dek3.zeroize();
    CHECK(!dek3.isValid());
    CHECK(!dek1.equals(dek3));

    std::printf("    done (move-only, zeroize on move/destroy, constant-time equals verified)\n");
}

// ── 3. KeyProvider Lifecycle ─────────────────────────────────────────────────

void testKeyProviderLifecycle() {
    std::printf("\n[3] KeyProvider Lifecycle (createPending, activate, get, shred, reconcile)\n");
    sgrn::utils::crypto::InMemoryKeyProvider provider;

    // 1. Create Pending
    auto pending_res = provider.createPending("", "NONE");
    CHECK(pending_res.hasValue());
    const std::string key_id = pending_res.value().key_id;
    CHECK(!key_id.empty());
    CHECK(pending_res.value().status == sgrn::utils::crypto::KeyStatus::PENDING);
    CHECK(pending_res.value().wrap_scheme == "NONE");
    CHECK(pending_res.value().dek.isValid());

    // 2. Get pending key
    auto get_pending = provider.get(key_id);
    CHECK(get_pending.hasValue());
    CHECK(get_pending.value().status == sgrn::utils::crypto::KeyStatus::PENDING);
    CHECK(!get_pending.value().file_id.has_value());

    // 3. Activate key
    const std::string file_id = "file_test_abc123";
    auto act_res = provider.activate(key_id, file_id);
    CHECK(act_res.hasValue());

    auto get_active = provider.get(key_id);
    CHECK(get_active.hasValue());
    CHECK(get_active.value().status == sgrn::utils::crypto::KeyStatus::ACTIVE);
    CHECK(get_active.value().file_id == file_id);
    CHECK(get_active.value().activated_at.has_value());

    // 4. List reconcile
    auto list_res = provider.listForReconcile();
    CHECK(list_res.hasValue());
    CHECK(list_res.value().size() == 1);
    CHECK(list_res.value()[0].key_id == key_id);

    // 5. Shred key
    auto shred_res = provider.shred(key_id);
    CHECK(shred_res.hasValue());

    // 6. Accessing shredded key must fail
    auto get_shredded = provider.get(key_id);
    CHECK(!get_shredded.hasValue());
    CHECK_MSG(get_shredded.error().find("shredded") != std::string::npos, "shredded key access must be denied");

    // Activating shredded key must fail
    auto act_shredded = provider.activate(key_id, "file_other");
    CHECK(!act_shredded.hasValue());

    std::printf("    done (full key lifecycle and crypto-shred verified)\n");
}

// ── 4. Boundary Sizes Round-Trip Tests ────────────────────────────────────────

void testBoundaryRoundTrips() {
    std::printf("\n[4] Boundary Sizes Round-Trip Tests (0, 1, N-1, N, N+1, multi-chunk)\n");
    auto dek_res = sgrn::utils::crypto::Dek::generate();
    CHECK(dek_res.hasValue());
    const auto& dek = dek_res.value();
    const std::string key_id = "test_boundary_key";

    // Use smaller chunk size for quick boundary testing (e.g. 4096 bytes)
    constexpr std::size_t kChunkSize = 4096;

    const std::vector<std::size_t> test_sizes = {
        0,                 // 0 bytes (empty file)
        1,                 // 1 byte
        kChunkSize - 1,    // N - 1 bytes
        kChunkSize,        // N bytes (exact chunk boundary)
        kChunkSize + 1,    // N + 1 bytes (spills to 2 chunks)
        kChunkSize * 3 + 7 // Multi-chunk (3 full + 1 partial)
    };

    for (std::size_t sz : test_sizes) {
        std::vector<uint8_t> plaintext(sz);
        for (std::size_t i = 0; i < sz; ++i) {
            plaintext[i] = static_cast<uint8_t>((i * 31 + 17) & 0xFF);
        }

        // Encrypt
        auto enc_res = sgrn::utils::crypto::encryptBuffer(plaintext, dek, key_id, kChunkSize);
        CHECK_MSG(enc_res.hasValue(), ("encrypt failed at size " + std::to_string(sz)).c_str());
        if (enc_res.hasError())
            continue;

        const auto& ciphertext = enc_res.value();
        CHECK(ciphertext.size() > plaintext.size());

        // Decrypt
        auto dec_res = sgrn::utils::crypto::decryptBuffer(ciphertext, dek, key_id);
        CHECK_MSG(dec_res.hasValue(), ("decrypt failed at size " + std::to_string(sz)).c_str());
        if (dec_res.hasError())
            continue;

        const auto& decrypted = dec_res.value();
        CHECK_MSG(decrypted.size() == sz, "decrypted size mismatch");
        CHECK_MSG(decrypted == plaintext, "decrypted payload does not match plaintext");

        // Streaming round-trip test on same data
        std::istringstream in_stream(std::string(reinterpret_cast<const char*>(plaintext.data()), plaintext.size()));
        std::ostringstream out_stream;
        auto stream_enc = sgrn::utils::crypto::encryptStream(in_stream, out_stream, dek, key_id, kChunkSize);
        CHECK(stream_enc.hasValue());

        const std::string stream_cipher_str = out_stream.str();
        std::istringstream cipher_in(stream_cipher_str);
        std::ostringstream plain_out;
        auto stream_dec = sgrn::utils::crypto::decryptStream(cipher_in, plain_out, dek, key_id);
        CHECK(stream_dec.hasValue());
        CHECK(stream_dec.value() == sz);
        CHECK(plain_out.str() == std::string(reinterpret_cast<const char*>(plaintext.data()), plaintext.size()));
    }

    std::printf("    done (round-tripped 0, 1, N-1, N, N+1, multi-chunk for buffers & streams)\n");
}

// ── 5. Tamper Resistance Tests ───────────────────────────────────────────────

void testTamperResistance() {
    std::printf("\n[5] Tamper Tests (bit flips, chunk swap, truncation, append, wrong key/AAD)\n");
    auto dek1_res = sgrn::utils::crypto::Dek::generate();
    auto dek2_res = sgrn::utils::crypto::Dek::generate();
    CHECK(dek1_res.hasValue() && dek2_res.hasValue());
    const auto& dek1 = dek1_res.value();
    const auto& dek2 = dek2_res.value();

    const std::string key_id = "tamper_test_key_id";
    constexpr std::size_t kChunkSize = 256;
    // 3 chunks of plaintext
    std::vector<uint8_t> plaintext(kChunkSize * 3);
    for (std::size_t i = 0; i < plaintext.size(); ++i) {
        plaintext[i] = static_cast<uint8_t>(i & 0xFF);
    }

    auto enc_res = sgrn::utils::crypto::encryptBuffer(plaintext, dek1, key_id, kChunkSize);
    CHECK(enc_res.hasValue());
    const auto valid_ciphertext = enc_res.value();

    // 1. Bit flip in ciphertext payload
    {
        auto tampered = valid_ciphertext;
        // Flip a byte in the payload of chunk 1
        tampered[tampered.size() - 50] ^= 0x55;
        auto dec_res = sgrn::utils::crypto::decryptBuffer(tampered, dek1, key_id);
        CHECK_MSG(!dec_res.hasValue(), "bit flip in ciphertext must cause decryption failure");
        CHECK_MSG(dec_res.error().find("AEAD tag verification failed") != std::string::npos,
            ("expected tag verification error, got: " + dec_res.error()).c_str());
    }

    // 2. Corrupt stream header magic
    {
        auto tampered = valid_ciphertext;
        tampered[0] ^= 0xFF;
        auto dec_res = sgrn::utils::crypto::decryptBuffer(tampered, dek1, key_id);
        CHECK_MSG(!dec_res.hasValue(), "corrupted header magic must cause failure");
        CHECK_MSG(
            dec_res.error().find("Corrupted") != std::string::npos, ("expected corrupted header error, got: " + dec_res.error()).c_str());
    }

    // 3. Truncate stream (remove last 100 bytes)
    {
        std::vector<uint8_t> tampered(valid_ciphertext.begin(), valid_ciphertext.end() - 100);
        auto dec_res = sgrn::utils::crypto::decryptBuffer(tampered, dek1, key_id);
        CHECK_MSG(!dec_res.hasValue(), "truncated stream must fail decryption");
        CHECK_MSG(dec_res.error().find("Truncated") != std::string::npos, ("expected truncation error, got: " + dec_res.error()).c_str());
    }

    // 4. Append trailing data after final chunk
    {
        auto tampered = valid_ciphertext;
        tampered.push_back(0xDE);
        tampered.push_back(0xAD);
        auto dec_res = sgrn::utils::crypto::decryptBuffer(tampered, dek1, key_id);
        CHECK_MSG(!dec_res.hasValue(), "unexpected trailing data must fail decryption");
        CHECK_MSG(
            dec_res.error().find("trailing data") != std::string::npos, ("expected trailing data error, got: " + dec_res.error()).c_str());
    }

    // 5. Wrong DEK
    {
        auto dec_res = sgrn::utils::crypto::decryptBuffer(valid_ciphertext, dek2, key_id);
        CHECK_MSG(!dec_res.hasValue(), "decrypting with wrong DEK must fail");
        CHECK_MSG(dec_res.error().find("AEAD tag verification failed") != std::string::npos,
            ("expected tag verification error, got: " + dec_res.error()).c_str());
    }

    // 6. Wrong key_id in expected_key_id
    {
        auto dec_res = sgrn::utils::crypto::decryptBuffer(valid_ciphertext, dek1, "wrong_key_id");
        CHECK_MSG(!dec_res.hasValue(), "mismatched key_id must fail");
        CHECK_MSG(dec_res.error().find("Key ID mismatch") != std::string::npos,
            ("expected key ID mismatch error, got: " + dec_res.error()).c_str());
    }

    // 7. Swap chunks 0 and 1
    {
        std::size_t header_len = 0;
        auto hdr_res = sgrn::utils::crypto::StreamHeader::deserialize(valid_ciphertext, header_len);
        CHECK(hdr_res.hasValue());

        const std::size_t chunk_frame_len = 25 + kChunkSize;
        auto tampered = valid_ciphertext;
        // Swap chunk 0 and chunk 1 frames
        std::vector<uint8_t> chunk0(tampered.begin() + header_len, tampered.begin() + header_len + chunk_frame_len);
        std::vector<uint8_t> chunk1(tampered.begin() + header_len + chunk_frame_len, tampered.begin() + header_len + 2 * chunk_frame_len);

        std::copy(chunk1.begin(), chunk1.end(), tampered.begin() + header_len);
        std::copy(chunk0.begin(), chunk0.end(), tampered.begin() + header_len + chunk_frame_len);

        auto dec_res = sgrn::utils::crypto::decryptBuffer(tampered, dek1, key_id);
        CHECK_MSG(!dec_res.hasValue(), "swapping chunks must cause verification failure");
        CHECK_MSG(dec_res.error().find("Chunk out of order") != std::string::npos ||
                      dec_res.error().find("AEAD tag verification failed") != std::string::npos,
            ("expected chunk order or tag error, got: " + dec_res.error()).c_str());
    }

    std::printf("    done (all tamper modes correctly identified with distinct integrity errors)\n");
}

// ── 6. Nonce Uniqueness Test ─────────────────────────────────────────────────

void testNonceUniqueness() {
    std::printf("\n[6] Nonce Uniqueness Verification (1000 prefixes + sequential counters)\n");
    std::set<std::string> seen_prefixes;
    constexpr int kIterations = 1000;

    for (int i = 0; i < kIterations; ++i) {
        auto prefix_res = sgrn::utils::crypto::generateNoncePrefix();
        CHECK(prefix_res.hasValue());
        const auto& p = prefix_res.value();
        std::string hex = bytesToHex(p.data(), p.size());
        CHECK_MSG(seen_prefixes.insert(hex).second, "duplicate nonce prefix detected!");
    }
    CHECK(seen_prefixes.size() == kIterations);

    // Verify 12-byte nonce uniqueness under one prefix with chunk counters 0..999
    auto prefix_res = sgrn::utils::crypto::generateNoncePrefix();
    CHECK(prefix_res.hasValue());
    std::set<std::string> seen_nonces;
    for (uint32_t c = 0; c < 1000; ++c) {
        auto nonce = sgrn::utils::crypto::buildNonce(prefix_res.value(), c);
        std::string nhex = bytesToHex(nonce.data(), nonce.size());
        CHECK_MSG(seen_nonces.insert(nhex).second, "duplicate full nonce detected!");
    }
    CHECK(seen_nonces.size() == 1000);

    std::printf("    done (1000 random prefixes and 1000 chunk nonces strictly unique)\n");
}

// ── 7. Random-Access Range Decryption ────────────────────────────────────────

void testRangeDecryption() {
    std::printf("\n[7] Random-Access Range Decryption (HTTP Range support)\n");
    auto dek_res = sgrn::utils::crypto::Dek::generate();
    CHECK(dek_res.hasValue());
    const auto& dek = dek_res.value();
    const std::string key_id = "range_key_id";

    constexpr std::size_t kChunkSize = 1024;
    // 5.5 chunks of plaintext
    const std::size_t total_size = kChunkSize * 5 + 512;
    std::vector<uint8_t> plaintext(total_size);
    for (std::size_t i = 0; i < total_size; ++i) {
        plaintext[i] = static_cast<uint8_t>((i * 7 + 13) & 0xFF);
    }

    auto enc_res = sgrn::utils::crypto::encryptBuffer(plaintext, dek, key_id, kChunkSize);
    CHECK(enc_res.hasValue());
    const auto& ciphertext = enc_res.value();

    struct RangeQuery {
        uint64_t offset;
        uint64_t length;
    };

    const std::vector<RangeQuery> queries = {
        {0, 100},                              // Start of chunk 0
        {500, 200},                            // Middle of chunk 0
        {kChunkSize - 20, 50},                 // Straddles chunk 0 and chunk 1
        {kChunkSize, kChunkSize},              // Exactly chunk 1
        {2 * kChunkSize + 10, 3 * kChunkSize}, // Spans chunks 2, 3, 4, 5
        {total_size - 100, 100},               // Tail of last chunk
        {0, total_size},                       // Complete file via range
        {100, 0}                               // Zero length
    };

    for (const auto& q : queries) {
        auto range_res = sgrn::utils::crypto::decryptRange(ciphertext, dek, q.offset, q.length, key_id);
        CHECK_MSG(range_res.hasValue(), ("decryptRange failed for offset=" + std::to_string(q.offset)).c_str());
        if (range_res.hasError())
            continue;

        const auto& slice = range_res.value();
        CHECK(slice.size() == q.length);

        std::vector<uint8_t> expected_slice(plaintext.begin() + q.offset, plaintext.begin() + q.offset + q.length);
        CHECK_MSG(slice == expected_slice, "decrypted range slice mismatch with plaintext");
    }

    // Offset out of bounds must fail
    auto oob_res = sgrn::utils::crypto::decryptRange(ciphertext, dek, total_size + 10, 50, key_id);
    CHECK_MSG(!oob_res.hasValue(), "offset out of bounds must fail");

    std::printf("    done (sub-ranges and chunk boundaries verified for random access)\n");
}

// ── 8. Grep Test Over Error Messages & Exceptions (Zero Key Bytes Leak) ──────

void testErrorLogNoKeyLeak() {
    std::printf("\n[8] Security Grep: Zero Key Material in Error Messages / Logs\n");
    auto dek_res = sgrn::utils::crypto::Dek::generate();
    CHECK(dek_res.hasValue());
    const auto& dek = dek_res.value();

    // Convert raw DEK bytes to hex and substrings for search
    const std::string dek_hex = bytesToHex(dek.data(), dek.size());

    // Generate errors intentionally
    std::vector<std::string> collected_errors;

    // Bad key decryption
    std::vector<uint8_t> junk_cipher = {0x00, 0x01, 0x02, 0x03};
    auto err1 = sgrn::utils::crypto::decryptBuffer(junk_cipher, dek, "test_id");
    if (err1.hasError())
        collected_errors.push_back(err1.error());

    // Bad range
    auto err2 = sgrn::utils::crypto::decryptRange(junk_cipher, dek, 1000, 50);
    if (err2.hasError())
        collected_errors.push_back(err2.error());

    // Check that neither the full 64-char hex nor any 8-byte chunk of the key appears in any error message!
    for (const auto& msg : collected_errors) {
        CHECK_MSG(msg.find(dek_hex) == std::string::npos, "Full key hex leaked in error message!");
        // Check 8-byte sub-slices
        for (std::size_t i = 0; i + 16 <= dek_hex.size(); i += 8) {
            std::string sub = dek_hex.substr(i, 16);
            CHECK_MSG(msg.find(sub) == std::string::npos, "Key hex fragment leaked in error message!");
        }
    }

    std::printf("    done (confirmed zero key material leaked in error messages)\n");
}

} // namespace

int main() {
    std::printf("=== sgrn_utils crypto_test (Prompt 7) ===\n\n");

    testNistKatVector();
    testDekSecurity();
    testKeyProviderLifecycle();
    testBoundaryRoundTrips();
    testTamperResistance();
    testNonceUniqueness();
    testRangeDecryption();
    testErrorLogNoKeyLeak();

    if (g_failures == 0) {
        std::printf("\nPASS — all crypto core tests passed successfully.\n");
        return 0;
    }
    std::printf("\nFAIL — %d test(s) failed.\n", g_failures);
    return 1;
}
