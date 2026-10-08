// RuntimeSync protocol codec tests.
//
// These tests cover the transport-neutral SGRW frame codec used by both the
// gateway WebSocket adapter and s7shell GatewaySync.  Keep this test free of
// Crow/network dependencies so malformed-frame handling can be exercised
// quickly and deterministically.

#include <sgrn/gateway/adapters/websocket/RuntimeSyncProtocol.hpp>

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;

#define CHECK(condition)                                                                                                                   \
    do {                                                                                                                                   \
        if (!(condition)) {                                                                                                                \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                               \
        }                                                                                                                                  \
    } while (false)

using namespace sgrn::gateway::adapters::websocket::runtime_sync;

void checkRoundTrip() {
    Frame frame;
    frame.kind = Kind::Write;
    frame.sequence = 0x0102030405060708ULL;
    frame.timestamp_ms = 0x1112131415161718ULL;
    frame.records = {
        Record{1, 0x11223344U, {0x00, 0x01, 0xFE, 0xFF}},
        Record{65535, 0xAABBCCDDU, {0x10, 0x20, 0x30}},
    };

    const std::string encoded = encode(frame);
    CHECK(!encoded.empty());
    CHECK(encoded.size() == kHeaderSize + (kRecordHeaderSize + 4) + (kRecordHeaderSize + 3));
    CHECK(isFrame(encoded));

    // The wire format is explicitly little-endian and versioned.
    CHECK(static_cast<uint8_t>(encoded[0]) == 'S');
    CHECK(static_cast<uint8_t>(encoded[1]) == 'G');
    CHECK(static_cast<uint8_t>(encoded[2]) == 'R');
    CHECK(static_cast<uint8_t>(encoded[3]) == 'W');
    CHECK(static_cast<uint8_t>(encoded[4]) == 0x01);
    CHECK(static_cast<uint8_t>(encoded[5]) == 0x00);
    CHECK(static_cast<uint8_t>(encoded[6]) == static_cast<uint8_t>(Kind::Write));
    CHECK(static_cast<uint8_t>(encoded[8]) == 0x08);
    CHECK(static_cast<uint8_t>(encoded[9]) == 0x07);
    CHECK(static_cast<uint8_t>(encoded[24]) == 0x02);

    Frame decoded;
    std::string error;
    CHECK(decode(encoded, decoded, &error));
    CHECK(error.empty());
    CHECK(decoded.kind == frame.kind);
    CHECK(decoded.sequence == frame.sequence);
    CHECK(decoded.timestamp_ms == frame.timestamp_ms);
    CHECK(decoded.records.size() == frame.records.size());
    for (size_t i = 0; i < frame.records.size() && i < decoded.records.size(); ++i) {
        CHECK(decoded.records[i].db == frame.records[i].db);
        CHECK(decoded.records[i].offset == frame.records[i].offset);
        CHECK(decoded.records[i].bytes == frame.records[i].bytes);
    }
    CHECK(encode(decoded) == encoded);
}

void checkKindsAndEmptyAck() {
    for (Kind kind : {Kind::Delta, Kind::Write, Kind::Ack}) {
        Frame frame;
        frame.kind = kind;
        frame.sequence = 42;
        frame.timestamp_ms = 99;
        const std::string encoded = encode(frame);
        CHECK(encoded.size() == kHeaderSize);

        Frame decoded;
        CHECK(decode(encoded, decoded));
        CHECK(decoded.kind == kind);
        CHECK(decoded.records.empty());
    }
}

void checkMalformedFramesFailClosed() {
    Frame decoded;
    std::string error;

    CHECK(!decode("", decoded, &error));
    CHECK(error == "invalid runtime-sync frame size");
    CHECK(!isFrame("SGR"));

    std::string bad_magic(kHeaderSize, '\0');
    CHECK(!decode(bad_magic, decoded, &error));
    CHECK(error == "invalid runtime-sync magic");

    std::string bad_version = encode(Frame{});
    bad_version[4] = 2;
    CHECK(!decode(bad_version, decoded, &error));
    CHECK(error == "unsupported runtime-sync frame");

    std::string bad_kind = encode(Frame{});
    bad_kind[6] = 0;
    CHECK(!decode(bad_kind, decoded, &error));
    CHECK(error == "unsupported runtime-sync frame");

    // One declared record, but no record header follows the frame header.
    std::string missing_record = encode(Frame{});
    missing_record[24] = 1;
    CHECK(!decode(missing_record, decoded, &error));
    CHECK(error == "runtime-sync record count exceeds frame");

    Frame one_record;
    one_record.records.push_back(Record{7, 10, {1, 2, 3}});
    std::string truncated = encode(one_record);
    truncated.pop_back();
    CHECK(!decode(truncated, decoded, &error));
    CHECK(error == "runtime-sync record exceeds frame");

    std::string trailing = encode(Frame{});
    trailing.push_back('\0');
    CHECK(!decode(trailing, decoded, &error));
    CHECK(error == "trailing runtime-sync bytes");

    // A frame larger than the protocol limit is rejected before parsing.
    std::string oversized(kMaxFrameSize + 1, '\0');
    CHECK(!decode(oversized, decoded, &error));
    CHECK(error == "invalid runtime-sync frame size");
}

void checkEncodeLimit() {
    Frame frame;
    frame.kind = Kind::Write;
    frame.records.push_back(Record{1, 0, std::vector<uint8_t>(kMaxFrameSize, 0xA5)});
    CHECK(encode(frame).empty());
}

void checkRandomInputsDoNotCrash() {
    std::mt19937 generator(0x53475257U);
    std::uniform_int_distribution<size_t> length(0, 256);
    std::uniform_int_distribution<unsigned> byte(0, 255);

    for (size_t i = 0; i < 10000; ++i) {
        std::string input(length(generator), '\0');
        for (char& value : input)
            value = static_cast<char>(byte(generator));

        Frame decoded;
        std::string error;
        const bool valid = decode(input, decoded, &error);
        if (valid)
            CHECK(encode(decoded) == input);
    }
}

} // namespace

int main() {
    checkRoundTrip();
    checkKindsAndEmptyAck();
    checkMalformedFramesFailClosed();
    checkEncodeLimit();
    checkRandomInputsDoNotCrash();
    return g_failures == 0 ? 0 : 1;
}
