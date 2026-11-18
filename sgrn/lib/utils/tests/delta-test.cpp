// Unit tests for sgrn::utils::delta — the reverse-delta codec built on zstd
// prefix mode (makeDelta / applyDelta). Plain zstd inflate lives in
// sgrn/utils/compression.hpp and is covered by compression-test.cpp.
//
// Contract: makeDelta(base, target) takes `base` as the NEWER version and
// `target` as the OLDER version, so
//     applyDelta(base, delta, target.size()) == target
// must always hold for logical (decrypted, decompressed) bytes.
//
// The test also prints concrete delta sizes/ratios ("EXAMPLE ...") so the
// behaviour is visible when running the binary directly:
//
//     .build/linux-static-release/sgrn/lib/utils/delta_test
//
// Run with: delta_test  (exit 0 = pass, non-zero = fail)
#include <sgrn/utils/delta.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace delta = sgrn::utils::delta;

// ── Helpers ───────────────────────────────────────────────────────────────────

static void check(bool t_cond, std::string_view t_msg) {
    if (!t_cond) {
        std::cerr << "FAIL: " << t_msg << '\n';
        std::exit(1);
    }
}

// Deterministic, line-oriented payload that resembles a sensor log. Used for
// the larger cases so prefix matching has realistic redundancy to work with.
static std::string makeSensorLog(size_t t_lines) {
    std::string out;
    out.reserve(t_lines * 48);
    for (size_t i = 0; i < t_lines; ++i) {
        out += "line ";
        out += std::to_string(i);
        out += " sensor=temp value=";
        out += std::to_string((i * 37) % 1000);
        out += " status=ok\n";
    }
    return out;
}

// Deterministic pseudo-random bytes: effectively incompressible, so a delta
// against unrelated content must not pretend to be smaller than the target.
static std::string makeNoise(size_t t_size, uint64_t t_seed) {
    std::string out;
    out.reserve(t_size);
    uint64_t x = t_seed | 1;
    while (out.size() < t_size) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        out.push_back(static_cast<char>(x & 0xFF));
    }
    return out;
}

static std::string hexPreview(std::string_view t_bytes, size_t t_limit) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    const size_t n = std::min(t_limit, t_bytes.size());
    out.reserve(n * 3 + 4);
    for (size_t i = 0; i < n; ++i) {
        if (i != 0) {
            out += ' ';
        }
        const auto byte = static_cast<unsigned char>(t_bytes[i]);
        out += kHex[byte >> 4];
        out += kHex[byte & 0x0F];
    }
    if (t_bytes.size() > n) {
        out += " ...";
    }
    return out;
}

static void printExample(std::string_view t_name, std::string_view t_base, std::string_view t_target, std::string_view t_delta) {
    std::ostringstream line;
    line << "EXAMPLE " << t_name << '\n'
         << "  base(newer)=" << t_base.size() << " B, target(older)=" << t_target.size() << " B, delta=" << t_delta.size() << " B";
    if (!t_target.empty()) {
        const double pct = 100.0 * static_cast<double>(t_delta.size()) / static_cast<double>(t_target.size());
        line << " (" << std::fixed << std::setprecision(2) << pct << "% of target";
        if (!t_delta.empty()) {
            line << ", " << std::setprecision(1) << static_cast<double>(t_target.size()) / static_cast<double>(t_delta.size())
                 << "x smaller";
        }
        line << ")";
    }
    line << "\n  first bytes: " << hexPreview(t_delta, 32);
    std::cout << line.str() << '\n';
}

// makeDelta + full round-trip check. Every positive test funnels through here
// so no case can accidentally verify only one direction.
static std::string makeAndVerify(std::string_view t_base, std::string_view t_target, int t_level) {
    std::string blob;
    auto made = delta::makeDelta(t_base, t_target, t_level, blob);
    check(made.has_value(), "makeDelta should succeed");
    check(!blob.empty(), "delta must not be empty");

    // A delta is a normal zstd frame; keep that observable contract honest.
    check(blob.size() >= 4 && static_cast<unsigned char>(blob[0]) == 0x28 && static_cast<unsigned char>(blob[1]) == 0xB5 &&
              static_cast<unsigned char>(blob[2]) == 0x2F && static_cast<unsigned char>(blob[3]) == 0xFD,
        "delta must be a zstd frame");

    std::string restored;
    auto applied = delta::applyDelta(t_base, blob, t_target.size(), restored);
    check(applied.has_value(), "applyDelta should succeed");
    check(restored == t_target, "round-trip must be byte-exact");
    check(restored.size() == t_target.size(), "round-trip must preserve size");
    return blob;
}

// ── Tests ─────────────────────────────────────────────────────────────────────

// The original hand-written scenario: the newer version is the base, the
// older (shorter) version is rebuilt from the delta.
static void testHelloWorldRoundTrip() {
    const std::string original_message = "Hello World!";
    const std::string second_version = "Hello World! Hello World!";

    const std::string blob = makeAndVerify(second_version, original_message, 19);
    printExample("hello-world", second_version, original_message, blob);

    // The codec is deterministic: applying the same blob again yields the same
    // bytes, and the caller's output buffer is fully overwritten each time.
    std::string restored_again = "stale contents that must be replaced, longer than the target";
    auto applied_again = delta::applyDelta(second_version, blob, original_message.size(), restored_again);
    check(applied_again.has_value(), "applyDelta should be repeatable");
    check(restored_again == original_message, "repeat apply must produce identical output");

    std::puts("PASS testHelloWorldRoundTrip");
}

// Logical content is opaque bytes: embedded NULs and high-bit values must
// survive unchanged.
static void testBinaryRoundTrip() {
    std::string base;
    base.reserve(64 * 1024);
    for (size_t i = 0; i < 64 * 1024; ++i) {
        base.push_back(static_cast<char>((i * 31 + 7) % 256));
    }

    std::string target = base;
    for (size_t i = 0; i < target.size(); i += 4093) {
        target[i] = static_cast<char>(target[i] ^ 0x5A);
    }
    target.resize(target.size() - 100); // also exercise a shorter older version
    target += std::string("\0binary\0tail", 12);

    const std::string blob = makeAndVerify(base, target, 3);
    printExample("binary-with-nuls", base, target, blob);
    check(blob.size() < target.size(), "delta of similar binaries should be smaller");

    std::puts("PASS testBinaryRoundTrip");
}

// A version that did not change still round-trips, and its delta should be
// tiny because the whole target is a prefix match. Production loads the two
// versions into separate buffers, so use two distinct strings here.
static void testIdenticalVersions() {
    const std::string content = makeSensorLog(500);
    const std::string same_content = makeSensorLog(500);

    const std::string blob = makeAndVerify(content, same_content, 19);
    printExample("identical-versions", content, same_content, blob);
    check(blob.size() < content.size() / 100, "identical-version delta should be <1% of the content");

    std::puts("PASS testIdenticalVersions");
}

// zstd prefix mode references the base buffer rather than copying it. When
// base and target overlap in memory, the match finder cannot exploit the
// prefix, so makeDelta copies the prefix internally. The ratio must match the
// independent-buffer case, not merely stay correct.
static void testAliasedBuffersStayCorrect() {
    const std::string content = makeSensorLog(500);

    {
        // base and target are the very same object.
        const std::string blob = makeAndVerify(content, content, 3);
        printExample("aliased-same-object", content, content, blob);
        check(blob.size() < content.size() / 100, "same-object delta must stay <1% of the content");
    }
    {
        // base is a view into the target's buffer (append-in-place pattern).
        std::string shared = content;
        shared += "appended only in the target\n";
        const std::string_view base_view(shared.data(), content.size());

        const std::string blob = makeAndVerify(base_view, shared, 3);
        printExample("aliased-prefix-view", base_view, shared, blob);
        check(blob.size() < shared.size() / 100, "overlapping-view delta must stay <1% of the content");
    }

    std::puts("PASS testAliasedBuffersStayCorrect");
}

// Empty base, empty target, and both empty are legal storage states (e.g. a
// file created empty, or a first version with no older content).
static void testEmptyInputs() {
    {
        const std::string base = "non-empty newer version";
        std::string blob;
        auto made = delta::makeDelta(base, std::string_view{}, 3, blob);
        check(made.has_value(), "makeDelta to an empty target should succeed");

        std::string restored = "must be cleared";
        auto applied = delta::applyDelta(base, blob, 0, restored);
        check(applied.has_value(), "applyDelta to zero expected size should succeed");
        check(restored.empty(), "empty target must rebuild as empty");
    }
    {
        const std::string target = "non-empty older version";
        const std::string blob = makeAndVerify(std::string_view{}, target, 3);
        printExample("empty-base", std::string_view{}, target, blob);
    }
    {
        std::string blob;
        auto made = delta::makeDelta(std::string_view{}, std::string_view{}, 3, blob);
        check(made.has_value(), "makeDelta of two empty versions should succeed");

        std::string restored = "must be cleared";
        auto applied = delta::applyDelta(std::string_view{}, blob, 0, restored);
        check(applied.has_value(), "applyDelta of two empty versions should succeed");
        check(restored.empty(), "empty round-trip must stay empty");
    }

    std::puts("PASS testEmptyInputs");
}

// The common versioning pattern: the older version is a prefix of the newer
// one (append-only log / growing report). The delta should be near-constant.
static void testAppendOnly() {
    const std::string older = makeSensorLog(4000);
    const std::string newer = older + makeSensorLog(500) + "appended trailer\n";

    const std::string blob = makeAndVerify(newer, older, 3);
    printExample("append-only", newer, older, blob);
    check(blob.size() * 100 < older.size(), "append-only delta should be <1% of the older version");

    std::puts("PASS testAppendOnly");
}

// Edits that shift content: this is where long-distance matching earns its
// keep. In each case the bytes of `older` move to a different offset in the
// newer version, so naive "same offset" diffing would fail.
static void testInsertionAndDeletionShift() {
    const std::string older = makeSensorLog(3000);
    const std::string prefix = "preamble inserted only in the newer version\n";
    const std::string middle = "middle block inserted only in the newer version\n";

    {
        // Insertion at the front of the newer version.
        const std::string newer = prefix + older;
        const std::string blob = makeAndVerify(newer, older, 3);
        printExample("insert-at-front", newer, older, blob);
        check(blob.size() * 20 < older.size(), "front-insertion delta should be <5% of the older version");
    }
    {
        // Insertion in the middle of the newer version.
        std::string newer = older;
        newer.insert(newer.size() / 2, middle);
        const std::string blob = makeAndVerify(newer, older, 3);
        printExample("insert-in-middle", newer, older, blob);
        check(blob.size() * 20 < older.size(), "middle-insertion delta should be <5% of the older version");
    }
    {
        // Deletion from the newer version: the older version is LONGER, and
        // matching bytes sit at a larger offset in the base.
        std::string newer = older;
        newer.erase(newer.size() / 3, 4096);
        const std::string blob = makeAndVerify(newer, older, 3);
        printExample("delete-in-middle", newer, older, blob);
        check(blob.size() * 20 < older.size(), "deletion delta should be <5% of the older version");
    }

    std::puts("PASS testInsertionAndDeletionShift");
}

// The compressor sizes its window from max(base, target) and enables long
// distance matching. Exercise sizes on both sides of powers of two (where the
// window log jumps) and force a reference from the very end of the target to
// the very beginning of the base.
static void testWindowBoundarySizes() {
    std::cout << "EXAMPLE window-boundary sizes (same-size base/target, edit at front/middle/end):\n";
    for (const size_t n : {size_t{1023}, size_t{1024}, size_t{1025}, size_t{65535}, size_t{65536}, size_t{65537}}) {
        std::string base(n, 'a');
        base += "TAIL-MARKER";
        std::string target = base;
        target.front() = 'Z';
        target[target.size() / 2] = static_cast<char>(target[target.size() / 2] ^ 0x01);
        target.back() = '!';

        const std::string blob = makeAndVerify(base, target, 3);
        std::cout << "  n=" << std::setw(5) << n << " B -> delta=" << std::setw(6) << blob.size() << " B\n";
        check(blob.size() < target.size(), "similar same-size content should delta smaller");
    }

    std::puts("PASS testWindowBoundarySizes");
}

// Unrelated or incompressible content must not produce a "delta" smaller than
// the target by accident: this is the compactor's keep-the-full-copy case.
static void testUnrelatedContentIsNotAFreeLunch() {
    const std::string base = makeSensorLog(1500); // ~64 KiB of text
    const std::string target = makeNoise(64 * 1024, 0xC0FFEE);

    const std::string blob = makeAndVerify(base, target, 3);
    printExample("unrelated-noise", base, target, blob);
    check(blob.size() <= target.size() + 1024, "delta of unrelated content must not beat the target by much");

    std::puts("PASS testUnrelatedContentIsNotAFreeLunch");
}

// The recorded original_size is a hard contract: a smaller buffer cannot hold
// the version, and a larger expected size must be rejected rather than padded.
static void testSizeMismatchIsRejected() {
    const std::string base = makeSensorLog(200);
    const std::string target = makeSensorLog(190);

    const std::string blob = makeAndVerify(base, target, 3);

    {
        std::string restored;
        auto applied = delta::applyDelta(base, blob, target.size() - 1, restored);
        check(applied.hasError(), "too-small expected size must be rejected");
        check(!applied.error().empty(), "size error must carry a message");
    }
    {
        std::string restored;
        auto applied = delta::applyDelta(base, blob, target.size() + 16, restored);
        check(applied.hasError(), "too-large expected size must be rejected");
        check(!applied.error().empty(), "size error must carry a message");
    }

    // The correct size still works after the failed attempts.
    std::string restored;
    auto applied = delta::applyDelta(base, blob, target.size(), restored);
    check(applied.has_value(), "correct expected size must still succeed");
    check(restored == target, "correct-size rebuild must be exact");

    std::puts("PASS testSizeMismatchIsRejected");
}

// A delta bound to one base must not be silently applied to a different one.
// makeDelta enables the zstd content checksum, so a wrong base fails loudly
// instead of returning plausible-looking corruption.
static void testWrongBaseIsRejected() {
    const std::string base_a = makeSensorLog(400);
    const std::string target_a = makeSensorLog(380);

    const std::string blob = makeAndVerify(base_a, target_a, 3);

    std::string wrong_base = base_a;
    for (size_t i = 0; i < wrong_base.size(); i += 17) {
        wrong_base[i] = static_cast<char>(wrong_base[i] ^ 0x3C);
    }

    std::string restored;
    auto applied = delta::applyDelta(wrong_base, blob, target_a.size(), restored);
    check(applied.hasError(), "delta applied against the wrong base must be rejected");
    check(!applied.error().empty(), "wrong-base error must carry a message");

    std::puts("PASS testWrongBaseIsRejected");
}

// Corrupt or truncated deltas must be reported, never partially accepted.
static void testCorruptDeltaIsRejected() {
    const std::string base = makeSensorLog(300);
    const std::string target = makeSensorLog(280);

    const std::string blob = makeAndVerify(base, target, 3);

    {
        std::string corrupted = blob;
        corrupted[0] = static_cast<char>(corrupted[0] ^ 0xFF); // destroy the frame magic
        std::string restored;
        auto applied = delta::applyDelta(base, corrupted, target.size(), restored);
        check(applied.hasError(), "corrupted frame header must be rejected");
    }
    {
        std::string truncated = blob.substr(0, blob.size() / 2);
        std::string restored;
        auto applied = delta::applyDelta(base, truncated, target.size(), restored);
        check(applied.hasError(), "truncated delta must be rejected");
    }
    {
        std::string restored;
        auto applied = delta::applyDelta(base, std::string_view{}, target.size(), restored);
        check(applied.hasError(), "empty delta must be rejected");
    }

    std::puts("PASS testCorruptDeltaIsRejected");
}

// Reverse deltas exist to keep old versions cheap: a slightly edited large
// version must encode to a small fraction of the logical size at every
// supported compression level. Also prints sizes and wall-clock costs.
static void testLargeVersionCompressesWell() {
    const std::string base = makeSensorLog(12000); // ~0.5 MiB
    std::string target = base;

    // Sprinkle small edits through the older version.
    for (size_t pos = 4096; pos < target.size(); pos += 32768) {
        target[pos] = 'X';
    }
    target += "one extra trailing line that is not in the newer version\n";

    std::cout << "EXAMPLE large-version levels (base=" << base.size() << " B, target=" << target.size() << " B):\n";
    for (const int level : {1, 3, 19}) {
        const auto before = std::chrono::steady_clock::now();
        const std::string blob = makeAndVerify(base, target, level);
        const auto after = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(after - before).count();
        const double pct = 100.0 * static_cast<double>(blob.size()) / static_cast<double>(target.size());
        std::cout << "  level " << std::setw(2) << level << ": delta=" << std::setw(7) << blob.size() << " B (" << std::fixed
                  << std::setprecision(2) << pct << "% of target), make+apply=" << std::setprecision(1) << ms << " ms\n";
        check(blob.size() * 10 < base.size(), "large similar version should encode to <10% of its size");
    }

    std::puts("PASS testLargeVersionCompressesWell");
}

// Simulates the storage use-case: the head version is full, while every older
// version is stored as a delta against the next newer one. Reconstructing an
// old version means replaying the chain from the head down.
static void testVersionChain() {
    std::vector<std::string> versions;
    versions.push_back(makeSensorLog(800));
    for (size_t i = 1; i < 6; ++i) {
        std::string next = versions.back();
        next.insert(next.size() / 2, "revision " + std::to_string(i) + " notes\n");
        next += "line appended in revision " + std::to_string(i) + "\n";
        versions.push_back(std::move(next));
    }

    // d[i] = delta(newer=versions[i+1], older=versions[i]).
    std::vector<std::string> deltas(versions.size() - 1);
    for (size_t i = 0; i + 1 < versions.size(); ++i) {
        deltas[i] = makeAndVerify(versions[i + 1], versions[i], 3);
    }

    // Head is still stored full; walk backwards applying one delta per step.
    std::string current = versions.back();
    std::cout << "EXAMPLE version-chain (6 versions, head full, 5 reverse deltas):\n";
    for (size_t step = deltas.size(); step-- > 0;) {
        std::string older;
        auto applied = delta::applyDelta(current, deltas[step], versions[step].size(), older);
        check(applied.has_value(), "chain step must apply");
        check(older == versions[step], "chain step must reproduce the exact older version");
        current = std::move(older);
        const double pct = 100.0 * static_cast<double>(deltas[step].size()) / static_cast<double>(versions[step].size());
        std::cout << "  v" << step << " <- v" << (step + 1) << ": delta=" << std::setw(6) << deltas[step].size() << " B (" << std::fixed
                  << std::setprecision(2) << pct << "% of v" << step << ")\n";
    }
    check(current == versions.front(), "the full chain must rebuild the oldest version");

    std::puts("PASS testVersionChain");
}

// The generic overloads accept any contiguous 1-byte range for inputs and any
// resizable byte container for outputs, not just std::string.
static void testGenericByteContainers() {
    const std::string base = makeSensorLog(400);
    const std::string target = makeSensorLog(380);

    // vector<char> inputs, vector<unsigned char> output, out-parameter API.
    const std::vector<char> base_chars(base.begin(), base.end());
    const std::vector<unsigned char> target_uchars(target.begin(), target.end());

    std::vector<unsigned char> blob;
    auto made = delta::makeDelta(base_chars, target_uchars, 3, blob);
    check(made.has_value(), "generic makeDelta should succeed");

    std::vector<unsigned char> restored;
    auto applied = delta::applyDelta(base_chars, blob, target.size(), restored);
    check(applied.has_value(), "generic applyDelta should succeed");
    check(restored.size() == target.size(), "generic output should have the recorded size");
    check(std::equal(restored.begin(), restored.end(), target.begin(), target.end(),
              [](unsigned char t_lhs, char t_rhs) { return t_lhs == static_cast<unsigned char>(t_rhs); }),
        "generic round-trip must be byte-exact");

    // std::vector<std::byte> with the return-value overloads.
    std::vector<std::byte> base_raw(base.size());
    std::memcpy(base_raw.data(), base.data(), base.size());
    std::vector<std::byte> target_raw(target.size());
    std::memcpy(target_raw.data(), target.data(), target.size());

    auto blob_result = delta::makeDelta(base_raw, target_raw, 3);
    check(blob_result.has_value(), "generic return-value makeDelta should succeed");

    auto restored_result = delta::applyDelta(base_raw, blob_result.value(), target.size());
    check(restored_result.has_value(), "generic return-value applyDelta should succeed");
    check(restored_result->size() == target.size(), "generic return-value size should match");
    check(std::memcmp(restored_result->data(), target.data(), target.size()) == 0, "generic return-value round-trip must be byte-exact");

    // A fixed-size std::array is a valid input range.
    std::array<char, 5> array_base{'h', 'e', 'l', 'l', 'o'};
    std::array<char, 4> array_target{'w', 'o', 'r', 'l'};
    auto array_blob = delta::makeDelta(array_base, array_target, 3);
    check(array_blob.has_value(), "std::array input should be accepted");
    auto array_restored = delta::applyDelta(array_base, array_blob.value(), array_target.size());
    check(array_restored.has_value(), "std::array round-trip should succeed");
    check(array_restored.value() == std::string(array_target.begin(), array_target.end()), "std::array round-trip must match");

    std::puts("PASS testGenericByteContainers");
}

int main() {
    testHelloWorldRoundTrip();
    testBinaryRoundTrip();
    testIdenticalVersions();
    testAliasedBuffersStayCorrect();
    testEmptyInputs();
    testAppendOnly();
    testInsertionAndDeletionShift();
    testWindowBoundarySizes();
    testUnrelatedContentIsNotAFreeLunch();
    testSizeMismatchIsRejected();
    testWrongBaseIsRejected();
    testCorruptDeltaIsRejected();
    testLargeVersionCompressesWell();
    testVersionChain();
    testGenericByteContainers();

    std::puts("PASS delta_test");
    return 0;
}
