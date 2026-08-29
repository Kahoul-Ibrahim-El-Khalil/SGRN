#include <sgrn/scl/schema/DbSymbolsParser.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>

#include <rapidjson/document.h>
#include <s7codec/s7.hpp>

#include <cassert>
#include <cstdint>
#include <string>
#include <vector>

using sgrn::Result;
using namespace sgrn::scl;

// ── Small test helpers ───────────────────────────────────────────────────────
static Result<ParseResult, SchemaError> parseIt(const std::string& src) {
    return DbSymbolsParser::parseString(src);
}

static const UdtDefinition& findUdt(const ParseResult& r, const std::string& name) {
    for (const auto& udt : r.udts) {
        if (udt.name == name)
            return udt;
    }
    assert(false && "UDT not found");
}

static const DbSchema& findDb(const ParseResult& r, const std::string& name) {
    for (const auto& db : r.dbs) {
        if (db.db_name == name)
            return db;
    }
    assert(false && "DB not found");
}

static const DbField* findField(const std::vector<DbField>& fields, const std::string& name) {
    for (const auto& f : fields) {
        if (f.name == name)
            return &f;
    }
    return nullptr;
}

// ── Schema under test ────────────────────────────────────────────────────────
static const char* kScalarSchema = R"(
TYPE "MotorState" : Int #ENUM(Off=0, On=1, Fault=2) #UNIT("state") #RANGE(0, 2) END_TYPE
TYPE "SpeedAlias" : UInt #UNIT("rpm") END_TYPE
DATA_BLOCK "Pump" DB1
VAR
    st : "MotorState";
    sp : "SpeedAlias";
    ov : "MotorState" #UNIT("overrideUnit") #ENUM(Custom=5) #RANGE(0, 100);
    arr : ARRAY[1..4] OF "MotorState";
END_VAR
END_DATA_BLOCK
)";

static const char* kNativeEnumSchema = R"(
TYPE "PlantMode"
VERSION : 0.1
   (STOPPED := 0,
    STARTING := 1,
    RUNNING := 2);
END_TYPE
)";

static const char* kNativeEnumGapSchema = R"(
TYPE "PlantMode"
VERSION : 0.1
   (STOPPED := 0,
    STARTING := 1,
    RUNNING := 2,
    FAULT := 4,
    STOP := 5,
    UKNOWN);
END_TYPE
)";

int main() {
    // 1. Scalar alias parsing
    auto p = parseIt(kScalarSchema);
    if (!p.has_value()) {
        fmt::print(stderr, "Parse error: {}\n", sgrn::scl::toString(p.error()));
        return 1;
    }
    const auto& motor = findUdt(p.value(), "MotorState");
    assert(motor.is_scalar_alias);
    assert(motor.scalar_type == DataType::Int);
    assert(motor.enum_map.size() == 3);
    assert(motor.enum_map.at(0) == "Off");
    assert(motor.enum_map.at(1) == "On");
    assert(motor.enum_map.at(2) == "Fault");
    assert(motor.unit.has_value() && *motor.unit == "state");
    assert(motor.min_val.has_value() && *motor.min_val == 0.0);
    assert(motor.max_val.has_value() && *motor.max_val == 2.0);

    const auto& speed = findUdt(p.value(), "SpeedAlias");
    assert(speed.is_scalar_alias);
    assert(speed.scalar_type == DataType::UInt);
    assert(speed.unit.has_value() && *speed.unit == "rpm");

    // 1b. Native TIA enum syntax parses to the same enum map as #ENUM.
    auto native_enum = parseIt(kNativeEnumSchema);
    assert(native_enum.has_value());
    const auto& native_plant_mode = findUdt(native_enum.value(), "PlantMode");
    assert(native_plant_mode.is_scalar_alias);
    assert(native_plant_mode.scalar_type == DataType::Int);
    assert(native_plant_mode.enum_map.size() == 3);
    assert(native_plant_mode.enum_map.at(0) == "STOPPED");
    assert(native_plant_mode.enum_map.at(1) == "STARTING");
    assert(native_plant_mode.enum_map.at(2) == "RUNNING");

    auto alias_enum = parseIt(R"(
TYPE "PlantMode" : Int #ENUM(STOPPED=0, STARTING=1, RUNNING=2) END_TYPE
)");
    assert(alias_enum.has_value());
    const auto& alias_plant_mode = findUdt(alias_enum.value(), "PlantMode");
    assert(alias_plant_mode.enum_map == native_plant_mode.enum_map);

    // 1c. Native enum auto-numbering continues after explicit gaps.
    auto gap_enum = parseIt(kNativeEnumGapSchema);
    assert(gap_enum.has_value());
    const auto& gap_plant_mode = findUdt(gap_enum.value(), "PlantMode");
    assert(gap_plant_mode.enum_map.at(6) == "UKNOWN");

    // 1d. Malformed native enum syntax must fail loudly.
    auto malformed_enum = parseIt(R"(
TYPE "Broken"
VERSION : 0.1
   (STOPPED := 0,
    STARTING := 1,
    RUNNING := 2;
END_TYPE
)");
    assert(!malformed_enum.has_value());

    auto empty_udt = parseIt(R"(
TYPE "Empty"
END_TYPE
)");
    assert(empty_udt.has_value());
    assert(!empty_udt.value().warnings.empty());

    // 2. Type matrix: every supported integer type parses as a scalar alias.
    struct TypeCase {
        const char* name;
        DataType dt;
        int size;
    };
    const TypeCase type_cases[] = {
        {"SIntAlias", DataType::SInt, 1},
        {"USIntAlias", DataType::USInt, 1},
        {"IntAlias", DataType::Int, 2},
        {"UIntAlias", DataType::UInt, 2},
        {"DIntAlias", DataType::DInt, 4},
        {"UDIntAlias", DataType::UDInt, 4},
        {"LIntAlias", DataType::LInt, 8},
        {"ULIntAlias", DataType::ULInt, 8},
        {"ByteAlias", DataType::Byte, 1},
        {"WordAlias", DataType::Word, 2},
        {"DWordAlias", DataType::DWord, 4},
        {"LWordAlias", DataType::LWord, 8},
    };
    for (const auto& tc : type_cases) {
        std::string scl = std::string("TYPE \"") + tc.name + "\" : " + s7codec::s7TypeToString(tc.dt) + " END_TYPE";
        auto p2 = parseIt(scl);
        assert(p2.has_value());
        const auto& ud = findUdt(p2.value(), tc.name);
        assert(ud.is_scalar_alias);
        assert(ud.scalar_type == tc.dt);
        assert(ud.size_bytes == tc.size);
    }

    // 3. resolveFields: semantic inheritance + field-level overrides
    auto pr = parseIt(R"(
TYPE "MotorState" : Int #ENUM(Off=0, On=1, Fault=2) #UNIT("state") #RANGE(0, 2) END_TYPE
TYPE "SpeedAlias" : UInt #UNIT("rpm") END_TYPE
DATA_BLOCK "Ctl" DB1
VAR
    st : "MotorState";
    sp : "SpeedAlias";
    ov : "MotorState" #UNIT("overrideUnit") #ENUM(Custom=5) #RANGE(0, 100);
    arr : ARRAY[1..4] OF "MotorState";
END_VAR
END_DATA_BLOCK
)");
    assert(pr.has_value());
    const auto& db = findDb(pr.value(), "Ctl");

    const auto* f_st = findField(db.fields, "st");
    assert(f_st != nullptr);
    assert(f_st->type == DataType::Int);       // underlying scalar, not Struct
    assert(kind_of(*f_st) == FieldKind::Enum); // reference fully substituted
    assert(f_st->enum_map == motor.enum_map);
    assert(f_st->unit.has_value() && *f_st->unit == "state");
    assert(f_st->min_val.has_value() && *f_st->min_val == 0.0);
    assert(f_st->max_val.has_value() && *f_st->max_val == 2.0);

    const auto* f_sp = findField(db.fields, "sp");
    assert(f_sp != nullptr);
    assert(f_sp->type == DataType::UInt);
    assert(f_sp->unit.has_value() && *f_sp->unit == "rpm");

    // field-level #UNIT/#RANGE/#ENUM overrides are preserved instead of inherited
    const auto* f_ov = findField(db.fields, "ov");
    assert(f_ov != nullptr);
    assert(f_ov->type == DataType::Int);
    assert(f_ov->enum_map.size() == 1 && f_ov->enum_map.count(5) && f_ov->enum_map.at(5) == "Custom");
    assert(f_ov->unit.has_value() && *f_ov->unit == "overrideUnit");
    assert(f_ov->min_val.has_value() && *f_ov->min_val == 0.0);
    assert(f_ov->max_val.has_value() && *f_ov->max_val == 100.0);

    // 4. ARRAY OF scalar-alias: element type/substitution + count preserved
    const auto* f_arr = findField(db.fields, "arr");
    assert(f_arr != nullptr);
    assert(f_arr->type == DataType::Int);
    assert(f_arr->count == 4);
    assert(f_arr->children.empty());
    assert(f_arr->struct_size == 2);
    assert(f_arr->enum_map == motor.enum_map);
    assert(db.size_bytes >= 2 * 4); // enough room for the array

    // 5. Nested UDT + nested STRUCT resolving an alias field
    auto pn = parseIt(R"(
        TYPE "MotorState" : Int #ENUM(Off=0, On=1, Fault=2) END_TYPE
        TYPE "Outer"
          VAR
            inner : STRUCT
              mode : "MotorState";
            END_STRUCT;
          END_VAR
        END_TYPE
)");
    assert(pn.has_value());
    const auto& outer = findUdt(pn.value(), "Outer");
    assert(outer.fields.size() == 1);
    const auto& inner_struct = outer.fields.at(0);
    assert(inner_struct.type == DataType::Struct);
    const auto* f_mode = findField(inner_struct.children, "mode");
    assert(f_mode != nullptr);
    assert(f_mode->type == DataType::Int);
    assert(f_mode->enum_map == motor.enum_map);

    // 6. Enum collision: later duplicate key wins
    auto pc = parseIt(R"(
        TYPE "Collide" : INT #ENUM(One=1, Duplicate=1) END_TYPE
)");
    assert(pc.has_value());
    const auto& col = findUdt(pc.value(), "Collide");
    assert(col.enum_map.size() == 1 && col.enum_map.count(1) && col.enum_map.at(1) == "Duplicate");

    // 7. s7codec::isInRange validation against the alias's underlying C++ type
    assert(s7codec::isInRange<int16_t>(20000) == false);
    assert(s7codec::isInRange<int16_t>(200) == true);
    assert(s7codec::isInRange<uint16_t>(100000) == false);
    assert(s7codec::isInRange<uint16_t>(5000) == true);

    // 8. Canonical JSON round-trip preserves alias metadata and resolved fields.
    PlcSchemaStore store;
    auto loaded = store.loadSchema(kScalarSchema);
    assert(loaded.has_value());
    const auto json_out = store.toJson();

    rapidjson::Document doc;
    doc.Parse(json_out.c_str());
    assert(!doc.HasParseError());

    PlcSchemaStore store2;
    auto loaded2 = PlcSchemaStore::loadFromJson(doc);
    assert(loaded2.has_value());
    store2 = loaded2.value();

    auto udt2 = store2.getUdtByName("MotorState");
    assert(udt2.has_value());
    assert((*udt2)->is_scalar_alias);
    assert((*udt2)->scalar_type == DataType::Int);
    assert((*udt2)->unit.has_value() && *(*udt2)->unit == "state");
    assert((*udt2)->enum_map == motor.enum_map);

    // The resolved DB field (scalar type + inherited metadata) survives the round-trip.
    auto db2_it = store2.dbs().find(1);
    assert(db2_it != store2.dbs().end());
    const auto& db2 = db2_it->second;
    const auto* f2 = findField(db2.fields, "st");
    assert(f2 != nullptr);
    assert(f2->type == DataType::Int);
    assert(kind_of(*f2) == FieldKind::Enum);
    assert(f2->enum_map == motor.enum_map);
    assert(f2->unit.has_value() && *f2->unit == "state");
    assert(f2->min_val.has_value() && *f2->min_val == 0.0);
    assert(f2->max_val.has_value() && *f2->max_val == 2.0);
    return 0;
}
