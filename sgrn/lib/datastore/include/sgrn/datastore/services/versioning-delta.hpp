// include/sgrn/datastore/services/versioning-delta.hpp
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
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <zstd.h>

#include <sgrn/Result.hpp>

namespace sgrn::datastore::versioning
{

constexpr const char* kDeltaAlgorithm = "zstd-prefix";

enum class DeltaStorageStatus { DeltaWasStored, DeltaWasNotStored };

namespace detail
{
inline int windowLogFor(size_t t_bytes) {
    int wlog = 10;
    while (wlog < 30 && (size_t{1} << wlog) < t_bytes) {
        ++wlog;
    }
    return wlog;
}

// zstd's prefix mode references the base buffer instead of copying it. When
// the prefix overlaps the input, the match finder cannot exploit it, so an
// overlapping prefix gets a private copy.
inline bool rangesOverlap(std::string_view t_a, std::string_view t_b) {
    if (t_a.empty() || t_b.empty()) {
        return false;
    }
    const auto a_begin = reinterpret_cast<std::uintptr_t>(t_a.data());
    const auto b_begin = reinterpret_cast<std::uintptr_t>(t_b.data());
    return a_begin < b_begin + t_b.size() && b_begin < a_begin + t_a.size();
}
} // namespace detail

// Returns true and fills t_out on success. On failure t_err explains why.
// base and target may overlap in memory; an overlapping prefix is copied
// internally so zstd can still use it as a match source.

inline Result<void, std::string> makeDelta(std::string_view t_base, std::string_view t_target, int t_level, std::string& t_out) {

    std::string prefix_copy;
    std::string_view prefix = t_base;

    if (detail::rangesOverlap(t_base, t_target)) {
        prefix_copy.assign(t_base.data(), t_base.size());
        prefix = prefix_copy;
    }

    std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)> cctx(ZSTD_createCCtx(), &ZSTD_freeCCtx);

    RETURN_IF_NULL(cctx, "ZSTD_createCCtx failed");

    ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_compressionLevel, t_level);
    ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_windowLog, detail::windowLogFor(std::max(t_base.size(), t_target.size())));
    ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_enableLongDistanceMatching, 1);
    ZSTD_CCtx_setParameter(cctx.get(), ZSTD_c_checksumFlag, 1);
    ZSTD_CCtx_setPledgedSrcSize(cctx.get(), t_target.size());

    RETURN_ERROR_IF(const size_t rc = ZSTD_CCtx_refPrefix(cctx.get(), prefix.data(), prefix.size());
        ZSTD_isError(rc), ZSTD_getErrorName(rc));

    t_out.assign(ZSTD_compressBound(t_target.size()), '\0');

    ZSTD_inBuffer in{t_target.data(), t_target.size(), 0};
    ZSTD_outBuffer out{t_out.data(), t_out.size(), 0};

    size_t remaining = 1;

    while (remaining != 0) {
        remaining = ZSTD_compressStream2(cctx.get(), &out, &in, ZSTD_e_end);

        RETURN_ERROR_IF(ZSTD_isError(remaining), ZSTD_getErrorName(remaining));

        RETURN_ERROR_IF(remaining != 0 && out.pos == out.size, "delta output buffer exhausted");
    }
    t_out.resize(out.pos);
    return {};
}

// t_expected_size is the exact logical size of the version being rebuilt
// (storage.file_versions.original_size); the result is rejected on mismatch.

inline Result<void, std::string> applyDelta(std::string_view t_base, std::string_view t_delta, size_t t_expected_size, std::string& t_out) {
    std::string delta_copy;
    std::string_view delta = t_delta;

    if (detail::rangesOverlap(t_base, t_delta)) {
        delta_copy.assign(t_delta.data(), t_delta.size());
        delta = delta_copy;
    }

    std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> dctx(ZSTD_createDCtx(), &ZSTD_freeDCtx);

    RETURN_IF_NULL(dctx, "ZSTD_createDCtx failed");

    ZSTD_DCtx_setParameter(dctx.get(), ZSTD_d_windowLogMax, 31);

    RETURN_IF(const size_t rc = ZSTD_DCtx_refPrefix(dctx.get(), t_base.data(), t_base.size()); ZSTD_isError(rc), ZSTD_getErrorName(rc));

    t_out.assign(t_expected_size, '\0');
    ZSTD_inBuffer in{delta.data(), delta.size(), 0};
    ZSTD_outBuffer out{t_out.data(), t_out.size(), 0};
    size_t rc = 1;

    while (rc != 0) {
        const size_t in_before = in.pos;
        const size_t out_before = out.pos;
        rc = ZSTD_decompressStream(dctx.get(), &out, &in);

        RETURN_ERROR_IF(ZSTD_isError(rc), ZSTD_getErrorName(rc));

        RETURN_ERROR_IF(rc != 0 && in.pos == in_before && out.pos == out_before, "delta is truncated or larger than the expected size");
    }
    RETURN_ERROR_IF(out.pos != t_expected_size, "reconstructed size does not match the recorded size");

    return {};
}

// Plain zstd inflate lives in sgrn/utils/compression.hpp
// (compression::decompressStringZstd) — it is not delta-specific.

} // namespace sgrn::datastore::versioning
