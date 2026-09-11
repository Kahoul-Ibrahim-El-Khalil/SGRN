#pragma once

#include <sgrn/common/ErrorClass.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>

namespace sgrn::gateway::common
{

/// Maps a twin failure onto the protocol-agnostic severity class that every
/// protocol adapter's wire-code translator switches on. Kept here (not in
/// sgrn/common) because it names twin::PlcMemoryError — sgrn/common must
/// stay independent of sgrn_gateway_twin.
constexpr ::sgrn::common::ErrorClass classify(twin::PlcMemoryError t_status) {
    using twin::PlcMemoryError;
    switch (t_status) {
        case PlcMemoryError::PLC_STATE_NOT_INITIALIZED:
            return ::sgrn::common::ErrorClass::NotInitialized;
        case PlcMemoryError::DB_SEGMENT_NOT_FOUND:
        case PlcMemoryError::UNMAPPED_ARENA_REGION:
            return ::sgrn::common::ErrorClass::NotFound;
        case PlcMemoryError::RANGE_EXCEEDS_ALLOWED_SPACE:
        case PlcMemoryError::RANGE_CROSSES_SEGMENT_BOUNDARY:
            return ::sgrn::common::ErrorClass::OutOfRange;
        case PlcMemoryError::NULL_BUFFER:
        case PlcMemoryError::INVALID_BIT_INDEX:
            return ::sgrn::common::ErrorClass::Internal;
    }
    return ::sgrn::common::ErrorClass::Internal;
}

} // namespace sgrn::gateway::common
