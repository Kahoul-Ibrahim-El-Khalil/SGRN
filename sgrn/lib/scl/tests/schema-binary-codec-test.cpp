// Binary schema codec round-trip tests (SchemaSerializer::serializeBinary /
// deserializeBinary, PlcSchemaStore::toBinary / loadFromBinary).
//
// Builds a store exercising every encoded member (nested children, all
// scalar widths, strings, temporals, enum maps, units, min/max, init
// values, array bounds, endianness, flags, UDTs incl. scalar aliases, and
// fully-addressed tags), then asserts a lossless round-trip: identical
// canonical JSON plus byte-exact spot checks of the members the JSON form
// drops (bounds, init values, alias detail, tag addresses). Malformed
// payloads must fail closed.

#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/schema/SchemaSerializer.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{

int g_failures = 0;

#define CHECK(cond)                                                                                                                        \
    do {                                                                                                                                   \
        if (!(cond)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                                    \
        }                                                                                                                                  \
    } while (0)

using sgrn::scl::DbField;
using sgrn::scl::DbSchema;
using sgrn::scl::PlcSchemaStore;
using sgrn::scl::PlcTag;
using sgrn::scl::UdtDefinition;
using DataType = sgrn::scl::DataType;

DbField makeField(const std::string& t_name, DataType t_type, int32_t t_offset = 0) {
    DbField f;
    f.name = t_name;
    f.type = t_type;
    f.offset = t_offset;
    f.count = 1;
    return f;
}

PlcSchemaStore buildRichStore() {
    PlcSchemaStore store;

    DbSchema db;
    db.db_number = 10;
    db.db_name = "ReactorCore";
    db.size_bytes = 256;
    db.max_depth = 3;
    db.source_file = "reactor.scl";
    db.endianness = s7codec::Endian::Little;
    db.trigger_events = true;
    db.modbus_area = sgrn::scl::ModbusArea::Holding;

    DbField run = makeField("run", DataType::Bool, 0);
    run.bit_index = 3;
    db.fields.push_back(run);

    DbField temps = makeField("temps", DataType::Int, 2);
    temps.count = 4;
    temps.array_lower_bound = 1;
    temps.array_upper_bound = 4;
    db.fields.push_back(temps);

    DbField label = makeField("label", DataType::String, 10);
    label.string_capacity = 32;
    label.struct_size = 34;
    db.fields.push_back(label);

    DbField names = makeField("names", DataType::WString, 44);
    names.count = 2;
    names.string_capacity = 16;
    names.struct_size = 36;
    db.fields.push_back(names);

    DbField press = makeField("pressure", DataType::Real, 116);
    press.unit = "bar";
    press.dimension = "pressure";
    press.description = "Reactor pressure";
    press.precision = 1;
    press.nominal = 2700.0;
    press.alarm_lo = 2500.0;
    press.alarm_hi = 3000.0;
    press.min_val = 0.0;
    press.max_val = 16.5;
    press.init_value = "1.013";
    db.fields.push_back(press);

    DbField mode = makeField("mode", DataType::DInt, 120);
    mode.enum_map = {{0, "Off"}, {1, "On"}, {2, "Auto"}};
    mode.is_dynamic = true;
    mode.is_label = true;
    mode.is_transient = true;
    mode.is_read_only = true;
    db.fields.push_back(mode);

    DbField stamp = makeField("stamp", DataType::DateTime, 124);
    stamp.endianness = s7codec::Endian::Little;
    db.fields.push_back(stamp);

    DbField rod;
    rod.name = "rod";
    rod.type = DataType::Struct;
    rod.offset = 132;
    rod.struct_size = 16;
    rod.udt_name = "RodUdt";
    DbField pos = makeField("position", DataType::LReal, 0);
    DbField sub;
    sub.name = "sub";
    sub.type = DataType::Struct;
    sub.offset = 8;
    sub.struct_size = 8;
    DbField flag = makeField("flag", DataType::Byte, 0);
    sub.children.push_back(flag);
    rod.children.push_back(pos);
    rod.children.push_back(sub);
    db.fields.push_back(rod);

    if (store.addDb(std::move(db)).hasError()) {
        ++g_failures;
        std::printf("FAIL addDb failed\n");
    }

    UdtDefinition udt;
    udt.udt_number = 1;
    udt.name = "RodUdt";
    udt.size_bytes = 16;
    udt.fields.push_back(makeField("position", DataType::LReal, 0));
    if (store.addUdt(std::move(udt)).hasError()) {
        ++g_failures;
        std::printf("FAIL addUdt failed\n");
    }

    UdtDefinition alias;
    alias.udt_number = 2;
    alias.name = "TempAlias";
    alias.is_scalar_alias = true;
    alias.scalar_type = DataType::Real;
    alias.enum_map = {{0, "Cold"}, {1, "Hot"}};
    alias.unit = "degC";
    alias.dimension = "temperature";
    alias.description = "Temperature alias";
    alias.precision = 1;
    alias.nominal = 20.0;
    alias.alarm_lo = -40.0;
    alias.alarm_hi = 120.0;
    alias.min_val = -40.0;
    alias.max_val = 120.0;
    if (store.addUdt(std::move(alias)).hasError()) {
        ++g_failures;
        std::printf("FAIL addUdt(alias) failed\n");
    }

    PlcTag tag;
    tag.name = "StartButton";
    tag.table_name = "Inputs";
    tag.type_str = "Bool";
    tag.remark = "panel";
    tag.addr.area = 0x81;
    tag.addr.db_number = 0;
    tag.addr.byte_offset = 0;
    tag.addr.bit_index = 0;
    tag.addr.word_len = 1;
    tag.addr.byte_count = 1;
    tag.addr.label = "I0.0";
    tag.type = DataType::Bool;
    if (store.addTag(std::move(tag)).hasError()) {
        ++g_failures;
        std::printf("FAIL addTag failed\n");
    }

    return store;
}

const DbField* findField(const std::vector<DbField>& t_fields, const std::string& t_name) {
    for (const auto& f : t_fields) {
        if (f.name == t_name)
            return &f;
    }
    return nullptr;
}

} // namespace

int main() {
    const PlcSchemaStore store = buildRichStore();
    if (g_failures != 0)
        return 1;

    auto bin_res = store.toBinary();
    CHECK(!bin_res.hasError());
    if (bin_res.hasError())
        return 1;
    const std::string bytes = std::move(bin_res).value();
    CHECK(bytes.size() >= 4 && bytes[0] == 'S' && bytes[1] == 'G' && bytes[2] == 'R' && bytes[3] == 'S');
    std::printf("info: binary schema %zu bytes vs JSON %zu bytes\n", bytes.size(), store.toJson().size());

    auto load_res = PlcSchemaStore::loadFromBinary(bytes);
    CHECK(!load_res.hasError());
    if (load_res.hasError())
        return 1;
    PlcSchemaStore back = std::move(load_res).value();

    // Canonical JSON equality (both sides drop the same members).
    CHECK(back.toJson() == store.toJson());

    // Lossless members the JSON form drops must survive exactly.
    {
        auto db_res = back.getDb(10);
        CHECK(!db_res.hasError());
        const DbSchema* db = db_res.hasError() ? nullptr : db_res.value();
        CHECK(db != nullptr);
        if (db) {
            CHECK(db->db_name == "ReactorCore");
            CHECK(db->source_file == "reactor.scl");
            CHECK(db->endianness == s7codec::Endian::Little);
            CHECK(db->trigger_events);
            CHECK(db->modbus_area == sgrn::scl::ModbusArea::Holding);
            const DbField* temps = findField(db->fields, "temps");
            CHECK(temps && temps->array_lower_bound == 1 && temps->array_upper_bound == 4 && temps->count == 4);
            const DbField* press = findField(db->fields, "pressure");
            CHECK(press && press->unit && *press->unit == "bar");
            CHECK(press && press->dimension && *press->dimension == "pressure");
            CHECK(press && press->min_val && *press->min_val == 0.0);
            CHECK(press && press->max_val && *press->max_val == 16.5);
            CHECK(press && press->init_value == "1.013");
            CHECK(press && press->description && *press->description == "Reactor pressure");
            CHECK(press && press->precision && *press->precision == 1);
            CHECK(press && press->nominal && *press->nominal == 2700.0);
            CHECK(press && press->alarm_lo && *press->alarm_lo == 2500.0);
            CHECK(press && press->alarm_hi && *press->alarm_hi == 3000.0);
            const DbField* mode = findField(db->fields, "mode");
            CHECK(mode && mode->enum_map.size() == 3 && mode->enum_map.at(2) == "Auto" && mode->is_dynamic);
            CHECK(mode && mode->is_label && mode->is_transient && mode->is_read_only);
            const DbField* rod = findField(db->fields, "rod");
            CHECK(rod && rod->udt_name == "RodUdt" && rod->children.size() == 2);
            CHECK(rod && rod->children[1].children.size() == 1 && rod->children[1].children[0].name == "flag");
            const DbField* stamp = findField(db->fields, "stamp");
            CHECK(stamp && stamp->endianness == s7codec::Endian::Little);
        }
        auto udt_res = back.getUdtByName("TempAlias");
        CHECK(!udt_res.hasError());
        if (!udt_res.hasError()) {
            const UdtDefinition* udt = udt_res.value();
            CHECK(udt->is_scalar_alias && udt->scalar_type == DataType::Real);
            CHECK(udt->enum_map.at(1) == "Hot" && udt->unit && *udt->unit == "degC");
            CHECK(udt->dimension && *udt->dimension == "temperature");
            CHECK(udt->description && *udt->description == "Temperature alias");
            CHECK(udt->precision && *udt->precision == 1);
            CHECK(udt->nominal && *udt->nominal == 20.0);
            CHECK(udt->alarm_lo && *udt->alarm_lo == -40.0);
            CHECK(udt->alarm_hi && *udt->alarm_hi == 120.0);
            CHECK(udt->min_val && *udt->min_val == -40.0 && udt->max_val && *udt->max_val == 120.0);
        }
        auto tag_res = back.getTag("StartButton");
        CHECK(!tag_res.hasError());
        if (!tag_res.hasError()) {
            const PlcTag* tag = tag_res.value();
            CHECK(tag->addr.area == 0x81 && tag->addr.byte_offset == 0 && tag->addr.bit_index == 0);
            CHECK(tag->addr.word_len == 1 && tag->addr.byte_count == 1 && tag->addr.label == "I0.0");
            CHECK(tag->type == DataType::Bool && tag->remark == "panel");
        }
    }

    // Empty store round-trips.
    {
        PlcSchemaStore empty;
        auto e_res = empty.toBinary();
        CHECK(!e_res.hasError());
        auto l_res = PlcSchemaStore::loadFromBinary(e_res.value());
        CHECK(!l_res.hasError());
        if (!l_res.hasError())
            CHECK(l_res->toJson() == empty.toJson());
    }

    // Malformed payloads fail closed.
    CHECK(PlcSchemaStore::loadFromBinary("").hasError());
    CHECK(PlcSchemaStore::loadFromBinary("{not binary}").hasError());
    CHECK(PlcSchemaStore::loadFromBinary(bytes.substr(0, bytes.size() / 2)).hasError()); // truncated
    CHECK(PlcSchemaStore::loadFromBinary(bytes + "trailing").hasError());                // trailing garbage
    {
        std::string bad_ver = bytes;
        bad_ver[4] = 99; // codec_ver low byte
        bad_ver[5] = 0;
        CHECK(PlcSchemaStore::loadFromBinary(bad_ver).hasError());
    }
    {
        // Unknown resolved type name (length-preserving patch of the first
        // canonical "BOOL"; the free-form XML type_str next to it is opaque
        // metadata and intentionally unvalidated).
        std::string bad_type = bytes;
        const size_t at = bad_type.find("BOOL");
        CHECK(at != std::string::npos);
        if (at != std::string::npos) {
            bad_type.replace(at, 4, "ZZZZ");
            CHECK(PlcSchemaStore::loadFromBinary(bad_type).hasError());
        }
    }

    if (g_failures == 0)
        std::printf("schema_binary_codec_test: ALL CHECKS PASSED\n");
    else
        std::printf("schema_binary_codec_test: %d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
