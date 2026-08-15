#include <sgrn/gateway/adapters/opcua/TypeTranslation.hpp>
#include <open62541/types_generated.h>
#include <s7codec/codec.hpp>

using ::sgrn::scl::DataType;

namespace sgrn::gateway::adapters
{

int s7TypeToUaTypeIndex(DataType t_type) {
    switch (t_type) {
        case DataType::Bool:
            return UA_TYPES_BOOLEAN;
        case DataType::Byte:
        case DataType::USInt:
            return UA_TYPES_BYTE;
        case DataType::SInt:
            return UA_TYPES_SBYTE;
        case DataType::Char:
            return UA_TYPES_BYTE;
        case DataType::WChar:
            return UA_TYPES_UINT16; // no native OPC UA wide-char type; UInt16 preserves
                                    // the UTF-16 code unit losslessly (same precedent as Char→Byte)
        case DataType::Int:
            return UA_TYPES_INT16;
        case DataType::Word:
        case DataType::UInt:
            return UA_TYPES_UINT16;
        case DataType::DInt:
            return UA_TYPES_INT32;
        case DataType::DWord:
        case DataType::UDInt:
            return UA_TYPES_UINT32;
        case DataType::LInt:
            return UA_TYPES_INT64;
        case DataType::LWord:
        case DataType::ULInt:
            return UA_TYPES_UINT64;
        case DataType::Real:
            return UA_TYPES_FLOAT;
        case DataType::LReal:
            return UA_TYPES_DOUBLE;
        case DataType::Time:
            // OPC UA's native "Duration" (Part 8 §5.6.2) is itself a Double subtype in ms —
            // was UA_TYPES_INT32 here while memory_to_ua.cpp's scalar encoder always emits
            // a Double for Time, so the node's declared DataType attribute never matched
            // the Value it served. Double is both the spec-correct choice and the one that
            // now actually matches the encoder.
            return UA_TYPES_DOUBLE;
        case DataType::LTime:
            return UA_TYPES_INT64; // ns duration
        case DataType::Date:
            return UA_TYPES_UINT16;
        case DataType::TimeOfDay:
            return UA_TYPES_UINT32;
        case DataType::DTL:
        case DataType::DateTime:
            return UA_TYPES_DATETIME; // was falling to -1 → served as UA_String; see
                                      // memory_to_ua.cpp companion fix below
        case DataType::Counter:
        case DataType::Timer:
            return UA_TYPES_UINT16;
        case DataType::String:
        case DataType::WString:
        case DataType::XString:
        case DataType::XWString:
            return UA_TYPES_STRING;
        default:
            return -1; // fallback to JSON string
    }
}
size_t boolArrayByteCount(size_t t_bit_count) {
    return (t_bit_count + 7U) / 8U;
}

bool writeBoolArrayToS7(const UA_Boolean* tp_source, size_t t_count, uint8_t* tp_destination, size_t t_destination_size) {
    for (size_t index = 0; index < t_count; ++index) {
        const size_t byte_index = index / 8U;
        if (byte_index >= t_destination_size)
            return false;
        auto status = s7codec::encodeBool(
            tp_source[index] != 0, static_cast<int>(index % 8U), tp_destination + byte_index, t_destination_size - byte_index);
        if (!status.ok())
            return false;
    }
    return true;
}

} // namespace sgrn::gateway::adapters
