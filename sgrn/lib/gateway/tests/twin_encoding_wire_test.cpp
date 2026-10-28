// Wire-level regression tests for the encoder/decoder layer migrated to the
// s7codec span/options API.
//
// Encode side targets the production write chain:
//   gateway::twin::encodeFieldAt / applyJsonPatchToFields (src/twin/encoding.cpp)
//   — the exact functions behind PlcCommandProcessor, DbSnapshot, DbIOProvider,
//   PlcRuntime, WalReplayer and plcsim's tag table.
//
// Decode side targets the two production read renderers:
//   sgrn::scl::decodeFieldAt (snapshot/scl layer; string capacity consumed from
//   DbField::string_capacity) and S7SerializationUtils::serializeFieldTo
//   (S7 adapter decode hop; Byte/Char/Word hex-render, Time format).
//
// Assertions pin exact wire bytes and JSON renders so a signed cast, widened
// conversion or dropped clamp in the migration cannot hide behind "the
// round-trip still worked". Semantics pinned, now as the UNIFIED single
// implementation (scl owns it; twin delegates as of the copy unification):
//   - scalar String tiles: ENCODE capacity comes from string_capacity with a
//     count fallback; DECODE in scl consumes string_capacity, in the S7
//     adapter consumes count. The count-driven decode asymmetry is the
//     documented conflation, preserved.
//   - string arrays encode one tile per element (fieldElementSpanBytes).
//     The pre-unification twin fork refused them (primitiveSize(String) ==
//     1-byte window); the unified layer took the scl variant that matches
//     the decode layout.
//   - enum labels resolve inside encodeScalarValue against
//     DbField::enum_map (unknown labels refused).
//   - DTL byte[4] (weekday) is zone-dependent, never decoded — unasserted.
//
// Not covered here (store/state bound, exercised only by the live gateway
// suites): PlcMemory init-value seeding, ScriptDataBlock/SchemaVM.

#include <sgrn/common/S7SerializationUtils.hpp>
#include <sgrn/gateway/twin/encoding.hpp>
#include <sgrn/gateway/twin/path.hpp>
#include <sgrn/scl/utils.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

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

using s7codec::Endian;
using sgrn::scl::DataType;
using sgrn::scl::DbField;
using sgrn::scl::decodeFieldAt;
using sgrn::scl::fieldElementSpanBytes;
using sgrn::scl::fieldSpanSize;

namespace twin = sgrn::gateway::twin;

DbField mkField(const std::string& t_name, DataType t_type, int32_t t_offset = 0, uint32_t t_count = 1, uint32_t t_cap = 0) {
    DbField f;
    f.name = t_name;
    f.type = t_type;
    f.offset = t_offset;
    f.count = t_count;
    f.string_capacity = t_cap;
    return f;
}

// Encode via the production twin chain into a fresh zeroed span.
std::vector<uint8_t> encode(const DbField& t_field, const char* t_json, bool* t_ok = nullptr) {
    std::vector<uint8_t> buf(fieldSpanSize(t_field), 0);
    const auto r = twin::encodeFieldAt(t_field, t_json, buf.data(), buf.size(), 0, t_field.endianness);
    if (t_ok != nullptr)
        *t_ok = r.has_value();
    return buf;
}

// Encode into a CALLER-sized span, preserving preset bytes (bit packing).
bool encodeInto(const DbField& t_field, const char* t_json, std::vector<uint8_t>& t_buf) {
    return twin::encodeFieldAt(t_field, t_json, t_buf.data(), t_buf.size(), 0, t_field.endianness).has_value();
}

// Decode render via the scl snapshot layer (JSON, string_capacity driven).
// Calls .value(); only use on decodes known to succeed.
std::string render(const DbField& t_field, const std::vector<uint8_t>& t_buf) {
    return decodeFieldAt(t_field, t_buf.data(), t_buf.size(), 0, t_field.endianness).value();
}

// Decode render via the S7 adapter hop (count driven, Byte/Char/Word hex).
std::string renderAdapter(const DbField& t_field, const std::vector<uint8_t>& t_buf) {
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);
    sgrn::gateway::adapters::s7::serializeFieldTo(t_writer, t_field, t_buf.data(), t_buf.size());
    t_writer.Flush();
    return sb.GetString();
}

template <size_t N>
bool bytesEqual(const std::vector<uint8_t>& t_buf, const uint8_t (&t_expected)[N]) {
    return t_buf.size() == N && std::equal(t_buf.begin(), t_buf.end(), t_expected);
}

void scalarWire() {
    // BE Int — exact bytes plus round-trip.
    DbField fi = mkField("i", DataType::Int);
    bool ok = false;
    auto b = encode(fi, "42", &ok);
    CHECK(ok);
    CHECK(bytesEqual(b, {0x00, 0x2A}));
    CHECK(render(fi, b) == "42");

    // LITTLE_ENDIAN block — byte order must follow the field's block scope.
    DbField fil = mkField("il", DataType::Int);
    fil.endianness = Endian::Little;
    b = encode(fil, "-1234", &ok);
    CHECK(ok);
    CHECK(bytesEqual(b, {0x2E, 0xFB})); // -1234 = 0xFB2E little-endian
    CHECK(render(fil, b) == "-1234");

    // Real — IEEE 754 big-endian single.
    DbField fr = mkField("r", DataType::Real);
    b = encode(fr, "23.5", &ok);
    CHECK(ok);
    CHECK(bytesEqual(b, {0x41, 0xBC, 0x00, 0x00}));
    CHECK(render(fr, b) == "23.5");

    // LReal — IEEE 754 big-endian double.
    DbField fl = mkField("lr", DataType::LReal);
    b = encode(fl, "45.5", &ok);
    CHECK(ok);
    CHECK(bytesEqual(b, {0x40, 0x46, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00}));
    CHECK(render(fl, b) == "45.5");

    // DInt — 4-byte signed, two's complement on the wire.
    DbField fd = mkField("di", DataType::DInt);
    b = encode(fd, "-2", &ok);
    CHECK(ok);
    CHECK(bytesEqual(b, {0xFF, 0xFF, 0xFF, 0xFE}));
    CHECK(render(fd, b) == "-2");

    // UDInt.
    DbField fu = mkField("udi", DataType::UDInt);
    b = encode(fu, "305419896", &ok);
    CHECK(ok);
    CHECK(bytesEqual(b, {0x12, 0x34, 0x56, 0x78}));
    CHECK(render(fu, b) == "305419896");

    // Byte renders as a quoted hex string on both decode paths.
    DbField fby = mkField("by", DataType::Byte);
    b = encode(fby, "65", &ok);
    CHECK(ok);
    CHECK(bytesEqual(b, {0x41}));
    CHECK(render(fby, b) == "\"0x41\"");

    std::printf("[PASS] scalar wire bytes + round-trips (BE/LE, Real/LReal, DInt/UDInt/Byte)\n");
}

void adapterRenders() {
    // S7 adapter decode hop: Char renders as a character, Word as u16 hex.
    DbField fc = mkField("c", DataType::Char);
    std::vector<uint8_t> cb{0x41};
    CHECK(renderAdapter(fc, cb) == "\"A\"");

    DbField fw = mkField("wo", DataType::Word);
    fw.endianness = Endian::Little;
    std::vector<uint8_t> wb{0x34, 0x12};
    CHECK(renderAdapter(fw, wb) == "\"0x1234\""); // LE decoded, rendered big-endian hex label

    DbField ft = mkField("t", DataType::Time);
    std::vector<uint8_t> tb{0x02, 0xB3, 0x2C, 0x95}; // 45296789 ms
    CHECK(renderAdapter(ft, tb) == "\"#T12:34:56.789\"");
}

void boolBitPacking() {
    DbField fb = mkField("b", DataType::Bool);
    fb.bit_index = 3;

    // Set must not disturb the preset neighbor at bit 5 (read-modify-write).
    std::vector<uint8_t> buf{0x20};
    CHECK(encodeInto(fb, "true", buf));
    CHECK(buf[0] == 0x28);
    CHECK(render(fb, buf) == "true");

    // Clear must write exactly one bit low.
    buf = {0xFF};
    CHECK(encodeInto(fb, "false", buf));
    CHECK(buf[0] == 0xF7);
    CHECK(render(fb, buf) == "false");

    std::printf("[PASS] Bool bit_offset set/clear bit-exact\n");
}

void stringTiles() {
    // Parser shape (count=1, string_capacity=N): the encode side takes the
    // max_len from string_capacity; the tile span is 2 + capacity bytes.
    DbField fs = mkField("s", DataType::String, 0, 1, 8);
    std::vector<uint8_t> buf(10, 0);
    CHECK(encodeInto(fs, R"JSON("hi")JSON", buf));
    CHECK(bytesEqual(buf, {0x08, 0x02, 'h', 'i', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    CHECK(render(fs, buf) == R"JSON("hi")JSON");

    // Overflow past the capacity is refused.
    CHECK(!encodeInto(fs, R"JSON("123456789")JSON", buf));

    // Corrupt current-length header (200 > capacity) is clamped on decode.
    std::vector<uint8_t> corrupt{0x08, 0xC8, 'h', 'i', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(render(fs, corrupt) == R"JSON("hi")JSON");

    // WString: [max,cur] as BE u16 headers, BE code units, zeroed tail.
    DbField fw = mkField("w", DataType::WString, 0, 1, 4);
    std::vector<uint8_t> wbuf(12, 0);
    CHECK(encodeInto(fw, R"JSON("AB")JSON", wbuf));
    CHECK(bytesEqual(wbuf, {0x00, 0x04, 0x00, 0x02, 0x00, 0x41, 0x00, 0x42, 0x00, 0x00, 0x00, 0x00}));
    CHECK(render(fw, wbuf) == R"JSON("AB")JSON");

    // XString: u32 headers + raw bytes.
    DbField fx = mkField("x", DataType::XString, 0, 1, 8);
    std::vector<uint8_t> xbuf(16, 0);
    CHECK(encodeInto(fx, R"JSON("hi")JSON", xbuf));
    CHECK(bytesEqual(xbuf, {0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x02, 'h', 'i', 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
    CHECK(render(fx, xbuf) == R"JSON("hi")JSON");

    // XWString: u32 headers + BE code units.
    DbField fxw = mkField("xw", DataType::XWString, 0, 1, 4);
    std::vector<uint8_t> xwbuf(16, 0);
    CHECK(encodeInto(fxw, R"JSON("AB")JSON", xwbuf));
    CHECK(bytesEqual(xwbuf, {0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x41, 0x00, 0x42, 0x00, 0x00, 0x00, 0x00}));
    CHECK(render(fxw, xwbuf) == R"JSON("AB")JSON");

    // String array (count=2, capacity=4): one 6-byte tile per element via
    // fieldElementSpanBytes. The unified encoder layers both copies'
    // best behaviors: per-tile strings here, string_capacity-driven scalar
    // tiles, enum-label resolution.
    DbField fsa = mkField("sa", DataType::String, 0, 2, 4);
    bool ok2 = false;
    auto abuf = encode(fsa, R"JSON(["ab", "cd"])JSON", &ok2);
    CHECK(ok2);
    CHECK(abuf.size() == 12);
    CHECK(abuf[0] == 0x04 && abuf[1] == 0x02 && abuf[2] == 'a' && abuf[3] == 'b' && abuf[4] == 0x00 && abuf[5] == 0x00);
    CHECK(abuf[6] == 0x04 && abuf[7] == 0x02 && abuf[8] == 'c' && abuf[9] == 'd' && abuf[10] == 0x00 && abuf[11] == 0x00);
    CHECK(render(fsa, abuf) == R"JSON(["ab","cd"])JSON");

    std::printf("[PASS] String/WString/XString/XWString tiles, clamps, string array\n");
}

void temporals() {
    // DTL: year BE, month/day raw, hour/min/sec raw, ns BE. byte[4] is the
    // weekday computed by mktime (zone-dependent), so it is not asserted —
    // and the decoder never reads it, so the render is TZ-safe.
    DbField fl = mkField("dtl", DataType::DTL);
    bool ok = false;
    auto b = encode(fl, "\"2024-03-18 19:30:00.500000000\"", &ok);
    CHECK(ok);
    CHECK(b.size() == 12);
    CHECK(b[0] == 0x07 && b[1] == 0xE8);                                   // 2024
    CHECK(b[2] == 0x03 && b[3] == 0x12);                                   // March 18
    CHECK(b[5] == 0x13 && b[6] == 0x1E);                                   // 19:30
    CHECK(b[7] == 0x00);                                                   // :00 sec
    CHECK(b[8] == 0x1D && b[9] == 0xCD && b[10] == 0x65 && b[11] == 0x00); // 500000000 ns
    CHECK(render(fl, b) == "\"2024-03-18 19:30:00.500000000\"");

    // DTL with LITTLE_ENDIAN: year and nanoseconds must be in Little-Endian byte order.
    DbField fl_le = mkField("dtl_le", DataType::DTL);
    fl_le.endianness = Endian::Little;
    b = encode(fl_le, "\"2024-03-18 19:30:00.500000000\"", &ok);
    CHECK(ok);
    CHECK(b.size() == 12);
    CHECK(b[0] == 0xE8 && b[1] == 0x07);                                   // 2024 LE
    CHECK(b[2] == 0x03 && b[3] == 0x12);                                   // March 18
    CHECK(b[5] == 0x13 && b[6] == 0x1E);                                   // 19:30
    CHECK(b[7] == 0x00);                                                   // :00 sec
    CHECK(b[8] == 0x00 && b[9] == 0x65 && b[10] == 0xCD && b[11] == 0x1D); // 500000000 ns LE
    CHECK(render(fl_le, b) == "\"2024-03-18 19:30:00.500000000\"");

    // Symmetric byte swap verification on DTL
    ::sgrn::gateway::twin::swapFieldToBigEndian(fl_le, b.data(), b.size());
    CHECK(b[0] == 0x07 && b[1] == 0xE8);                                   // Year swapped to BE
    CHECK(b[8] == 0x1D && b[9] == 0xCD && b[10] == 0x65 && b[11] == 0x00); // Ns swapped to BE
    ::sgrn::gateway::twin::swapFieldToBigEndian(fl_le, b.data(), b.size());
    CHECK(b[0] == 0xE8 && b[1] == 0x07);                                   // Year swapped back to LE
    CHECK(b[8] == 0x00 && b[9] == 0x65 && b[10] == 0xCD && b[11] == 0x1D); // Ns swapped back to LE

    // Date = days since 1990-01-01, BE u16 on the wire; decode renders the
    // calendar date.
    DbField fda = mkField("d", DataType::Date);
    b = encode(fda, "0", &ok);
    CHECK(ok);
    CHECK(bytesEqual(b, {0x00, 0x00}));
    CHECK(render(fda, b) == "\"1990-01-01\"");
    b = encode(fda, "36", &ok);
    CHECK(ok);
    CHECK(bytesEqual(b, {0x00, 0x24}));
    CHECK(render(fda, b) == "\"1990-02-06\"");

    // Time: S7 32-bit millis (here 0x02B32C95 = 45296789), signed render.
    DbField fti = mkField("t", DataType::Time);
    b = encode(fti, "\"12:34:56.789\"", &ok);
    CHECK(ok);
    CHECK(b.size() == 4);
    CHECK(b[0] == 0x02 && b[1] == 0xB3 && b[2] == 0x2C && b[3] == 0x95);
    CHECK(render(fti, b) == "\"#T12:34:56.789\"");
    b = encode(fti, "\"-1:00:00.500\"", &ok);
    CHECK(ok);
    CHECK(render(fti, b) == "\"-#T01:00:00.500\"");

    // TimeOfDay: millis since midnight through the plain unsigned path.
    DbField ftod = mkField("tod", DataType::TimeOfDay);
    b = encode(ftod, "3600000", &ok);
    CHECK(ok);
    CHECK(bytesEqual(b, {0x00, 0x36, 0xEE, 0x80}));
    CHECK(render(ftod, b) == "\"01:00:00.000\"");

    std::printf("[PASS] DTL/Date/Time/TimeOfDay temporals\n");
}

void arraysAndEnums() {
    // UDInt array of 3 — one BE u32 per element.
    DbField f3 = mkField("u3", DataType::UDInt, 0, 3);
    bool ok = false;
    auto b = encode(f3, "[1, 2, 3]", &ok);
    CHECK(ok);
    CHECK(bytesEqual(b, {0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03}));
    CHECK(render(f3, b) == "[1,2,3]");

    // Packed Bool array (10 bits -> 2 bytes written into the 10-byte span):
    // elements 0 and 9 set. Bool arrays are always little-bit-order packed
    // (i/8, i%8) regardless of block endianness.
    DbField fb10 = mkField("b10", DataType::Bool, 0, 10);
    b = encode(fb10, "[true, false, false, false, false, false, false, false, false, true]", &ok);
    CHECK(ok);
    CHECK(b.size() == 10);
    CHECK(b[0] == 0x01 && b[1] == 0x02);
    CHECK(render(fb10, b) == "[true,false,false,false,false,false,false,false,false,true]");

    // Enum fields accept their symbolic name; the wire carries the member
    // value and decode renders the number (labels are a read-side
    // projection in PlcState, not in scl/f.scl decode).
    DbField fe = mkField("e", DataType::DInt);
    fe.enum_map.emplace(1, "RUN");
    fe.enum_map.emplace(2, "STOP");
    b = encode(fe, "\"STOP\"", &ok);
    CHECK(ok);
    CHECK(bytesEqual(b, {0x00, 0x00, 0x00, 0x02}));
    CHECK(render(fe, b) == "2");
    // A label not in the map falls through to numeric parsing and is refused.
    bool off_ok = true;
    (void)encode(fe, "\"OFF\"", &off_ok);
    CHECK(!off_ok);

    std::printf("[PASS] scalar arrays, packed Bool arrays, enum member values\n");
}

void structRoundTrip() {
    DbField st;
    st.name = "st";
    st.type = DataType::Struct;
    st.children.push_back(mkField("a", DataType::Int, 0));
    st.children.push_back(mkField("c", DataType::Bool, 2));
    st.struct_size = 3;

    bool ok = false;
    auto b = encode(st, R"JSON({"a":5, "c":true})JSON", &ok);
    CHECK(ok);
    CHECK(bytesEqual(b, {0x00, 0x05, 0x01}));
    CHECK(render(st, b) == R"JSON({"a":5,"c":true})JSON");

    std::printf("[PASS] Struct children encoded at offsets into one shared buffer\n");
}

void jsonPatch() {
    // applyJsonPatchToFields resolves patch keys by full field name and
    // encodes each at the field's own offset inside one shared buffer.
    std::vector<DbField> fields;
    fields.push_back(mkField("a", DataType::Int, 0));
    fields.push_back(mkField("b", DataType::String, 2, 1, 4));

    std::vector<uint8_t> buf(8, 0);
    const auto r = twin::applyJsonPatchToFields(fields, R"JSON({"a":7, "b":"ko"})JSON", buf.data(), buf.size(), Endian::Big);
    CHECK(r.has_value());
    CHECK(buf[0] == 0x00 && buf[1] == 0x07);
    CHECK(buf[2] == 0x04 && buf[3] == 0x02); // String header [4, 2]
    CHECK(buf[4] == 'k' && buf[5] == 'o' && buf[6] == 0x00 && buf[7] == 0x00);

    // Byte-preserving semantics for bit fields inside a patch: bit 5 preset
    // must survive a Bool write at bit 3 of the same byte.
    std::vector<DbField> fields2;
    DbField bo = mkField("bo", DataType::Bool, 0);
    bo.bit_index = 3;
    fields2.push_back(bo);
    buf = {0x20, 0x00, 0x00, 0x00};
    const auto r2 = twin::applyJsonPatchToFields(fields2, R"JSON({"bo":true})JSON", buf.data(), buf.size(), Endian::Big);
    CHECK(r2.has_value());
    CHECK(buf[0] == 0x28);

    std::printf("[PASS] applyJsonPatchToFields offsets + bit preservation\n");
}

void spanHelpers() {
    DbField fs = mkField("s", DataType::String, 0, 1, 8);
    CHECK(fieldSpanSize(fs) == 10); // scalar string span comes from string_capacity
    CHECK(fieldElementSpanBytes(fs) == 10);
    // Schema/symbol-shaped scalar STRING (count=1, string_capacity=8): the
    // twin fork computed typeSpanBytes(String,count) = 2+count = 3 bytes for
    // a 10-byte tile — the under-sizing behind out-of-range-style failures in
    // DbIOProvider/ScriptDataBlock/WalReplayer/sgrn_replay sizing. The twin
    // entry point now delegates to scl's and must agree on both shapes.
    CHECK(twin::fieldSpanSize(fs) == 10);

    // Tag-table-shaped string (chars conflated into count with
    // string_capacity == 0): capacity falls back to count and the string
    // tile is the whole span — no array multiplier (old twin behavior,
    // preserved; scl's former copy would have multiplied elem by count).
    DbField ftag = mkField("tg", DataType::String, 0, 44, 0);
    CHECK(fieldElementSpanBytes(ftag) == 46);
    CHECK(twin::fieldSpanSize(ftag) == 46);
    CHECK(fieldSpanSize(ftag) == 46);

    // Degenerate string (count=0, string_capacity=0) keeps a 2-byte header-only
    // span — matches the pre-unification twin typeSpanBytes behavior.
    DbField fz = mkField("z", DataType::String, 0, 0, 0);
    CHECK(fieldSpanSize(fz) == 2);

    DbField farr = mkField("sa", DataType::String, 0, 2, 4);
    CHECK(fieldElementSpanBytes(farr) == 6);
    CHECK(fieldSpanSize(farr) == 12);
    CHECK(twin::fieldSpanSize(farr) == 12);

    DbField fuds = mkField("ud", DataType::UDInt, 0, 4);
    CHECK(fieldSpanSize(fuds) == 16);

    DbField fst;
    fst.name = "st";
    fst.type = DataType::Struct;
    fst.children = {mkField("a", DataType::Int, 0), mkField("b", DataType::Bool, 2)};
    fst.struct_size = 3;
    CHECK(fieldSpanSize(fst) == 3);

    DbField fdyn = fs;
    fdyn.is_dynamic = true;
    CHECK(fieldSpanSize(fdyn) == 14); // + 4-byte length prefix

    std::printf("[PASS] fieldSpanSize/fieldElementSpanBytes cover strings, structs, dynamic prefix\n");
}

} // namespace

int main() {
    scalarWire();
    adapterRenders();
    boolBitPacking();
    stringTiles();
    temporals();
    arraysAndEnums();
    structRoundTrip();
    jsonPatch();
    spanHelpers();
    if (g_failures == 0) {
        std::printf("All tests passed successfully!\n");
        return 0;
    }
    std::printf("%d failure(s)\n", g_failures);
    return 1;
}
