#pragma once

#include <sgrn/Result.hpp>

namespace sgrn::scl
{
class PlcSchemaStore;
} // namespace sgrn::scl

namespace sgrn::gateway::wrappers::opcua
{
class TypeRegistry;
} // namespace sgrn::gateway::wrappers::opcua

namespace sgrn::gateway::adapters
{

/// Translate PlcSchemaStore UDT definitions into open62541 registration data.
///
/// This is the adapter-side home for the legacy registerUdtTypes business
/// logic from main. The wrapper TypeRegistry only stores the result.
sgrn::Result<void> populateTypeRegistryFromSchema(
    const ::sgrn::scl::PlcSchemaStore& t_registry, wrappers::opcua::TypeRegistry& t_type_registry);

} // namespace sgrn::gateway::adapters
