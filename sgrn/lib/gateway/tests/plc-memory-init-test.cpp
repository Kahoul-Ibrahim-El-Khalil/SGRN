// Coverage for the store-bound `:=` init-seeding path in PlcMemory.
//
// Drives the completely public load path — PlcSchemaStore::addDb() with a
// hand-built DbSchema, then PlcMemory::loadRegistry() — and verifies the
// seeded bytes through the same Tier-2 readDbMemory() the S7/protocol
// adapters use. applyFieldInit/applyDbFieldInits themselves are static;
// what is covered is their observable effect on DB memory. No network, no
// live PLC.
//
// Pinned semantics (after fixing the three defects this test found):
//   - numeric literals (decimal, negative) big-endian per field endianness
//   - quoted string literals ('AB'/strip) into [max,cur] tiles
//   - Bool TRUE/FALSE/1/0 via read-modify-write writeBit — Bools sharing a
//     byte all survive (each previously overwrote the whole byte: last
//     field processed won, FALSE wiped the byte)
//   - struct child initializers apply (previously dead: the parent's empty
//     init_value early return blocked the recursion), and children land at
//     parent base + child.offset (previously child.offset was used as an
//     absolute DB offset — a struct at non-zero offset seeded/clobbered the
//     wrong address)
//   - enum labels resolved case-insensitively via DbField::enum_map
//   - fields without initializers stay zeroed
//   - array initializers (`:= 9`) are intentionally skipped (documented:
//     single-scalar initializers only today)

#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/PlcState.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/types/DbSchema.hpp>

#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

namespace
{

int g_failures = 0;

namespace scl = sgrn::scl;
namespace twin = sgrn::gateway::twin;

#define CHECK(cond)                                                                                                                        \
    do {                                                                                                                                   \
        if (!(cond)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                                    \
        }                                                                                                                                  \
    } while (0)

scl::DbField mkField(const char* t_name, scl::DataType t_type, int t_offset, const char* t_init = nullptr) {
    scl::DbField f;
    f.name = t_name;
    f.type = t_type;
    f.offset = t_offset;
    f.endianness = s7codec::Endian::Big;
    if (t_init)
        f.init_value = t_init;
    return f;
}

// Read `t_size` bytes of DB `t_db` starting at `t_offset`; empty vector on error.
std::vector<uint8_t> seeded(const twin::PlcMemory& t_mem, uint16_t t_db, size_t t_offset, size_t t_size) {
    std::vector<uint8_t> buf(t_size, 0xEE);
    auto res = t_mem.readDbMemory(t_db, t_offset, t_size, buf.data());
    if (res.has_value())
        return buf;
    std::printf("readDbMemory(%u, %zu, %zu) failed\n", t_db, t_offset, t_size);
    return {};
}

} // namespace

int main() {
    using scl::DataType;

    // ── DB 101: scalars, bools, string, enums, skip-arrays ───────────────────
    scl::DbSchema schema;
    schema.db_number = 101;
    schema.db_name = "InitDb";
    schema.size_bytes = 40;
    schema.endianness = s7codec::Endian::Big;

    // Three Bools share byte 0: each must survive (read-modify-write).
    {
        scl::DbField b = mkField("bit0", DataType::Bool, 0, "TRUE");
        b.bit_index = 0;
        schema.fields.push_back(b);
        scl::DbField b2 = mkField("bit2", DataType::Bool, 0, "1");
        b2.bit_index = 2;
        schema.fields.push_back(b2);
        scl::DbField b3 = mkField("bit3", DataType::Bool, 0, "FALSE");
        b3.bit_index = 3;
        schema.fields.push_back(b3);
    }
    schema.fields.push_back(mkField("speed", DataType::Int, 2, "-1234"));
    schema.fields.push_back(mkField("w500", DataType::UInt, 4, "500"));
    schema.fields.push_back(mkField("temp", DataType::Real, 6, "45.5"));
    {
        scl::DbField s = mkField("s8", DataType::String, 10, "'AB'");
        s.count = 1;
        s.string_capacity = 8;
        s.struct_size = 10; // parser shape: typeSpanBytes(String, 8) == 10
        schema.fields.push_back(s);
    }
    {
        scl::DbField e = mkField("mode", DataType::UInt, 24, "ON");
        e.enum_map = {{0, "OFF"}, {1, "ON"}, {2, "STOP"}};
        schema.fields.push_back(e);
        scl::DbField e2 = mkField("mode_lo", DataType::UInt, 26, "off");
        e2.enum_map = {{0, "OFF"}, {1, "ON"}, {2, "STOP"}};
        schema.fields.push_back(e2);
    }
    schema.fields.push_back(mkField("untouched", DataType::Int, 28)); // no init
    {
        scl::DbField arr = mkField("arr", DataType::UInt, 30, "9");
        arr.count = 4; // array initializer — intentionally skipped
        schema.fields.push_back(arr);
    }

    // ── DB 102: struct-at-offset-0 child initializer ─────────────────────────
    scl::DbSchema schema2;
    schema2.db_number = 102;
    schema2.db_name = "StructDb";
    schema2.size_bytes = 8;
    schema2.endianness = s7codec::Endian::Big;
    {
        scl::DbField st;
        st.name = "st";
        st.type = DataType::Struct;
        st.offset = 0;
        st.struct_size = 6;
        st.count = 1;
        st.endianness = s7codec::Endian::Big;
        st.children.push_back(mkField("a", DataType::Int, 0, "7"));
        st.children.push_back(mkField("b", DataType::Real, 2, "1.5"));
        schema2.fields.push_back(st);
    }

    // ── DB 103: struct at NON-zero offset (base-offset propagation pin; the
    // child previously landed at child.offset counted from DB start) ─────────
    scl::DbSchema schema3;
    schema3.db_number = 103;
    schema3.db_name = "NestedDb";
    schema3.size_bytes = 10;
    schema3.endianness = s7codec::Endian::Big;
    {
        schema3.fields.push_back(mkField("pre", DataType::Int, 0)); // no init — must not be clobbered
        scl::DbField st;
        st.name = "st";
        st.type = DataType::Struct;
        st.offset = 4; // non-zero struct base: children are relative to THIS
        st.struct_size = 4;
        st.count = 1;
        st.endianness = s7codec::Endian::Big;
        st.children.push_back(mkField("x", DataType::Int, 2, "5")); // absolute 4 + 2
        schema3.fields.push_back(st);
    }

    scl::PlcSchemaStore store;
    CHECK(store.addDb(std::move(schema)).has_value());
    CHECK(store.addDb(std::move(schema2)).has_value());
    CHECK(store.addDb(std::move(schema3)).has_value());

    twin::PlcState state;
    twin::PlcMemory mem;
    mem.attachState(state);
    auto load = mem.loadRegistry(store);
    CHECK(load.has_value());

    // ── Numeric + bool byte ──────────────────────────────────────────────────
    const auto num = seeded(mem, 101, 0, 8);
    if (!num.empty()) {
        // All three Bools share byte 0 and all survive: bit0 (TRUE) + bit2 ("1")
        // set, bit3 (FALSE) clear → 0b0000_0101.
        CHECK(num[0] == 0x05);
        CHECK(num[1] == 0x00);
        CHECK(num[2] == 0xFB && num[3] == 0x2E);                                     // -1234 BE int16
        CHECK(num[4] == 0x01 && num[5] == 0xF4);                                     // 500 BE uint16
        CHECK(num[6] == 0x42 && num[7] == 0x36 && num[8] == 0x00 && num[9] == 0x00); // 45.5f BE
    }

    // ── String tile: capacity from struct_size (parser shape) ────────────────
    const auto str = seeded(mem, 101, 10, 12);
    if (!str.empty()) {
        CHECK(str[0] == 10 && str[1] == 2); // [max_len=10 (struct_size), cur=2]
        CHECK(str[2] == 'A' && str[3] == 'B');
        CHECK(str[4] == 0x00 && str[11] == 0x00); // zeroed tail
    }

    // ── Enum label resolution (exact + case-insensitive) ─────────────────────
    const auto en = seeded(mem, 101, 24, 4);
    if (!en.empty()) {
        CHECK(en[0] == 0x00 && en[1] == 0x01); // "ON" → 1
        CHECK(en[2] == 0x00 && en[3] == 0x00); // "off" → 0 (case-insensitive)
    }

    // ── No init + array-init skip ────────────────────────────────────────────
    const auto rest = seeded(mem, 101, 28, 12);
    if (!rest.empty()) {
        CHECK(rest[0] == 0x00 && rest[1] == 0x00); // no initializer → stays zero
        // array initializer skipped → whole 4×UInt stays zero
        bool all_zero = true;
        for (size_t i = 2; i < rest.size(); ++i)
            all_zero = all_zero && rest[i] == 0x00;
        CHECK(all_zero);
    }

    // ── Struct child inits: struct at offset 0 and at offset 4 ──────────────
    const auto st = seeded(mem, 102, 0, 6);
    if (!st.empty()) {
        CHECK(st[0] == 0x00 && st[1] == 0x07);                                   // Int child 7 BE
        CHECK(st[2] == 0x3F && st[3] == 0xC0 && st[4] == 0x00 && st[5] == 0x00); // Real 1.5 BE
    }

    const auto st2 = seeded(mem, 103, 0, 10);
    if (!st2.empty()) {
        CHECK(st2[0] == 0x00 && st2[1] == 0x00); // "pre" untouched (no clobber at old buggy address 2)
        CHECK(st2[4] == 0x00 && st2[5] == 0x00);
        CHECK(st2[6] == 0x00 && st2[7] == 0x05); // child at base 4 + rel 2 → 5 BE
    }

    std::printf("[PASS] PlcMemory init seeding: bools (shared-byte RMW), numerics, string tile, enums, skip-arrays, struct children incl. "
                "non-zero base\n");

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("All tests passed successfully!\n");
    return 0;
}
