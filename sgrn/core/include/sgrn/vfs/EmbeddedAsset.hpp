#pragma once
// ============================================================
// sgrn::vfs — Embedded Virtual File System
// ============================================================
// This header defines the generic asset descriptor used by ALL
// auto-generated embedded file headers across the SGRN system.
//
// Any subsystem (datastore, gateway, …) that needs to bundle
// static files (web assets, SQL scripts, default configs, …)
// into the binary simply:
//   1. Runs generate_embedded_assets.py to produce compressed
//      C++ arrays + a <subsystem>_assets.hpp master header.
//   2. Includes <sgrn/vfs/EmbeddedAsset.hpp> to get the shared
//      type definitions.
//   3. Uses sgrn::vfs::Registry<N> to expose the asset table.
//
// The subsystem never touches the compression logic — that is
// fully owned by sgrn::utils::compression::decompressStringZstd.
// ============================================================
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace sgrn::vfs
{

// -------------------------------------------------------
// Asset category — what kind of payload is this entry?
// -------------------------------------------------------
enum class AssetKind : uint8_t {
    Web,    ///< HTML / JS / CSS / image — served over HTTP
    Sql,    ///< SQL migration or seed script
    Config, ///< Default configuration file (JSON / TOML / …)
    Cert,   ///< TLS certificate or key material
    Other,  ///< Any other embedded payload
};

// -------------------------------------------------------
// EmbeddedAsset — a single entry in the VFS registry
// All pointer fields point into the binary's .rodata section.
// The struct itself is constexpr-constructible so every asset
// table can live in the binary's read-only data segment with
// zero heap allocation.
// -------------------------------------------------------
struct EmbeddedAsset {
    /// Virtual path used as the lookup key.
    /// Examples:  "/index.html"  "/sql/init.sql"  "/config/default.json"
    std::string_view virtual_path;

    /// MIME type string (e.g. "text/html; charset=utf-8").
    /// May be empty for non-HTTP assets such as SQL or config files.
    std::string_view content_type;

    /// Pointer into .rodata — the Zstd-compressed payload.
    const uint8_t* compressed_data;

    /// Byte length of the compressed_data buffer.
    size_t compressed_size;

    /// Original (decompressed) size in bytes.
    /// Useful for pre-allocating the decompression output buffer.
    size_t original_size;

    /// Semantic category of this asset.
    AssetKind kind;

    // -------------------------------------------------------
    // Convenience helpers
    // -------------------------------------------------------

    /// Return a std::span over the raw compressed bytes.
    [[nodiscard]] constexpr std::span<const uint8_t> getCompressedSpan() const noexcept {
        return {compressed_data, compressed_size};
    }

    /// Return a string_view over the raw compressed bytes.
    /// Useful as input to decompressStringZstd().
    [[nodiscard]] constexpr std::string_view getCompressedView() const noexcept {
        return {reinterpret_cast<const char*>(compressed_data), compressed_size};
    }
};

// -------------------------------------------------------
// Registry<N> — compile-time sorted static map.
//
// The constexpr constructor sorts an index array over the
// assets[] reference using insertion sort (ideal for small N,
// and mandated by constexpr context in C++20/23).
//
// Because the constructor is constexpr, when the generated
// master header writes:
//
//   inline constexpr auto VFS = sgrn::vfs::Registry{ASSETS};
//
// the entire sort happens at compile time — the sorted_index
// array is placed in .rodata alongside the compressed blobs.
// find() then runs a constexpr binary search in O(log N).
//
// No heap allocation. No std::map. No runtime overhead.
// -------------------------------------------------------
template <size_t N>
struct Registry {
    const EmbeddedAsset (&assets_)[N];

    // Sorted index into assets[], ordered by virtual_path.
    // Computed entirely at compile time by the constexpr ctor.
    std::array<size_t, N> sorted_index_;

    explicit constexpr Registry(const EmbeddedAsset (&t_arr)[N]) noexcept
        : assets_(t_arr)
        , sorted_index_{} {
        // Initialise identity mapping 0 … N-1
        for (size_t i = 0; i < N; ++i) {
            sorted_index_[i] = i;
        }

        // Insertion sort — O(N²) but N is tiny (< 200 entries in practice)
        // and constexpr-friendly unlike std::sort pre-C++26.
        for (size_t i = 1; i < N; ++i) {
            const size_t key = sorted_index_[i];
            size_t j = i;
            while (j > 0 && t_arr[sorted_index_[j - 1]].virtual_path > t_arr[key].virtual_path) {
                sorted_index_[j] = sorted_index_[j - 1];
                --j;
            }
            sorted_index_[j] = key;
        }
    }

    // -------------------------------------------------------
    // find() — O(log N) binary search over sorted_index.
    // Returns a pointer directly into the .rodata asset table —
    // no copy is ever made of the compressed payload descriptor.
    // Returns nullptr if the path is not registered.
    // -------------------------------------------------------
    [[nodiscard]] constexpr const EmbeddedAsset* find(std::string_view t_path) const noexcept {
        size_t lo = 0;
        size_t hi = N;
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            const std::string_view candidate = assets_[sorted_index_[mid]].virtual_path;
            if (candidate < t_path) {
                lo = mid + 1;
            } else if (candidate > t_path) {
                hi = mid;
            } else {
                return &assets_[sorted_index_[mid]];
            }
        }
        return nullptr;
    }

    // -------------------------------------------------------
    // forEach — iterate over all assets of a given kind.
    // Order is the original declaration order (not sorted).
    // -------------------------------------------------------
    template <typename Fn>
    constexpr void forEach(AssetKind t_ind, Fn&& t_fn) const noexcept {
        for (const auto& asset : assets_) {
            if (asset.kind == t_ind) {
                t_fn(asset);
            }
        }
    }

    /// Iterate over every asset regardless of kind.
    template <typename Fn>
    constexpr void forEach(Fn&& t_fn) const noexcept {
        for (const auto& asset : assets_) {
            t_fn(asset);
        }
    }

    [[nodiscard]] constexpr size_t size() const noexcept {
        return N;
    }
};

// CTAD deduction guide: Registry{my_array} works without spelling out N.
template <size_t N>
Registry(const EmbeddedAsset (&)[N]) -> Registry<N>;

} // namespace sgrn::vfs
