#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sgrn::crud
{

enum class FieldType { Int, BigInt, Text, Bool, Timestamp, Jsonb };

namespace Op
{
constexpr uint8_t None = 0;
constexpr uint8_t Eq = 1 << 0;   // 1
constexpr uint8_t Neq = 1 << 1;  // 2
constexpr uint8_t Gt = 1 << 2;   // 4
constexpr uint8_t Gte = 1 << 3;  // 8
constexpr uint8_t Lt = 1 << 4;   // 16
constexpr uint8_t Lte = 1 << 5;  // 32
constexpr uint8_t Like = 1 << 6; // 64
constexpr uint8_t In = 1 << 7;   // 128
} // namespace Op

struct FieldValidation {
    std::string format;
    uint32_t maxlen{0};
    bool unique{false};
    std::vector<std::string> enum_values{};
    double min_val{0.0};
    double max_val{0.0};
    bool has_min{false};
    bool has_max{false};
};

struct CrudFieldSpec {
    std::string name;
    FieldType type{FieldType::Text};
    bool readable{true};
    bool writable{true};
    bool filterable{false};
    uint8_t operators{Op::None};
    FieldValidation validation{};
    bool nullable{true};
    std::string default_val;
    bool soft_delete_field{false};
    bool version_field{false};
};

} // namespace sgrn::crud
