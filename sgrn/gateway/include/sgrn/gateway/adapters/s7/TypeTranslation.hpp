#pragma once

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

} // namespace sgrn::gateway::adapters::s7::TypeTranslation
