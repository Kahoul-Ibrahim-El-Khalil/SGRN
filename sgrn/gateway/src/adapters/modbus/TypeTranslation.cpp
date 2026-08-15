#include <fmt/core.h>
#include <sgrn/gateway/adapters/modbus/TypeTranslation.hpp>
#include <cmath>
#include <cstring>
#include <s7codec/codec.hpp>

namespace sgrn::gateway::adapters::modbus::TypeTranslation
{

std::string decodeBytesToString(sgrn::scl::DataType t_type, int t_useful_bytes, const uint8_t* tp_bytes) {
    using T = sgrn::scl::DataType;
    switch (t_type) {
        case T::Bool: {
            bool v = tp_bytes[0] != 0;
            return v ? "true" : "false";
        }
        case T::Byte:
        case T::USInt:
            return fmt::format("{}", tp_bytes[0]);
        case T::SInt: {
            int8_t v;
            std::memcpy(&v, tp_bytes, 1);
            return fmt::format("{}", v);
        }
        case T::Word:
        case T::UInt:
            return fmt::format("{}", s7codec::fromBE<uint16_t>(tp_bytes));
        case T::Int:
            return fmt::format("{}", s7codec::fromBE<int16_t>(tp_bytes));
        case T::DWord:
        case T::UDInt:
            return fmt::format("{}", s7codec::fromBE<uint32_t>(tp_bytes));
        case T::DInt:
            return fmt::format("{}", s7codec::fromBE<int32_t>(tp_bytes));
        case T::LWord:
        case T::ULInt:
            return fmt::format("{}", s7codec::fromBE<uint64_t>(tp_bytes));
        case T::LInt:
            return fmt::format("{}", s7codec::fromBE<int64_t>(tp_bytes));
        case T::Real: {
            float v = s7codec::fromBE<float>(tp_bytes);
            if (std::isinf(v) || std::isnan(v))
                return "null";
            return fmt::format("{:.7g}", v);
        }
        case T::LReal: {
            double v = s7codec::fromBE<double>(tp_bytes);
            if (std::isinf(v) || std::isnan(v))
                return "null";
            return fmt::format("{:.15g}", v);
        }
        default: {
            std::string hex = "[";
            for (int i = 0; i < t_useful_bytes; ++i)
                hex += fmt::format("{}{}", tp_bytes[i], i + 1 < t_useful_bytes ? "," : "");
            return hex + "]";
        }
    }
}

} // namespace sgrn::gateway::adapters::modbus::TypeTranslation
