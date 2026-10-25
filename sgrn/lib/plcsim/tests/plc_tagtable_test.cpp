// Coverage for plcsim's tag-table encode path (buildPutItems) and the local
// write path (commitLocalPut) — the store-bound code that turns JSON /
// DecodedValue tag writes into S7 wire bytes, with no network involved.
//
// The registry is loaded from a temp JSON file, exactly how Production
// loads it. The S7 addr of each tag is verified through describeTag; the
// encoded bytes through buildPutItems' out_bufs; the local shadow through
// read().
//
// Documented gap pinned as-is (not silently invented):
//   - String tags: the loader has no branch sizing string byte_count, so a
//     String tag registers with byte_count == 0 — blank shadow, and every
//     write refuses (encode into an empty buffer). Sizing string tags
//     needs a registry-format decision (e.g. a `len` field) before this
//     path can pass.

#include <sgrn/plcsim/PlcTagTable.hpp>

#include <snap7.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

using sgrn::wrappers::s7::S7DataItem;

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

const char* k_registry_json = R"JSON({
  "motor_speed": { "type": "Real", "area": "DB", "db": 1, "start": 0 },
  "pump_run":    { "type": "Bool", "area": "DB", "db": 1, "start": 4, "bit": 2 },
  "counter":     { "type": "DInt", "area": "DB", "db": 1, "start": 6 },
  "byte_tag":    { "type": "Byte", "area": "MK", "start": 3 },
  "desc":        { "type": "String", "area": "DB", "db": 1, "start": 10 }
})JSON";

} // namespace

int main() {
    // Materialize the registry the same way production does: a JSON file.
    std::filesystem::path reg = std::filesystem::temp_directory_path() / "sgrn_tagtable_test_registry.json";
    {
        std::ofstream f(reg);
        f << k_registry_json;
    }
    sgrn::plcsim::PlcTagTable table(reg.string());
    std::filesystem::remove(reg);

    using sgrn::plcsim::PlcTagTable;

    // ── Load + describe ──────────────────────────────────────────────────────
    CHECK(table.tagCount() == 5);
    const auto d = table.describeTag("motor_speed");
    CHECK(d.has_value());
    CHECK(d.value().type_name == "Real");
    CHECK(d.value().addr.db_number == 1);
    CHECK(d.value().addr.byte_count == 4);
    CHECK(d.value().addr.area == S7AreaDB);
    CHECK(d.value().addr.word_len == S7WLReal);

    const auto dbit = table.describeTag("pump_run");
    CHECK(dbit.has_value());
    CHECK(dbit.value().addr.bit_offset == 2);
    CHECK(dbit.value().type_name == "Bool");

    CHECK(table.describeTag("nope").hasError());

    // ── buildPutItems: string-JSON variant (encodeValue_ / twins encodeFieldAt) ─
    {
        std::vector<S7DataItem> items;
        std::vector<std::vector<uint8_t>> bufs;
        const auto res = table.buildPutItems({"motor_speed"}, {std::string("3.14")}, items, bufs);
        CHECK(res.has_value());
        CHECK(items.size() == 1 && bufs.size() == 1);
        CHECK(items[0].Area == S7AreaDB && items[0].DBNumber == 1 && items[0].Start == 0 && items[0].Amount == 4);
        CHECK(items[0].WordLen == S7WLReal);
        // 3.14 big-endian float
        CHECK(bufs[0].size() == 4);
        CHECK(bufs[0][0] == 0x40 && bufs[0][1] == 0x48 && bufs[0][2] == 0xF5 && bufs[0][3] == 0xC3);
    }

    // ── buildPutItems: DecodedValue variant — Bool honours the tag's bit ─────
    {
        std::vector<S7DataItem> items;
        std::vector<std::vector<uint8_t>> bufs;
        const auto dv = s7codec::DecodedValue::makeSigned(1);
        const auto res = table.buildPutItems({"pump_run"}, {dv}, items, bufs);
        CHECK(res.has_value());
        // Bit items address bit 4*8 + 2 = 34
        CHECK(items[0].Start == 34 && items[0].Amount == 1 && items[0].WordLen == S7WLBit);
        CHECK(bufs[0].size() == 1);
        CHECK(bufs[0][0] == 0x04); // bit 2 of byte 0
    }

    // ── buildPutItems: DecodedValue DInt big-endian ──────────────────────────
    {
        std::vector<S7DataItem> items;
        std::vector<std::vector<uint8_t>> bufs;
        const auto res = table.buildPutItems({"counter"}, {s7codec::DecodedValue::makeSigned(-7)}, items, bufs);
        CHECK(res.has_value());
        CHECK(bufs[0].size() == 4);
        CHECK(bufs[0][0] == 0xFF && bufs[0][1] == 0xFF && bufs[0][2] == 0xFF && bufs[0][3] == 0xF9);
    }

    // ── buildPutItems: unknown tag refused ───────────────────────────────────
    {
        std::vector<S7DataItem> items;
        std::vector<std::vector<uint8_t>> bufs;
        const auto res = table.buildPutItems({"ghost"}, {std::string("1")}, items, bufs);
        CHECK(res.hasError());
    }

    // ── commitLocalPut + read: local shadow without network ──────────────────
    {
        const auto res = table.commitLocalPut({"motor_speed"}, {std::string("45.5")});
        CHECK(res.has_value());
        const auto val = table.read("motor_speed");
        CHECK(val.has_value() && val.value() == "45.5");
    }
    {
        // Byte tag into the MK area — byte_count == 1 sizing from the mapping.
        std::vector<S7DataItem> items;
        std::vector<std::vector<uint8_t>> bufs;
        const auto res = table.buildPutItems({"byte_tag"}, {std::string("7")}, items, bufs);
        CHECK(res.has_value());
        CHECK(bufs[0].size() == 1 && bufs[0][0] == 0x07);
        CHECK(items[0].Area == S7AreaMK && items[0].WordLen == S7WLByte && items[0].Amount == 1);
    }

    // ── Documented gap: String tags register with byte_count == 0 and refuse ─
    {
        const auto dstr = table.describeTag("desc");
        CHECK(dstr.has_value());
        CHECK(dstr.value().addr.byte_count == 0); // loader has no string sizing branch
        CHECK(dstr.value().type_name == "String");

        std::vector<S7DataItem> items;
        std::vector<std::vector<uint8_t>> bufs;
        const auto res = table.buildPutItems({"desc"}, {std::string("\"hi\"")}, items, bufs);
        CHECK(res.hasError()); // encode into an empty buffer refuses — by construction today

        const auto local = table.commitLocalPut({"desc"}, {std::string("\"hi\"")});
        CHECK(local.hasError());
    }

    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("All tests passed successfully!\n");
    return 0;
}
