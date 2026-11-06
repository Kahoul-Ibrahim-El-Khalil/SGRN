#include <sgrn/gateway/adapters/opcua/schema-type-registry.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/wrappers/opcua/TypeRegistry.hpp>

#include <open62541/types_generated.h>

#include <cassert>
#include <cstdio>
#include <string>

// OPC UA projection regression test for first-class scalar-derived SCL types:
//  - scalar-alias UDTs must NOT become empty UA_DATATYPEKIND_STRUCTURE entries
//  - fields typed by an alias must project as their exact underlying scalar UA type
//    (the substitution happens in scl::resolveFields at parse time)
//  - scalar-alias UDTs carrying #ENUM additionally project as first-class
//    UA_DATATYPEKIND_ENUM types; member fields referencing them point at the
//    custom enum type (not the raw base scalar)
// The registry enumerates custom enum types before structure types, but the
// relative enum order is hash-based, so look up by name — never by index.

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
    (void)loaded;
    assert(loaded.has_value());

    sgrn::wrappers::opcua::TypeRegistry registry;
    auto result = sgrn::gateway::adapters::populateTypeRegistryFromSchema(store, registry);
    assert(result.has_value());

    // Two enum-backed aliases + the one structure UDT; the plain scalar alias
    // (BigAlias) is NOT registered at all.
    assert(registry.types().size() == 3);

    // The enum-backed aliases project as first-class UA enum types.
    const UA_DataType* motor_state = registry.find("MotorState");
    const UA_DataType* speed_alias = registry.find("SpeedAlias");
    assert(motor_state != nullptr && motor_state->typeKind == UA_DATATYPEKIND_ENUM);
    assert(speed_alias != nullptr && speed_alias->typeKind == UA_DATATYPEKIND_ENUM);

    // Only the non-alias structure UDT registers as a structure, by name.
    const UA_DataType* valve = registry.find("Valve");
    assert(valve != nullptr);
    assert(std::string(valve->typeName) == "Valve");
    assert(valve->typeKind == UA_DATATYPEKIND_STRUCTURE);

    // The plain scalar alias is NOT projected as an empty structure.
    assert(registry.find("BigAlias") == nullptr);

    // Enum-backed alias fields project as the custom enum type; remaining
    // fields project as the exact underlying scalar UA type.
    assert(valve->membersSize == 4);
    assert(std::string(valve->members[0].memberName) == "state");
    assert(valve->members[0].memberType == motor_state); // MotorState : Int
    assert(std::string(valve->members[1].memberName) == "speed");
    assert(valve->members[1].memberType == speed_alias); // SpeedAlias : DWord
    assert(std::string(valve->members[2].memberName) == "big");
    assert(valve->members[2].memberType == &UA_TYPES[UA_TYPES_UINT64]); // BigAlias : LWord
    assert(std::string(valve->members[3].memberName) == "position");
    assert(valve->members[3].memberType == &UA_TYPES[UA_TYPES_FLOAT]); // position : Real

    // None of the alias fields is treated as a custom struct reference or falls
    // back to the adapter's default BYTE type.
    for (size_t i = 0; i < valve->membersSize; ++i) {
        assert(valve->members[i].memberType->typeKind != UA_DATATYPEKIND_STRUCTURE);
        assert(valve->members[i].memberType != &UA_TYPES[UA_TYPES_BYTE]);
    }

    return 0;
}
