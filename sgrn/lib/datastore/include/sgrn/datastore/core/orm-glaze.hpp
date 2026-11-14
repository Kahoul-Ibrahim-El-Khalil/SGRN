#pragma once
// orm-glaze.hpp — Drogon ORM to native C++ struct reflection mapping via Glaze
//
// Eliminates repetitive manual boilerplate when mapping drogon::orm::Row and
// drogon::orm::Result into C++ structs. Supports:
// 1. Compile-Time Query Verification & Routing: passing the SQL query at compile
//    time parses the SELECT projection, verifies every field at compile time
//    (static_assert), and computes an O(1) direct index permutation map with
//    zero runtime overhead.
// 2. Pure Positional Mapping (O(1) index-based, fastest when order matches).
// 3. Named Mapping (runtime column resolution, order-independent).
//
// Automatically handles std::optional<T> and null database fields.

#include <drogon/orm/Field.h>
#include <drogon/orm/Result.h>
#include <drogon/orm/ResultIterator.h>
#include <drogon/orm/Row.h>
#include <glaze/glaze.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace sgrn::orm
{

/// Structural fixed string wrapper for passing SQL queries as NTTP (Non-Type Template Parameters)
template <std::size_t N>
struct FixedString {
    char buf[N]{};

    constexpr FixedString() noexcept = default;

    constexpr FixedString(const char (&str)[N]) noexcept {
        for (std::size_t i = 0; i < N; ++i) {
            buf[i] = str[i];
        }
    }

    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return N ? N - 1 : 0;
    }

    [[nodiscard]] constexpr const char* data() const noexcept {
        return buf;
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return {buf, size()};
    }

    constexpr operator std::string_view() const noexcept {
        return view();
    }
};

template <std::size_t N>
FixedString(const char (&)[N]) -> FixedString<N>;

namespace detail
{

template <typename T>
struct is_optional : std::false_type {};

template <typename T>
struct is_optional<std::optional<T>> : std::true_type {};

template <typename T>
inline constexpr bool is_optional_v = is_optional<T>::value;

template <typename T>
struct is_vector : std::false_type {};

template <typename T, typename Alloc>
struct is_vector<std::vector<T, Alloc>> : std::true_type {};

template <typename T>
inline constexpr bool is_vector_v = is_vector<T>::value;

template <typename Target>
Target extractField(const drogon::orm::Field& field) {
    if constexpr (is_optional_v<Target>) {
        if (field.isNull()) {
            return std::nullopt;
        }
        using Val = typename Target::value_type;
        return Val(extractField<Val>(field));
    } else if constexpr (is_vector_v<Target>) {
        if (field.isNull()) {
            return Target{};
        }
        const std::string_view sv = field.template as<std::string_view>();
        if (sv.empty() || sv == "{}" || sv == "[]") {
            return Target{};
        }
        Target out{};
        if (sv.front() == '[') {
            auto ec = glz::read_json(out, sv);
            if (!ec) {
                return out;
            }
        }
        return Target{};
    } else if constexpr (std::is_same_v<Target, bool>) {
        if (field.isNull()) {
            return false;
        }
        const auto len = field.length();
        if (len == 1) {
            const char* val = field.c_str();
            return val && (*val == 't' || *val == 'T' || *val == '1');
        } else if (len > 1) {
            const std::string_view sv = field.template as<std::string_view>();
            return sv == "true" || sv == "TRUE" || sv == "t" || sv == "1";
        }
        return false;
    } else if constexpr (std::is_integral_v<Target>) {
        if (field.isNull()) {
            return Target{0};
        }
        if constexpr (std::is_signed_v<Target>) {
            return static_cast<Target>(field.template as<long long>());
        } else {
            return static_cast<Target>(field.template as<unsigned long long>());
        }
    } else if constexpr (std::is_floating_point_v<Target>) {
        if (field.isNull()) {
            return Target{0};
        }
        return static_cast<Target>(field.template as<double>());
    } else if constexpr (std::is_same_v<Target, std::string>) {
        if (field.isNull()) {
            return std::string{};
        }
        return field.template as<std::string>();
    } else if constexpr (std::is_same_v<Target, std::string_view>) {
        if (field.isNull()) {
            return std::string_view{};
        }
        return field.template as<std::string_view>();
    } else if constexpr (std::is_constructible_v<Target, std::string>) {
        if (field.isNull()) {
            return Target{};
        }
        return Target{field.template as<std::string>()};
    } else {
        if (field.isNull()) {
            return Target{};
        }
        return field.template as<Target>();
    }
}

// ── Compile-time SQL parsing helpers ─────────────────────────────────────────

constexpr bool isWhitespace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

constexpr char toLower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
}

constexpr bool equalsIgnoreCase(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (toLower(a[i]) != toLower(b[i])) {
            return false;
        }
    }
    return true;
}

constexpr std::string_view trimWhitespace(std::string_view sv) noexcept {
    while (!sv.empty() && isWhitespace(sv.front())) {
        sv.remove_prefix(1);
    }
    while (!sv.empty() && isWhitespace(sv.back())) {
        sv.remove_suffix(1);
    }
    return sv;
}

constexpr std::size_t findKeywordTopLevel(std::string_view query, std::string_view keyword, std::size_t start_pos = 0) noexcept {
    int paren_depth = 0;
    const std::size_t klen = keyword.size();
    if (query.size() < klen) {
        return std::string_view::npos;
    }
    for (std::size_t i = start_pos; i + klen <= query.size(); ++i) {
        if (query[i] == '(') {
            ++paren_depth;
        } else if (query[i] == ')') {
            if (paren_depth > 0) {
                --paren_depth;
            }
        } else if (paren_depth == 0) {
            const bool boundary_before = (i == 0 || isWhitespace(query[i - 1]));
            const bool boundary_after =
                (i + klen == query.size() || isWhitespace(query[i + klen]) || query[i + klen] == '(' || query[i + klen] == ';');
            if (boundary_before && boundary_after && equalsIgnoreCase(query.substr(i, klen), keyword)) {
                return i;
            }
        }
    }
    return std::string_view::npos;
}

constexpr std::string_view extractColumnName(std::string_view raw_col) noexcept {
    std::string_view expr = trimWhitespace(raw_col);
    if (expr.empty()) {
        return {};
    }

    // Check for explicit " AS alias"
    const std::size_t as_pos = findKeywordTopLevel(expr, "AS");
    if (as_pos != std::string_view::npos) {
        std::string_view alias = trimWhitespace(expr.substr(as_pos + 2));
        if (alias.size() >= 2 && alias.front() == '"' && alias.back() == '"') {
            alias = alias.substr(1, alias.size() - 2);
        }
        return alias;
    }

    // Check for whitespace-separated alias: "expr alias" (where alias has no nested parens)
    int paren_depth = 0;
    std::size_t last_space = std::string_view::npos;
    for (std::size_t i = 0; i < expr.size(); ++i) {
        if (expr[i] == '(') {
            ++paren_depth;
        } else if (expr[i] == ')') {
            if (paren_depth > 0) {
                --paren_depth;
            }
        } else if (paren_depth == 0 && isWhitespace(expr[i])) {
            last_space = i;
        }
    }
    if (last_space != std::string_view::npos) {
        std::string_view possible_alias = trimWhitespace(expr.substr(last_space + 1));
        if (!possible_alias.empty() && possible_alias.find_first_of("(),.+-*/") == std::string_view::npos) {
            if (possible_alias.size() >= 2 && possible_alias.front() == '"' && possible_alias.back() == '"') {
                possible_alias = possible_alias.substr(1, possible_alias.size() - 2);
            }
            return possible_alias;
        }
    }

    // Strip table qualification: "table.column" -> "column"
    const std::size_t dot_pos = expr.find_last_of('.');
    if (dot_pos != std::string_view::npos && dot_pos + 1 < expr.size()) {
        expr = expr.substr(dot_pos + 1);
    }

    // Strip quotes if any: "\"id\"" -> "id"
    if (expr.size() >= 2 && expr.front() == '"' && expr.back() == '"') {
        expr = expr.substr(1, expr.size() - 2);
    }

    return trimWhitespace(expr);
}

template <std::size_t MaxCols = 64>
constexpr auto parseSqlProjection(std::string_view query) noexcept {
    struct ParseResult {
        std::array<std::string_view, MaxCols> cols{};
        std::size_t count{0};
    } res{};

    const std::size_t select_pos = findKeywordTopLevel(query, "SELECT");
    if (select_pos == std::string_view::npos) {
        return res;
    }

    std::size_t start = select_pos + 6;
    std::string_view after_select = trimWhitespace(query.substr(start));

    // Skip DISTINCT if present
    if (after_select.size() >= 8 && equalsIgnoreCase(after_select.substr(0, 8), "DISTINCT") &&
        (after_select.size() == 8 || isWhitespace(after_select[8]))) {
        after_select = trimWhitespace(after_select.substr(8));
        start = query.size() - after_select.size();
    }

    std::size_t from_pos = findKeywordTopLevel(query, "FROM", start);
    if (from_pos == std::string_view::npos) {
        from_pos = query.size();
    }

    const std::string_view proj = query.substr(start, from_pos - start);

    int paren_depth = 0;
    std::size_t token_start = 0;
    for (std::size_t i = 0; i <= proj.size(); ++i) {
        const bool at_end = (i == proj.size());
        if (!at_end) {
            if (proj[i] == '(') {
                ++paren_depth;
            } else if (proj[i] == ')') {
                if (paren_depth > 0) {
                    --paren_depth;
                }
            }
        }

        if ((at_end || proj[i] == ',') && paren_depth == 0) {
            const std::string_view token = proj.substr(token_start, i - token_start);
            const std::string_view col_name = extractColumnName(token);
            if (!col_name.empty() && res.count < MaxCols) {
                res.cols[res.count++] = col_name;
            }
            token_start = i + 1;
        }
    }

    return res;
}

} // namespace detail

// ── Compile-time Query Permutation & Validation ──────────────────────────────

template <FixedString Query, typename T>
struct QueryMapping {
    static constexpr auto parsed = detail::parseSqlProjection(Query.view());
    static constexpr std::size_t num_sql_cols = parsed.count;
    static constexpr std::size_t num_struct_fields = glz::reflect<T>::size;
    static constexpr auto struct_keys = glz::reflect<T>::keys;

    static constexpr std::size_t kNotFound = static_cast<std::size_t>(-1);

    /// Compute compile-time permutation map: col_indices[I] gives the SQL column index for struct field I
    static constexpr auto col_indices = []() consteval {
        std::array<std::size_t, num_struct_fields> indices{};
        for (std::size_t i = 0; i < num_struct_fields; ++i) {
            const std::string_view key = struct_keys[i];
            std::size_t found_idx = kNotFound;
            for (std::size_t c = 0; c < num_sql_cols; ++c) {
                if (key == parsed.cols[c]) {
                    found_idx = c;
                    break;
                }
            }
            indices[i] = found_idx;
        }
        return indices;
    }();
};

// ── Strategy Two: Compile-Time Verified & Permuted Mapping ────────────────────

/// Map a single drogon::orm::Row into struct T using compile-time SQL query routing.
/// Parses the query projection at compile-time, verifies non-optional fields,
/// and reads row fields using direct compile-time O(1) indices (zero runtime string lookups).
template <typename T, FixedString Query>
T fromRow(const drogon::orm::Row& row) {
    using Mapping = QueryMapping<Query, T>;
    static_assert(Mapping::num_sql_cols > 0, "Query does not contain a valid SELECT projection!");

    T obj{};
    constexpr std::size_t N = glz::reflect<T>::size;
    if constexpr (N > 0) {
        auto tie = glz::to_tie(obj);
        const auto row_size = row.size();
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (
                [&]() {
                    if constexpr (I < N) {
                        constexpr std::size_t col_idx = Mapping::col_indices[I];
                        using FieldType = std::remove_cvref_t<decltype(glz::get<I>(tie))>;
                        if constexpr (!detail::is_optional_v<FieldType>) {
                            static_assert(col_idx != Mapping::kNotFound,
                                "Compile-time SQL check failed: Required struct field not found in SELECT query projection!");
                        }
                        if constexpr (col_idx != Mapping::kNotFound) {
                            if (col_idx < row_size) {
                                glz::get<I>(tie) = detail::extractField<FieldType>(row[static_cast<drogon::orm::Row::SizeType>(col_idx)]);
                            }
                        }
                    }
                }(),
                ...);
        }(std::make_index_sequence<N>{});
    }
    return obj;
}

/// Map a drogon::orm::Result into std::vector<T> using compile-time SQL query routing.
template <typename T, FixedString Query>
std::vector<T> fromResult(const drogon::orm::Result& result) {
    std::vector<T> items;
    const auto sz = result.size();
    items.reserve(sz);
    for (drogon::orm::Result::SizeType r = 0; r < sz; ++r) {
        items.push_back(fromRow<T, Query>(result[r]));
    }
    return items;
}

/// Map a single result (or first row) into std::optional<T> using compile-time SQL query routing.
template <typename T, FixedString Query>
std::optional<T> fromSingleResult(const drogon::orm::Result& result) {
    if (result.empty()) {
        return std::nullopt;
    }
    return fromRow<T, Query>(result[0]);
}

// ── Positional Mapping (Direct Index O(1)) ────────────────────────────────────

/// Map a single drogon::orm::Row into struct T by positional index (O(1)).
/// Column 0 maps to member 0, Column 1 maps to member 1, etc.
template <typename T>
T fromRow(const drogon::orm::Row& row) {
    T obj{};
    constexpr std::size_t N = glz::reflect<T>::size;
    if constexpr (N > 0) {
        auto tie = glz::to_tie(obj);
        const auto row_size = row.size();
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (
                [&]() {
                    if constexpr (I < N) {
                        if (I < row_size) {
                            using FieldType = std::remove_cvref_t<decltype(glz::get<I>(tie))>;
                            glz::get<I>(tie) = detail::extractField<FieldType>(row[static_cast<drogon::orm::Row::SizeType>(I)]);
                        }
                    }
                }(),
                ...);
        }(std::make_index_sequence<N>{});
    }
    return obj;
}

/// Map a drogon::orm::Result into std::vector<T> by positional index (O(1)).
template <typename T>
std::vector<T> fromResult(const drogon::orm::Result& result) {
    std::vector<T> items;
    const auto sz = result.size();
    items.reserve(sz);
    for (drogon::orm::Result::SizeType r = 0; r < sz; ++r) {
        items.push_back(fromRow<T>(result[r]));
    }
    return items;
}

/// Map a single result (or first row) into std::optional<T> by positional index.
template <typename T>
std::optional<T> fromSingleResult(const drogon::orm::Result& result) {
    if (result.empty()) {
        return std::nullopt;
    }
    return fromRow<T>(result[0]);
}

// ── Named Mapping (Runtime Resilient) ─────────────────────────────────────────

/// Map a single drogon::orm::Row into struct T by field name matching.
template <typename T>
T fromRowByName(const drogon::orm::Row& row) {
    T obj{};
    constexpr std::size_t N = glz::reflect<T>::size;
    if constexpr (N > 0) {
        auto tie = glz::to_tie(obj);
        constexpr auto keys = glz::reflect<T>::keys;
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (
                [&]() {
                    if constexpr (I < N) {
                        constexpr std::string_view key = keys[I];
                        using FieldType = std::remove_cvref_t<decltype(glz::get<I>(tie))>;
                        try {
                            const auto field = row[std::string(key)];
                            glz::get<I>(tie) = detail::extractField<FieldType>(field);
                        } catch (...) {
                            // Column absent from query result
                        }
                    }
                }(),
                ...);
        }(std::make_index_sequence<N>{});
    }
    return obj;
}

/// Map a drogon::orm::Result into std::vector<T> by column name matching.
/// Resolves column indices ONCE across all rows in the Result, achieving O(1)
/// positional access for every row iteration while remaining order-independent.
template <typename T>
std::vector<T> fromResultByName(const drogon::orm::Result& result) {
    std::vector<T> items;
    if (result.empty()) {
        return items;
    }
    items.reserve(result.size());

    constexpr std::size_t N = glz::reflect<T>::size;
    std::array<ssize_t, N> col_indices;
    col_indices.fill(-1);

    const auto num_cols = result.columns();
    for (std::size_t col = 0; col < num_cols; ++col) {
        const std::string_view col_name = result.columnName(col);
        for (std::size_t i = 0; i < N; ++i) {
            if (col_name == glz::reflect<T>::keys[i]) {
                col_indices[i] = static_cast<ssize_t>(col);
                break;
            }
        }
    }

    const auto sz = result.size();
    for (drogon::orm::Result::SizeType r = 0; r < sz; ++r) {
        const auto& row = result[r];
        T obj{};
        auto tie = glz::to_tie(obj);
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (
                [&]() {
                    if constexpr (I < N) {
                        const ssize_t col_idx = col_indices[I];
                        if (col_idx >= 0 && static_cast<std::size_t>(col_idx) < row.size()) {
                            using FieldType = std::remove_cvref_t<decltype(glz::get<I>(tie))>;
                            glz::get<I>(tie) = detail::extractField<FieldType>(row[static_cast<drogon::orm::Row::SizeType>(col_idx)]);
                        }
                    }
                }(),
                ...);
        }(std::make_index_sequence<N>{});
        items.push_back(std::move(obj));
    }
    return items;
}

/// Map a single result (or first row) into std::optional<T> by field name matching.
template <typename T>
std::optional<T> fromSingleResultByName(const drogon::orm::Result& result) {
    if (result.empty()) {
        return std::nullopt;
    }
    return fromRowByName<T>(result[0]);
}

} // namespace sgrn::orm

namespace sgrn::datastore::orm
{
using namespace sgrn::orm;
} // namespace sgrn::datastore::orm
