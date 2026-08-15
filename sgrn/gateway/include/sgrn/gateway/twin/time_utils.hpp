#pragma once
#include <cstdint>
#include <optional>
#include <string>

namespace sgrn::gateway::twin
{
std::optional<int64_t> parseS7Time(const std::string& t_raw);
} // namespace sgrn::gateway::twin
