#pragma once

#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/wrappers/s7/error.hpp>

namespace sgrn::gateway::wrappers::s7
{

inline S7Error fromPlcMemoryErrorToS7Error(sgrn::gateway::twin::PlcMemoryError t_err) noexcept {

    using sgrn::gateway::twin::PlcMemoryError;

    switch (t_err) {
        case PlcMemoryError::PLC_STATE_NOT_INITIALIZED:
            return S7Error::NotConnected;

        case PlcMemoryError::DB_SEGMENT_NOT_FOUND:
            return S7Error::InvalidParam;

        case PlcMemoryError::RANGE_EXCEEDS_ALLOWED_SPACE:
            return S7Error::InvalidParam;

        case PlcMemoryError::RANGE_CROSSES_SEGMENT_BOUNDARY:
            return S7Error::InvalidParam;

        case PlcMemoryError::UNMAPPED_ARENA_REGION:
            return S7Error::InvalidParam;

        case PlcMemoryError::NULL_BUFFER:
            return S7Error::InvalidParam;

        case PlcMemoryError::INVALID_BIT_INDEX:
            return S7Error::InvalidParam;
    }

    return S7Error::Unknown;
}

} // namespace sgrn::gateway::wrappers::s7
