#pragma once

#include <sgrn/common/ErrorClass.hpp>
#include <sgrn/wrappers/s7/error.hpp>

namespace sgrn::wrappers::s7
{

/// ErrorClass -> S7Error. Group-consistent with the former twin-error
/// switch (only the not-attached case mapped to NotConnected; every
/// other twin failure collapsed onto InvalidParam), so behavior is
/// identical for every value a memory port can produce. NOTE: this header
/// currently has no callers — kept as the S7-side counterpart to the other
/// adapters' wire-code translators.
inline S7Error fromMemoryErrorToS7Error(sgrn::common::ErrorClass t_err) noexcept {

    using sgrn::common::ErrorClass;

    switch (t_err) {
        case ErrorClass::NotInitialized:
            return S7Error::NotConnected;

        case ErrorClass::NotFound:
        case ErrorClass::OutOfRange:
        case ErrorClass::Internal:
            return S7Error::InvalidParam;
    }

    return S7Error::Unknown;
}

} // namespace sgrn::wrappers::s7
