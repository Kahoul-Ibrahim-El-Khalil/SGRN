#include <sgrn/gateway/twin/time_utils.hpp>
#include <sgrn/utils/time.hpp>

namespace sgrn::gateway::twin
{

std::optional<int64_t> parseS7Time(const std::string& t_raw) {
    return sgrn::utils::time::parseS7Time(t_raw);
}

} // namespace sgrn::gateway::twin
