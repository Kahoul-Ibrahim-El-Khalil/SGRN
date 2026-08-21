#include <sgrn/gateway/adapters/opcua/TypeTranslation.hpp>
#include <sgrn/gateway/adapters/opcua/schema_type_registry.hpp>
#include <sgrn/gateway/wrappers/opcua/TypeRegistry.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>

#include <open62541/types_generated.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>

// Regression test for enum-backed simulation contexts.
//
// The important behavior is not only that enum aliases parse, but that the
// OPC-UA projection keeps enum-backed members on the enum UA_DataType instead
// of collapsing them to primitive BYTE/USINT members.

static const char* kSimulationSchema = R"(
TYPE "Status" : USInt #ENUM(ONE=0, TWO=1, THREE=2, FOUR=3) END_TYPE
TYPE "LineState" : Int #ENUM(IDLE=0, STARTING=1, RUNNING=2, STOPPED=3) END_TYPE
TYPE "Stage"
  VAR
    status : "Status";
    state : "LineState";
    counter : UInt;
  END_VAR
END_TYPE

DATA_BLOCK "Simulation" DB1
VAR
    status : "Status";
    stage  : "Stage";
END_VAR
END_DATA_BLOCK
)";

int main() {
    sgrn::scl::PlcSchemaStore store;
    auto loaded = store.loadSchema(kSimulationSchema);
    assert(loaded.has_value());

    const auto& udts = store.udts();
    auto status_alias = std::find_if(udts.begin(), udts.end(), [](const auto& udt) { return udt.name == "Status"; });
    auto line_state_alias = std::find_if(udts.begin(), udts.end(), [](const auto& udt) { return udt.name == "LineState"; });
    auto stage_udt = std::find_if(udts.begin(), udts.end(), [](const auto& udt) { return udt.name == "Stage"; });
    if (status_alias == udts.end() || line_state_alias == udts.end() || stage_udt == udts.end())
        return 1;
    if (stage_udt->fields.size() != 3)
        return 1;

    assert(status_alias->is_scalar_alias);
    assert(status_alias->scalar_type == sgrn::scl::DataType::USInt);
    assert(status_alias->enum_map.size() == 4);
    assert(line_state_alias->is_scalar_alias);
    assert(line_state_alias->scalar_type == sgrn::scl::DataType::Int);
    assert(line_state_alias->enum_map.size() == 4);

    sgrn::gateway::wrappers::opcua::TypeRegistry registry;
    auto result = sgrn::gateway::adapters::populateTypeRegistryFromSchema(store, registry);
    assert(result.has_value());

    assert(registry.enumDefinitions().size() == 2);
    const UA_DataType* status_enum = registry.findEnumBySignature(sgrn::gateway::adapters::enumTypeSignature(
        sgrn::gateway::adapters::s7TypeToUaTypeIndex(sgrn::scl::DataType::USInt), status_alias->enum_map));
    const UA_DataType* line_state_enum = registry.findEnumBySignature(sgrn::gateway::adapters::enumTypeSignature(
        sgrn::gateway::adapters::s7TypeToUaTypeIndex(sgrn::scl::DataType::Int), line_state_alias->enum_map));
    if (!status_enum || !line_state_enum) {
        std::fprintf(stderr, "enum lookup failed: defs=%zu status=%p line=%p\n", registry.enumDefinitions().size(),
            static_cast<const void*>(status_enum), static_cast<const void*>(line_state_enum));
        for (const auto& def : registry.enumDefinitions()) {
            std::fprintf(stderr, "  enum %s sig=%s size=%zu\n", def.name.c_str(), def.signature.c_str(), def.values.size());
        }
        return 2;
    }
    if (status_enum->typeKind != UA_DATATYPEKIND_ENUM || line_state_enum->typeKind != UA_DATATYPEKIND_ENUM)
        return 3;
    if (status_enum->memSize != sizeof(UA_Int32) || line_state_enum->memSize != sizeof(UA_Int32))
        return 4;

    const UA_DataType* stage_type = registry.find("Stage");
    if (!stage_type)
        return 5;
    if (stage_type->typeKind != UA_DATATYPEKIND_STRUCTURE || stage_type->membersSize != 3)
        return 6;
    if (stage_type->members[0].memberType != status_enum || stage_type->members[1].memberType != line_state_enum ||
        stage_type->members[2].memberType != &UA_TYPES[UA_TYPES_UINT16])
        return 7;

    return 0;
}
