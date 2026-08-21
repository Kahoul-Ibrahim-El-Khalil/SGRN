#include <sgrn/gateway/adapters/opcua/TypeTranslation.hpp>
#include <open62541/types_generated.h>
#include <s7codec/codec.hpp>

using ::sgrn::scl::DataType;

namespace sgrn::gateway::adapters
{

int s7TypeToUaTypeIndex(DataType t_type) {
    try {
        return sgrn::scl::info_of(t_type).ua_type_index;
    } catch (...) {
        return -1;
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
