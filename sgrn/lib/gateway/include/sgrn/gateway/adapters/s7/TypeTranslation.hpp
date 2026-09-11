#pragma once

#include <sgrn/common/ErrorClass.hpp>

#include <cstddef>
#include <optional>
#include <snap7.h>

namespace sgrn::gateway::adapters::s7::TypeTranslation
{

/**
 * @brief Normalizes a client area code (e.g., S7AreaPE) to the equivalent Snap7 server area code (e.g., srvAreaPE).
 */
int normalizeServerAreaCode(int t_area_code);

/**
 * @brief Calculates the byte size of a given TS7Tag.
 */
std::optional<size_t> requestByteSize(const TS7Tag& t_tag);

/**
 * @brief Maps a memory-port failure class onto the Snap7 server callback
 * return code: unattached/internal → generic exception, missing DB →
 * area-not-found, range violations → out-of-range. (Previously every
 * backend failure collapsed onto evrErrOutOfRange.)
 */
constexpr int evrCodeForError(sgrn::common::ErrorClass t_class) {
    switch (t_class) {
        case sgrn::common::ErrorClass::NotInitialized:
        case sgrn::common::ErrorClass::Internal:
            return ::evrErrException;
        case sgrn::common::ErrorClass::NotFound:
            return ::evrErrAreaNotFound;
        case sgrn::common::ErrorClass::OutOfRange:
            return ::evrErrOutOfRange;
    }
    return ::evrErrException;
}

} // namespace sgrn::gateway::adapters::s7::TypeTranslation
