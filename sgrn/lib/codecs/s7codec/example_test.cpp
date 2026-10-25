// s7codec_example_test.cpp — regression harness for the header-only S7
// codec: scalar decode round-trips (Int/Real big-endian) and packed-struct
// memory mirroring (auto byte-swap assignments matching S7 wire layout).
//
// Note: assertions are kept via -UNDEBUG in CMakeLists.txt since this is the
// only guard the harness has.

#include <s7codec/s7.hpp>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <variant>
#include <vector>

using s7codec::DecodedValue;
using s7codec::decodeScalar;

int main() {
    std::cout << "Running S7 Codec Tests..." << std::endl;

    // Test 1: Scalar Decoding (Int — S7 Int is big-endian int16)
    uint8_t int_buffer[] = {0x00, 0x2A}; // 42 in big-endian
    DecodedValue int_val = decodeScalar(s7codec::Type::Int, int_buffer, sizeof(int_buffer));
    assert(int_val.valid());
    assert(std::get<int64_t>(int_val) == 42);
    std::cout << "[PASS] Scalar Int Decoding" << std::endl;

    // Test 2: Real Decoding (S7 Real is big-endian IEEE 754 float)
    uint8_t real_buffer[] = {0x41, 0xBC, 0x00, 0x00}; // 23.5 in IEEE 754 Big-Endian
    DecodedValue real_val = decodeScalar(s7codec::Type::Real, real_buffer, sizeof(real_buffer));

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

    DecodedValue speed_back = decodeScalar(s7codec::Type::Int, db_buffer, 2);
    assert(speed_back.valid() && std::get<int64_t>(speed_back) == 1500);

    DecodedValue temp_back = decodeScalar(s7codec::Type::Real, db_buffer + 2, 4);

    assert(temp_back.valid());
    assert(std::get<float>(temp_back) > 45.49f && std::get<float>(temp_back) < 45.51f);

    std::cout << "[PASS] Structural Mirroring (Endianness)" << std::endl;

    std::cout << "All tests passed successfully!" << std::endl;
    return 0;
}
