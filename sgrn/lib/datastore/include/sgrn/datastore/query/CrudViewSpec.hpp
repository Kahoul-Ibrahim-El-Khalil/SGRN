#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sgrn::datastore::query
{

// Bitmask of SQL comparison operators a column may be filtered with.
// Deliberately closed-by-default: a Field with filter_ops == 0 is not
// filterable through the API at all, regardless of what the client sends.
enum class Op : uint8_t {
    Eq = 1 << 0,
    Neq = 1 << 1,
    Gt = 1 << 2,
    Gte = 1 << 3,
    Lt = 1 << 4,
    Lte = 1 << 5,
    Like = 1 << 6,
    In = 1 << 7,
};

constexpr uint8_t operator|(Op a, Op b) {
    return static_cast<uint8_t>(a) | static_cast<uint8_t>(b);
}
constexpr uint8_t operator|(uint8_t a, Op b) {
    return a | static_cast<uint8_t>(b);
}
constexpr bool operator&(uint8_t a, Op b) {
    return (a & static_cast<uint8_t>(b)) != 0;
}

enum class FieldType { Int, BigInt, Text, Bool, Timestamp, Jsonb };

struct Field {
    std::string_view name; // must equal the real column name — no aliasing
    FieldType type;
    uint8_t filter_ops = 0;  // 0 = not filterable; set from `sgrn: filter=...` comment
    bool insertable = false; // default false unless comment/grant says otherwise
    bool updatable = false;
};

struct PrimaryKey {
    std::string_view name;
    FieldType type;
};

struct CrudViewSpec {
    std::string_view read_relation; // what SELECT reads from (may be a view)
    std::string_view write_table;   // what INSERT/UPDATE/DELETE hit (the real table)
    std::string_view tenant_column; // bound server-side only, never client-settable
    PrimaryKey pk;
    std::span<const Field> fields; // whitelist — nothing outside this is reachable
    std::string_view default_order;
    std::size_t max_limit = 500;
};

} // namespace sgrn::datastore::query
