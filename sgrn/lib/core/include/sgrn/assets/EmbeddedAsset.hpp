#pragma once
// ============================================================
// sgrn — Embedded Asset System
// ============================================================
// Defines the shared types used by every auto-generated asset
// bundle across the SGRN system (datastore, gateway, …).
//
// The types live directly in namespace sgrn so they are
// available to any SGRN component without a deep qualifier.
//
// Generated asset tables follow the convention:
//   sgrn::{project}::assets::{kind}
//
// Examples:
//   sgrn::datastore::assets::web   — React SPA assets
//   sgrn::datastore::assets::sql   — SQL migration scripts
//   sgrn::datastore::assets::config — Default config files
//   sgrn::gateway::assets::web     — Gateway web UI assets
//
// Usage in consuming code:
//
//   #include <sgrn/assets/EmbeddedAsset.hpp>
//   #include <datastore_sql_assets.hpp>          // generated
//
//   auto* asset = sgrn::datastore::assets::sql::VFS.find("/sql/init.sql");
//   if (asset) {
//       auto sql = sgrn::utils::compression::decompressStringZstd(
//           asset->compressedView());
//   }
//
// ============================================================
// ARCHITECTURE BENEFITS IN EMBEDDED / INDUSTRIAL PROGRAMMING
// ============================================================
// 1. True Zero-Heap Initialization: Dynamic allocation during
//    startup causes heap fragmentation. Here, the entire VFS
//    metadata and search index is evaluated via `constexpr` at
//    compile-time and mapped directly into the `.rodata` segment.
//
// 2. Deterministic Access: No SD card read errors or IO locks.
//    - O(1) retrieval when routes are registered by array index `i`.
//    - O(log N) guaranteed bound for dynamic lookups via `find()`.
//
// 3. Immunity to FS Corruption: Sudden power loss often corrupts
//    flash memory. By baking HTML/JS/SQL/JSON configs into the
//    compiled executable, the assets are completely immutable.
//
// 4. Zero-Dependency Deployments: Eliminates the need to ship a
//    `var/www` or `etc/config` folder alongside the binary.
//    Results in a completely self-contained industrial gateway.
// ============================================================
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>

namespace sgrn
{

// -------------------------------------------------------
// AssetKind — semantic category of an embedded payload.
// -------------------------------------------------------
enum class AssetKind : uint8_t {
    Web,    ///< HTML / JS / CSS / image — served over HTTP
    Sql,    ///< SQL migration or seed script
    Config, ///< Default configuration file (JSON / TOML / …)
    Cert,   ///< TLS certificate or key material
    Other,  ///< Any other embedded payload
};

// -------------------------------------------------------
// EmbeddedAsset — a single entry in an asset registry.
//
// All pointer fields reference the binary's .rodata section.
// The struct is constexpr-constructible so every asset table
// lives in .rodata with zero heap allocation.
// -------------------------------------------------------
struct EmbeddedAsset {
    /// Virtual path — the lookup key.
    /// Examples:  "/index.html"  "/sql/init.sql"  "/config/default.json"
    std::string_view virtual_path;

    /// MIME type string (e.g. "text/html; charset=utf-8").
    /// Empty for non-HTTP assets such as SQL or config files.
    std::string_view content_type;

    /// Pointer into .rodata — the Zstd-compressed payload.
    const uint8_t* compressed_data;

    /// Byte length of the compressed_data buffer.
    size_t compressed_size;

    /// Original (decompressed) byte count.
    /// Used to pre-allocate the decompression output buffer.
    size_t original_size;

    /// Semantic category.
    AssetKind kind;

    // -------------------------------------------------------
    // Convenience accessors — no allocation, no copy.
    // -------------------------------------------------------

    /// std::span over the raw compressed bytes.
    [[nodiscard]] constexpr std::span<const uint8_t> compressedSpan() const noexcept {
        return {compressed_data, compressed_size};
    }

    /// std::string_view over the raw compressed bytes —
    /// the natural input type for decompressStringZstd().
    [[nodiscard]] constexpr std::string_view compressedView() const noexcept {
        return {reinterpret_cast<const char*>(compressed_data), compressed_size};
    }
};

// -------------------------------------------------------
// AssetRegistry<N> — compile-time sorted static map.
//
// N is the number of assets, known at compile time from the
// generated constexpr array.  The constructor builds a sorted
// index via insertion sort (constexpr-friendly, O(N²), fine
// for N < 200).  Because the object is declared
//
//   inline constexpr auto VFS = sgrn::AssetRegistry{ASSETS};
//
// the sort runs at compile time and the sorted_index array is
// placed in .rodata alongside the compressed blobs.
//
// find() performs O(log N) binary search entirely over
// .rodata — no heap allocation at any point.
// -------------------------------------------------------
template <size_t N>
struct AssetRegistry {
    /// Reference into .rodata — not a copy.
    const EmbeddedAsset (&assets_)[N];

    /// Sorted index into assets[], ordered by virtual_path.
    /// Computed entirely at compile time in the constexpr ctor.
    std::array<size_t, N> sorted_index_;

    explicit constexpr AssetRegistry(const EmbeddedAsset (&t_arr)[N]) noexcept
        : assets_(t_arr)
        , sorted_index_{} {
        // Identity mapping 0 … N-1
        for (size_t i = 0; i < N; ++i) {
            sorted_index_[i] = i;
        }

        // Insertion sort — constexpr-friendly, N is small.
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
    // find() — O(log N) binary search.
    // Returns a pointer directly into the .rodata asset table.
    // Returns nullptr when the path is not registered.
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
    // forEach — iterate in original declaration order.
    // -------------------------------------------------------
    template <typename Fn>
    constexpr void forEach(AssetKind t_kind, Fn&& t_fn) const noexcept {
        for (const auto& asset : assets_) {
            if (asset.kind == t_kind) {
                t_fn(asset);
            }
        }
    }

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

// CTAD deduction guide — AssetRegistry{ASSETS} infers N.
template <size_t N>
AssetRegistry(const EmbeddedAsset (&)[N]) -> AssetRegistry<N>;

} // namespace sgrn
