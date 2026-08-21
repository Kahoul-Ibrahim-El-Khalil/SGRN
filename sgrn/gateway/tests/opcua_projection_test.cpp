#include <sgrn/gateway/adapters/opcua/schema_type_registry.hpp>
#include <sgrn/gateway/wrappers/opcua/TypeRegistry.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>

#include <open62541/types_generated.h>

#include <cassert>
#include <string>

// OPC UA projection regression test for first-class scalar-derived SCL types:
//  - scalar-alias UDTs must NOT become empty UA_DATATYPEKIND_STRUCTURE entries
//  - fields typed by an alias must project as their exact underlying scalar UA type
//    (the substitution happens in scl::resolveFields at parse time)

static const char* kSchema = R"(
TYPE "MotorState" : Int #ENUM(Off=0, On=1) END_TYPE
TYPE "SpeedAlias" : DWord #ENUM(Slow=0, Fast=1) END_TYPE
TYPE "BigAlias"   : LWord END_TYPE
TYPE "Valve"
  VAR
    state    : "MotorState";
    speed    : "SpeedAlias";
    big      : "BigAlias";
    position : Real;
  END_VAR
END_TYPE
)";

int main() {
    sgrn::scl::PlcSchemaStore store;
    auto loaded = store.loadSchema(kSchema);
    assert(loaded.has_value());

    sgrn::gateway::wrappers::opcua::TypeRegistry registry;
    auto result = sgrn::gateway::adapters::populateTypeRegistryFromSchema(store, registry);
    assert(result.has_value());

    // Only the non-alias structure UDT is registered.
    assert(registry.types().size() == 1);
    const UA_DataType& valve = registry.types().front();
    assert(std::string(valve.typeName) == "Valve");
    assert(valve.typeKind == UA_DATATYPEKIND_STRUCTURE);

    // The scalar aliases are NOT projected as empty structures.
    assert(registry.find("MotorState") == nullptr);
    assert(registry.find("SpeedAlias") == nullptr);
    assert(registry.find("BigAlias") == nullptr);

    // Fields of alias type project as the exact underlying scalar UA type.
    assert(valve.membersSize == 4);
    assert(valve.members[0].memberType == &UA_TYPES[UA_TYPES_INT16]);  // MotorState : Int
    assert(valve.members[1].memberType == &UA_TYPES[UA_TYPES_UINT32]); // SpeedAlias : DWord
    assert(valve.members[2].memberType == &UA_TYPES[UA_TYPES_UINT64]); // BigAlias : LWord
    assert(valve.members[3].memberType == &UA_TYPES[UA_TYPES_FLOAT]);  // position : Real

    // None of the alias fields is treated as a custom struct reference or falls
    // back to the adapter's default BYTE type.
    for (size_t i = 0; i < valve.membersSize; ++i) {
        assert(valve.members[i].memberType->typeKind != UA_DATATYPEKIND_STRUCTURE);
        assert(valve.members[i].memberType != &UA_TYPES[UA_TYPES_BYTE]);
    }

    return 0;
}
