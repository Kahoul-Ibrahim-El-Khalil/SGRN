#include <sgrn/Result.hpp>
#include <sgrn/gateway/adapters/opcua/TypeTranslation.hpp>
#include <open62541/types_generated.h>
#include <s7codec/codec.hpp>

using ::sgrn::scl::DataType;

namespace sgrn::gateway::adapters
{

sgrn::Result<int, std::string> dataTypeToUaTypeIndex(DataType t_type) {
    try {
        return sgrn::scl::info_of(t_type).ua_type_index;
    } catch (...) {
        return sgrn::Error("Data Type unkwown");
    }
}

} // namespace sgrn::gateway::adapters
