// Shared "contiguous bytes" concepts and adapters.
//
// These let byte-oriented APIs accept anything that is a contiguous range of
// 1-byte trivially-copyable values (std::string, std::string_view,
// std::vector<char>, std::vector<std::byte>, std::array<char, N>, ...) while
// still using string_view internally.
#pragma once
#include <cstddef>
#include <cstring>
#include <ranges>
#include <string_view>
#include <type_traits>

namespace sgrn::utils::bytes
{

template <class T>
concept ByteRange = std::ranges::contiguous_range<const T&> && std::ranges::sized_range<const T&> &&
                    (sizeof(std::ranges::range_value_t<T>) == 1) && std::is_trivially_copyable_v<std::ranges::range_value_t<T>> &&
                    // Built-in arrays (notably string literals) keep the normal
                    // string_view overload, so `compressStringZstd("", 3)`
                    // still means "empty", not "one NUL byte".
                    (!std::is_array_v<std::remove_cvref_t<T>>);

template <class T>
concept MutableByteRange = ByteRange<T> && std::ranges::contiguous_range<T&> && (!std::is_const_v<std::ranges::range_value_t<T>>) &&
                           requires(T& t, std::size_t t_size) { t.resize(t_size); };

template <ByteRange T>
std::string_view view(const T& t_data) {
    const auto size = static_cast<std::size_t>(std::ranges::size(t_data));
    if (size == 0) {
        return {};
    }
    return {reinterpret_cast<const char*>(std::ranges::data(t_data)), size};
}

template <MutableByteRange T>
void assign(T& t_out, std::string_view t_bytes) {
    t_out.resize(t_bytes.size());
    if (!t_bytes.empty()) {
        std::memcpy(static_cast<void*>(std::ranges::data(t_out)), t_bytes.data(), t_bytes.size());
    }
}

} // namespace sgrn::utils::bytes
