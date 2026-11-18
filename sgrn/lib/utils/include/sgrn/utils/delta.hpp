
//
// Reverse-delta codec over zstd "prefix" mode (same mechanism as
// `zstd --patch-from`). No new dependency: zstd is already linked.
//
//   makeDelta(base, target)  -> blob          (base = NEWER version, target = OLDER)
//   applyDelta(base, blob)   -> target bytes
//
// Inputs are LOGICAL bytes (decrypted, decompressed). Errors are returned as
// strings so this header has no dependency on BackendError and can be unit-tested
// on its own; wrap into BackendError at the call site.
#pragma once
#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <zstd.h>

#include <sgrn/Result.hpp>
#include <sgrn/utils/bytes.hpp>

namespace sgrn::utils::delta
{

constexpr const char* kDeltaAlgorithm = "zstd-prefix";

enum class DeltaStorageStatus { DeltaWasStored, DeltaWasNotStored };

// Returns true and fills t_out on success. On failure t_err explains why.
// base and target may overlap in memory; an overlapping prefix is copied
// internally so zstd can still use it as a match source.

Result<void, std::string> makeDelta(std::string_view t_base, std::string_view t_target, int t_level, std::string& t_out);

// t_expected_size is the exact logical size of the version being rebuilt
// (storage.file_versions.original_size); the result is rejected on mismatch.

Result<void, std::string> applyDelta(std::string_view t_base, std::string_view t_delta, size_t t_expected_size, std::string& t_out);

// ── Generic byte-range overloads ─────────────────────────────────────────────
// Inputs may be any contiguous range of 1-byte trivially-copyable values
// (std::string, std::string_view, std::vector<char>, std::vector<std::byte>,
// std::array<char, N>, ...). Outputs must be resizable with contiguous
// writable storage (std::string, std::vector<char>, std::vector<std::byte>, ...).
// The std::string overloads above remain the non-template core.

template <bytes::ByteRange Base, bytes::ByteRange Target, bytes::MutableByteRange Out>
Result<void, std::string> makeDelta(const Base& t_base, const Target& t_target, int t_level, Out& t_out) {
    std::string blob;
    auto result = makeDelta(bytes::view(t_base), bytes::view(t_target), t_level, blob);

    IF_ERROR_PROPAGATE(result);

    bytes::assign(t_out, blob);
    return {};
}

template <bytes::ByteRange Base, bytes::ByteRange Target>
Result<std::string> makeDelta(const Base& t_base, const Target& t_target, int t_level) {
    std::string blob;
    auto result = makeDelta(t_base, t_target, t_level, blob);

    IF_ERROR_PROPAGATE(result);

    return blob;
}

template <bytes::ByteRange Base, bytes::ByteRange Delta, bytes::MutableByteRange Out>
Result<void, std::string> applyDelta(const Base& t_base, const Delta& t_delta, size_t t_expected_size, Out& t_out) {
    std::string bytes;
    auto result = applyDelta(bytes::view(t_base), bytes::view(t_delta), t_expected_size, bytes);

    IF_ERROR_PROPAGATE(result);

    bytes::assign(t_out, bytes);
    return {};
}

template <bytes::ByteRange Base, bytes::ByteRange Delta>
Result<std::string> applyDelta(const Base& t_base, const Delta& t_delta, size_t t_expected_size) {
    std::string bytes;
    auto result = applyDelta(t_base, t_delta, t_expected_size, bytes);

    IF_ERROR_PROPAGATE(result);

    return bytes;
}

// (compression::decompressStringZstd) — it is not delta-specific.

} // namespace sgrn::utils::delta
