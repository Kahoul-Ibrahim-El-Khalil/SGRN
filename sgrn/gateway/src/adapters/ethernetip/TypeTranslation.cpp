#include <sgrn/gateway/adapters/ethernetip/TypeTranslation.hpp>
#include <algorithm>
#include <s7codec/codec.hpp>

namespace sgrn::gateway::adapters::ethernetip::TypeTranslation
{

size_t getAlignment(const sgrn::scl::DbField& t_field) {
    using sgrn::scl::DataType;
    if (!t_field.children.empty() || t_field.type == DataType::Struct) {
        size_t max_align = 1;
        for (const auto& child : t_field.children) {
            max_align = std::max(max_align, getAlignment(child));
        }
        return std::min<size_t>(max_align, 4);
    }

    switch (t_field.type) {
        case DataType::Bool:
        case DataType::Byte:
        case DataType::SInt:
        case DataType::USInt:
        case DataType::Char:
            return 1;
        case DataType::Word:
        case DataType::UInt:
        case DataType::Int:
            return 2;
        default:
            return 4; // DWord, Real, String, etc.
    }
}

size_t s7SpanBytes(const sgrn::scl::DbField& t_field) {
    return (!t_field.children.empty() || t_field.type == sgrn::scl::DataType::Struct)
               ? size_t(t_field.struct_size) * size_t(std::max<uint32_t>(1u, t_field.count))
               : size_t(sgrn::scl::rawTypeSpanBytes(t_field.type, t_field.count));
}

// A unified template function that deduplicates the scalar loading and storing logic.
// By using a constexpr bool flag `IsLoad`, the compiler generates two optimized versions of this function:
// one for reading from S7 bytes (load) and one for writing to S7 bytes (store), sharing the exact same type switch.
template <bool IsLoad>
static bool transferScalar(const sgrn::scl::DbField& t_field, uint8_t* tp_s7_buf, void* tp_cip_buf) {
    using sgrn::scl::DataType;

    // Generic lambda to handle the actual byte conversion via s7codec.
    // We use .template operator()<T>() below due to C++ dependent name lookup rules.
    auto transfer = [&]<typename T>() {
        if constexpr (IsLoad) {
            *static_cast<T*>(tp_cip_buf) = s7codec::fromEndian<T>(tp_s7_buf, t_field.endianness);
        } else {
            s7codec::toEndian<T>(*static_cast<const T*>(tp_cip_buf), tp_s7_buf, t_field.endianness);
        }
    };

    switch (t_field.type) {
        case DataType::Bool: {
            if constexpr (IsLoad) {
                const bool v = (tp_s7_buf[0] & (1 << t_field.bit_index)) != 0;
                *static_cast<uint8_t*>(tp_cip_buf) = v ? 1u : 0u;
            } else {
                // Not supported for boolean store directly via this path in CIP codec
                // CIP Codec usually handles booleans differently in store (or leaves to a custom impl if needed)
                // But following original logic: original didn't implement store for Bool, so returning false.
                return false;
            }
            return true;
        }
        case DataType::Byte:
        case DataType::USInt:
        case DataType::Char:
            if constexpr (IsLoad) {
                *static_cast<uint8_t*>(tp_cip_buf) = tp_s7_buf[0];
            } else {
                tp_s7_buf[0] = *static_cast<const uint8_t*>(tp_cip_buf);
            }
            return true;
        case DataType::SInt:
            transfer.template operator()<int8_t>();
            return true;
        case DataType::Word:
        case DataType::UInt:
            transfer.template operator()<uint16_t>();
            return true;
        case DataType::Int:
            transfer.template operator()<int16_t>();
            return true;
        case DataType::DWord:
        case DataType::UDInt:
        case DataType::Time:
        case DataType::TimeOfDay:
            transfer.template operator()<uint32_t>();
            return true;
        case DataType::DInt:
            transfer.template operator()<int32_t>();
            return true;
        case DataType::Real:
            transfer.template operator()<float>();
            return true;
        case DataType::LWord:
        case DataType::ULInt:
        case DataType::LTime:
        case DataType::LTimeOfDay:
            transfer.template operator()<uint64_t>();
            return true;
        case DataType::LInt:
            transfer.template operator()<int64_t>();
            return true;
        case DataType::LReal:
            transfer.template operator()<double>();
            return true;
        default:
            return false;
    }
}

bool loadScalarFromS7(const sgrn::scl::DbField& t_field, const uint8_t* tp_s7_buf, void* tp_dst_buf) {
    return transferScalar<true>(t_field, const_cast<uint8_t*>(tp_s7_buf), tp_dst_buf);
}

bool storeScalarToS7(const sgrn::scl::DbField& t_field, const void* tp_src_buf, uint8_t* tp_s7_buf) {
    return transferScalar<false>(t_field, tp_s7_buf, const_cast<void*>(tp_src_buf));
}

} // namespace sgrn::gateway::adapters::ethernetip::TypeTranslation
