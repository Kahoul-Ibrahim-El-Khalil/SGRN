// s7codec_example_test.cpp — regression harness for the header-only S7
// codec: scalar decode round-trips (Int/Real big-endian), packed-struct
// memory mirroring (auto byte-swap assignments matching S7 wire layout),
// span-based primitive round-trips with exact byte(layout) checks, bounds
// behavior on undersized buffers, and string/date capacity semantics.
//
// Note: assertions are kept via -UNDEBUG in CMakeLists.txt since this is the
// only guard the harness has.

#include <s7codec/s7.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using s7codec::BitOffset;
using s7codec::CodecStatus;
using s7codec::DecodedValue;
using s7codec::DecodeOptions;
using s7codec::decodeScalar;
using s7codec::DtlComponents;
using s7codec::encodeBool;
using s7codec::EncodeOptions;
using s7codec::encodeScalar;
using s7codec::encodeString;
using s7codec::encodeU16;
using s7codec::encodeU32;
using s7codec::encodeU8;
using s7codec::Endian;
using s7codec::ReadBuffer;
using s7codec::StringCapacity;
using s7codec::Type;
using s7codec::WriteBuffer;

int main() {
    std::cout << "Running S7 Codec Tests..." << std::endl;

    // Test 1: Scalar Decoding (Int — S7 Int is big-endian int16)
    uint8_t int_buffer[] = {0x00, 0x2A}; // 42 in big-endian
    DecodedValue int_val = decodeScalar(s7codec::Type::Int, s7codec::ReadBuffer{int_buffer, sizeof(int_buffer)});
    assert(int_val.valid());
    assert(std::get<int64_t>(int_val) == 42);
    std::cout << "[PASS] Scalar Int Decoding" << std::endl;

    // Test 2: Real Decoding (S7 Real is big-endian IEEE 754 float)
    uint8_t real_buffer[] = {0x41, 0xBC, 0x00, 0x00}; // 23.5 in IEEE 754 Big-Endian
    DecodedValue real_val = decodeScalar(s7codec::Type::Real, s7codec::ReadBuffer{real_buffer, sizeof(real_buffer)});

    assert(real_val.valid());

    const float decoded_real = std::get<float>(real_val);

    assert(decoded_real > 23.49f && decoded_real < 23.51f);

    std::cout << "[PASS] Scalar Real Decoding" << std::endl;

    // Test 3: Structural Mirroring — packed struct over a raw DB buffer with
    // auto byte-swapping assignments, then simulate the production path by
    // decoding the buffer back through the scalar codec.
    struct __attribute__((packed)) TestDB {
        s7codec::Int speed;
        s7codec::Real temp;
    };
    static_assert(sizeof(TestDB) == 6);

    uint8_t db_buffer[6];

    auto* db = reinterpret_cast<TestDB*>(db_buffer);
    db->speed = 1500;
    db->temp = 45.5f;

    assert(db_buffer[0] == 0x05 && db_buffer[1] == 0xDC); // 1500 is 0x05DC big-endian

    DecodedValue speed_back = decodeScalar(s7codec::Type::Int, s7codec::ReadBuffer{db_buffer, 2});
    assert(speed_back.valid() && std::get<int64_t>(speed_back) == 1500);

    DecodedValue temp_back = decodeScalar(s7codec::Type::Real, s7codec::ReadBuffer{db_buffer + 2, 4});

    assert(temp_back.valid());
    assert(std::get<float>(temp_back) > 45.49f && std::get<float>(temp_back) < 45.51f);

    std::cout << "[PASS] Structural Mirroring (Endianness)" << std::endl;

    // Test 4: Span-API round-trips (Int / Real / LReal) through the
    // span-based encode/decode primaries, plus exact big-endian bytes.
    {
        std::array<uint8_t, 8> int_buf{};
        assert(encodeScalar(DecodedValue(static_cast<int64_t>(-12345)), Type::Int, WriteBuffer(int_buf)).has_value());
        assert(int_buf[0] == 0xCF && int_buf[1] == 0xC7); // -12345 is 0xCFC7 big-endian
        DecodedValue iv = decodeScalar(Type::Int, ReadBuffer(int_buf));
        assert(iv.valid() && std::get<int64_t>(iv) == -12345);

        std::array<uint8_t, 8> real_buf{};
        assert(encodeScalar(DecodedValue(23.5f), Type::Real, WriteBuffer(real_buf)).has_value());
        assert(real_buf[0] == 0x41 && real_buf[1] == 0xBC && real_buf[2] == 0x00 && real_buf[3] == 0x00);
        DecodedValue rv = decodeScalar(Type::Real, ReadBuffer(real_buf));
        assert(rv.valid() && std::get<float>(rv) == 23.5f);

        std::array<uint8_t, 8> lreal_buf{};
        assert(encodeScalar(DecodedValue(45.5), Type::LReal, WriteBuffer(lreal_buf)).has_value());
        assert(lreal_buf[0] == 0x40 && lreal_buf[1] == 0x46 && lreal_buf[2] == 0xC0); // 45.5 = 0x4046C000..
        DecodedValue lv = decodeScalar(Type::LReal, ReadBuffer(lreal_buf));
        assert(lv.valid() && std::get<double>(lv) == 45.5);

        std::cout << "[PASS] Span round-trips (Int/Real/LReal)" << std::endl;
    }

    // Test 5: UInt16 in BOTH endians — exact expected bytes on encode and
    // byte-order honored on decode.
    {
        std::array<uint8_t, 2> be_buf{};
        assert(encodeScalar(
            DecodedValue(static_cast<uint64_t>(0xBEEFu)), Type::UInt, WriteBuffer(be_buf), EncodeOptions{.endian = Endian::Big})
                .has_value());
        assert(be_buf[0] == 0xBE && be_buf[1] == 0xEF);
        DecodedValue be_v = decodeScalar(Type::UInt, ReadBuffer(be_buf), DecodeOptions{.endian = Endian::Big});
        assert(be_v.valid() && std::get<uint64_t>(be_v) == 0xBEEF);

        std::array<uint8_t, 2> le_buf{};
        assert(encodeScalar(
            DecodedValue(static_cast<uint64_t>(0xBEEFu)), Type::UInt, WriteBuffer(le_buf), EncodeOptions{.endian = Endian::Little})
                .has_value());
        assert(le_buf[0] == 0xEF && le_buf[1] == 0xBE);
        DecodedValue le_v = decodeScalar(Type::UInt, ReadBuffer(le_buf), DecodeOptions{.endian = Endian::Little});
        assert(le_v.valid() && std::get<uint64_t>(le_v) == 0xBEEF);

        std::cout << "[PASS] UInt16 big/little-endian byte layout" << std::endl;
    }

    // Test 6: Bool at BitOffset 5 — set clears/sets ONLY that bit (LSB-first
    // convention: bit N covers 0x01 << N of the first byte).
    {
        std::array<uint8_t, 1> bit_buf{};
        assert(encodeBool(true, BitOffset{5}, WriteBuffer(bit_buf)).has_value());
        assert(bit_buf[0] == 0x20);
        assert(encodeBool(false, BitOffset{5}, WriteBuffer(bit_buf)).has_value());
        assert(bit_buf[0] == 0x00); // clearing must write 0, not touch neighbors
        bit_buf[0] = 0xFF;
        assert(encodeBool(false, BitOffset{5}, WriteBuffer(bit_buf)).has_value());
        assert(bit_buf[0] == 0xDF);
        assert(encodeBool(true, BitOffset{5}, WriteBuffer(bit_buf)).has_value());
        assert(bit_buf[0] == 0xFF);

        std::cout << "[PASS] Bool BitOffset 5 set/clear" << std::endl;
    }

    // Test 7: BitOffset bounds — 7 is the highest valid offset, 8 is rejected.
    {
        std::array<uint8_t, 1> bit_buf{};
        assert(encodeBool(true, BitOffset{7}, WriteBuffer(bit_buf)).has_value());
        assert(bit_buf[0] == 0x80);
        auto bad = encodeBool(true, BitOffset{8}, WriteBuffer(bit_buf));
        assert(!bad.has_value());
        assert(bad.error() == CodecStatus::INVALID_BIT_INDEX);

        std::cout << "[PASS] BitOffset 7 accepted / 8 rejected" << std::endl;
    }

    // Test 8: Bounds on empty and undersized buffers — decode yields an
    // invalid value, encode reports BUFFER_TOO_SMALL.
    {
        DecodedValue v = decodeScalar(Type::Int, ReadBuffer{});
        assert(!v.valid());
        v = decodeScalar(Type::Real, ReadBuffer{});
        assert(!v.valid());

        std::array<uint8_t, 1> one{0}; // Int/Real need 2/4 bytes
        assert(!decodeScalar(Type::Int, ReadBuffer(one)).valid());
        assert(!decodeScalar(Type::Real, ReadBuffer(one)).valid());

        std::array<uint8_t, 2> two{};
        assert(!decodeScalar(Type::Real, ReadBuffer(two)).valid());

        auto e8 = encodeU8(0xFFu, WriteBuffer{});
        assert(!e8.has_value() && e8.error() == CodecStatus::BUFFER_TOO_SMALL);
        auto e16 = encodeU16(0x1234u, WriteBuffer(one));
        assert(!e16.has_value() && e16.error() == CodecStatus::BUFFER_TOO_SMALL);

        std::cout << "[PASS] Empty / undersized buffers rejected" << std::endl;
    }

    // Test 9: String capacity 8 — header layout [max_len=8, cur_len=2],
    // zero-filled tail, STRING_TOO_LONG on overflow, decode-side clamping
    // of a corrupt header, and bounds refusal when bytes really are missing.
    {
        std::array<uint8_t, 10> str_buf{}; // 2 header bytes + 8 capacity
        assert(encodeString("hi", StringCapacity{8}, WriteBuffer(str_buf)).has_value());
        assert(str_buf[0] == 8);
        assert(str_buf[1] == 2);
        assert(str_buf[2] == 'h' && str_buf[3] == 'i');
        for (size_t i = 4; i < str_buf.size(); ++i)
            assert(str_buf[i] == 0x00); // tail zeroed

        auto too_long = encodeString("123456789", StringCapacity{8}, WriteBuffer(str_buf));
        assert(!too_long.has_value() && too_long.error() == CodecStatus::STRING_TOO_LONG);

        // Decode with capacity 8 — corrupt cur_len=200 header is clamped to 8
        // (never read past the real capacity).
        std::array<uint8_t, 12> corrupt{8, 200, 'h', 'i'};
        DecodedValue sv = decodeScalar(Type::String, ReadBuffer(corrupt), DecodeOptions{.stringCapacity = StringCapacity{8}});
        assert(sv.valid() && sv.s().size() == 8 && sv.s().compare(0, 2, "hi") == 0);

        // With the default capacity (0 -> 254) the corrupt header survives
        // clamping, but the genuinely missing bytes keep the decode bounded.
        std::array<uint8_t, 4> truncated{8, 200, 0x00, 0x00};
        assert(!decodeScalar(Type::String, ReadBuffer(truncated)).valid());

        std::cout << "[PASS] String capacity semantics + decode clamp" << std::endl;
    }

    // Test 10: WString round-trip — UTF-8 in, UTF-16 on the wire (BE code
    // units), UTF-8 back out. Header is [max_len, cur_len] as u16s.
    {
        std::array<uint8_t, 12> w_buf{}; // 4 header bytes + 4 units * 2
        assert(encodeScalar(
            DecodedValue(std::string("AB")), Type::WString, WriteBuffer(w_buf), EncodeOptions{.stringCapacity = StringCapacity{4}})
                .has_value());
        assert(w_buf[0] == 0x00 && w_buf[1] == 0x04); // max_len BE u16
        assert(w_buf[2] == 0x00 && w_buf[3] == 0x02); // cur_len BE u16
        assert(w_buf[4] == 0x00 && w_buf[5] == 0x41); // 'A'
        assert(w_buf[6] == 0x00 && w_buf[7] == 0x42); // 'B'
        for (size_t i = 8; i < w_buf.size(); ++i)
            assert(w_buf[i] == 0x00);

        DecodedValue wv = decodeScalar(Type::WString, ReadBuffer(w_buf));
        assert(wv.valid() && wv.s() == "AB");

        std::cout << "[PASS] WString round-trip and layout" << std::endl;
    }

    // Test 11: DTL exact byte layout (Big-Endian) — 2024-03-18 (dow 1)
    // 19:30:00 ns=500000000.
    {
        std::array<uint8_t, 12> dtl_buf{};
        DtlComponents c{2024, 3, 18, 1, 19, 30, 0, 500'000'000u};
        assert(s7codec::encodeDtl(c, WriteBuffer(dtl_buf), Endian::Big).has_value());
        const std::array<uint8_t, 12> expected{0x07, 0xE8, 0x03, 0x12, 0x01, 0x13, 0x1E, 0x00, 0x1D, 0xCD, 0x65, 0x00};
        assert(dtl_buf == expected);
        // And the decode path renders the same instant back.
        DecodedValue dv = decodeScalar(Type::DTL, ReadBuffer(dtl_buf));
        assert(dv.valid() && dv.s() == "2024-03-18 19:30:00.500000000");

        std::cout << "[PASS] DTL exact byte layout + decode rendering" << std::endl;
    }

    // Test 12: Date / TOD round-trips through the string codec path.
    {
        std::array<uint8_t, 2> date_buf{};
        assert(encodeScalar(DecodedValue(std::string("1990-01-01")), Type::Date, WriteBuffer(date_buf)).has_value());
        assert(date_buf[0] == 0x00 && date_buf[1] == 0x00); // 0 days since 1990-01-01
        DecodedValue dv = decodeScalar(Type::Date, ReadBuffer(date_buf));
        assert(dv.valid() && dv.s() == "1990-01-01");

        std::array<uint8_t, 4> tod_buf{};
        assert(encodeScalar(DecodedValue(static_cast<int64_t>(3600000)), Type::TimeOfDay, WriteBuffer(tod_buf)).has_value());
        assert(tod_buf[0] == 0x00 && tod_buf[1] == 0x36 && tod_buf[2] == 0xEE && tod_buf[3] == 0x80); // 3600000 = 0x36EE80
        DecodedValue tv = decodeScalar(Type::TimeOfDay, ReadBuffer(tod_buf));
        assert(tv.valid() && tv.s() == "01:00:00.000");

        std::cout << "[PASS] Date / TOD round-trips" << std::endl;
    }

    // Test 13: DTL direct nanosecond conversion & round-trip (zero heap allocations, Big and Little Endian)
    {
        // 2024-03-18 19:30:00.500000000 UTC
        // Year 2024 (0x07E8), Month 3, Day 18, Weekday 1, Hour 19, Min 30, Sec 0, Ns 500000000 (0x1DCD6500)
        int64_t expected_ns = 1710790200'500'000'000LL;

        // --- Big-Endian ---
        std::array<uint8_t, 12> dtl_buf_be{0x07, 0xE8, 0x03, 0x12, 0x01, 0x13, 0x1E, 0x00, 0x1D, 0xCD, 0x65, 0x00};
        int64_t ns_be = s7codec::decodeDtlToNanoseconds(ReadBuffer(dtl_buf_be), DecodeOptions{.endian = Endian::Big});
        assert(ns_be == expected_ns);

        s7codec::DTL reconstructed_be = s7codec::DTL::fromUnixNanoseconds(ns_be);
        assert(reconstructed_be.toString() == "2024-03-18 19:30:00.500000000");

        std::array<uint8_t, 12> encoded_buf_be{};
        assert(s7codec::encodeDtl(ns_be, WriteBuffer(encoded_buf_be), Endian::Big).has_value());
        assert(encoded_buf_be == dtl_buf_be);

        // --- Little-Endian ---
        // Year LE: 0xE8, 0x07; Month..Sec: identical; Ns LE: 0x00, 0x65, 0xCD, 0x1D
        std::array<uint8_t, 12> dtl_buf_le{0xE8, 0x07, 0x03, 0x12, 0x01, 0x13, 0x1E, 0x00, 0x00, 0x65, 0xCD, 0x1D};
        int64_t ns_le = s7codec::decodeDtlToNanoseconds(ReadBuffer(dtl_buf_le), DecodeOptions{.endian = Endian::Little});
        assert(ns_le == expected_ns);

        s7codec::DTL reconstructed_le = s7codec::DTL::fromUnixNanoseconds(ns_le);
        assert(reconstructed_le.toString() == "2024-03-18 19:30:00.500000000");

        std::array<uint8_t, 12> encoded_buf_le{};
        assert(s7codec::encodeDtl(ns_le, WriteBuffer(encoded_buf_le), Endian::Little).has_value());
        assert(encoded_buf_le == dtl_buf_le);

        std::cout << "[PASS] DTL direct nanosecond encoding/decoding (BE & LE)" << std::endl;
    }

    std::cout << "All tests passed successfully!" << std::endl;
    return 0;
}
