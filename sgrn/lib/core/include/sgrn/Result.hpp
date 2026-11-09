#pragma once

#include <fmt/core.h>
#include <fmt/format.h>
#include <expected>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
namespace sgrn
{
// ---------------------------------------------------------------------------
// Result<T, E> — thin wrapper over std::expected<T, E>
//
// Design contract:
//   SUCCESS — implicit: `return value;` works via std::expected's constructor.
//   ERROR   — explicit: `return std::unexpected(e);`
//                    or `return Result<T,E>::Error(e);`  ← preferred factory
//
// Sugar provided on top of std::expected:
//   .hasError()          — inverse of has_value()
//   ::Error(E)           — explicit error factory (clear, grep-able)
// ---------------------------------------------------------------------------

template <typename T, typename E = std::string>
class Result : public std::expected<T, E> {
    using base = std::expected<T, E>;

public:
    // ── Inherit all std::expected constructors + assignment operators ──────
    using base::base;
    using base::operator=;

    // ── Inherit all ref-qualified error() overloads from std::expected ────
    // Includes error() && for zero-copy move propagation:
    //   co_return BackendResult<void>::Error(std::move(r).error());
    using base::error;

    // Enables `return SomeError{code, "msg"};` and `return r.error();`
    // as sugar for `return std::unexpected(e);`.
    // Safe because E is never implicitly convertible to T in our domain.
    template <typename Err, std::enable_if_t<std::is_constructible_v<E, Err> && !std::is_constructible_v<T, Err>, int> = 0>
    Result(Err&& t_e)
        : base(std::unexpect, E(std::forward<Err>(t_e))) {
    }

    // Direct-initialise E from a std::unexpected<G>. Needed because the
    // inherited expected(unexpected<G>) constructor is explicit whenever
    // E's conversion from G is explicit (e.g. std::string from string_view),
    // which makes `return sgrn::Error(...)` ill-formed. Direct-initialising
    // here keeps the Error()/macro sugar usable with such error types.
    template <typename G, std::enable_if_t<std::is_constructible_v<E, G>, int> = 0>
    Result(std::unexpected<G>&& t_e)
        : base(std::unexpect, E(std::move(t_e).error())) {
    }

    template <typename G, std::enable_if_t<std::is_constructible_v<E, const G&>, int> = 0>
    Result(const std::unexpected<G>& t_e)
        : base(std::unexpect, E(t_e.error())) {
    }

    // ── Syntactic sugar ───────────────────────────────────────────────────
    [[nodiscard]] bool hasError() const noexcept {
        return !base::has_value();
    }
    // ── Syntactic sugar ───────────────────────────────────────────────────
    [[nodiscard]] bool hasValue() const noexcept {
        return base::has_value();
    }

    template <typename... Args>
    static Result<T, E> Error(Args&&... t_args) {
        return Result<T, E>(std::unexpect, E(std::forward<Args>(t_args)...));
    }
};
template <typename T, typename E>
inline sgrn::Result<T, E> Error(auto&& t_error) {
    return Result<T, E>(std::unexpect, t_error);
}
template <typename E>
[[nodiscard]] constexpr auto Error(E&& e) {
    return std::unexpected<std::decay_t<E>>(std::forward<E>(e));
}

#define PROPAGATE_ERROR_IF(expr)                                                                                                           \
    do {                                                                                                                                   \
        auto&& _sgrn_result = (expr);                                                                                                      \
        if (!_sgrn_result.has_value())                                                                                                     \
            return ::sgrn::Error(std::move(_sgrn_result).error());                                                                         \
    } while (false)

#define ASSIGN_OR_RETURN(lhs, expr)                                                                                                        \
    do {                                                                                                                                   \
        auto&& _sgrn_result = (expr);                                                                                                      \
        if (!_sgrn_result.has_value())                                                                                                     \
            return ::sgrn::Error(_sgrn_result.error());                                                                                    \
        (lhs) = std::move(_sgrn_result.value());                                                                                           \
    } while (false)

#define RETURN_ERROR_IF(condition, error)                                                                                                  \
    do {                                                                                                                                   \
        if (condition)                                                                                                                     \
            return ::sgrn::Error(error);                                                                                                   \
    } while (false)

#define RETURN_IF(condition, error)                                                                                                        \
    do {                                                                                                                                   \
        if (condition)                                                                                                                     \
            return error;                                                                                                                  \
    } while (false)

#define RETURN_IF_NULL(ptr, error)                                                                                                         \
    do {                                                                                                                                   \
        if (!ptr)                                                                                                                          \
            return error;                                                                                                                  \
    } while (false)

#define RETURN_ERROR(...) return ::sgrn::Error(__VA_ARGS__)
#define CO_PROPAGATE_ERROR_IF(expr)                                                                                                        \
    do {                                                                                                                                   \
        auto&& _sgrn_result = (expr);                                                                                                      \
        if (!_sgrn_result.has_value())                                                                                                     \
            co_return ::sgrn::Error(std::move(_sgrn_result).error());                                                                      \
    } while (false)

#define CO_ASSIGN_OR_RETURN(lhs, expr)                                                                                                     \
    do {                                                                                                                                   \
        auto&& _sgrn_result = (expr);                                                                                                      \
        if (!_sgrn_result.has_value())                                                                                                     \
            co_return ::sgrn::Error(_sgrn_result.error());                                                                                 \
        (lhs) = std::move(_sgrn_result.value());                                                                                           \
    } while (false)

#define CO_RETURN_ERROR_IF(condition, error)                                                                                               \
    do {                                                                                                                                   \
        if (condition)                                                                                                                     \
            co_return ::sgrn::Error(error);                                                                                                \
    } while (false)

#define CO_RETURN_IF(condition, value)                                                                                                     \
    do {                                                                                                                                   \
        if (condition)                                                                                                                     \
            co_return value;                                                                                                               \
    } while (false)

#define CO_RETURN_IF_NULL(ptr, value)                                                                                                      \
    do {                                                                                                                                   \
        if (!ptr)                                                                                                                          \
            co_return value;                                                                                                               \
    } while (false)

#define CO_RETURN_ERROR(...) co_return ::sgrn::Error(__VA_ARGS__)

} // namespace sgrn
