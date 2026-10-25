// binary_live_snapshot_test.cpp — WebSocket JSON/binary parity on a live
// gateway, ported from tests/gateway/binary_live_snapshot.py (+ the
// dual-transport essence of tests/gateway/websocket.py) to a native
// IXWebSocket client.
//
// Scope asserted here:
//   1. The WebSocket JSON seed frame ({"<DbName>": {...}} pushed on open,
//      produced by server_.getDigitalTwinJsonString() → PlcState::serialize)
//      is structurally identical to HTTP GET /data/<DbName> (same render
//      family): rapidjson-independent deep equality via jsoncpp.
//   2. A full-DB binary subscription ({"command":"subscribe_binary","db":N})
//      receives an immediate binary frame whose payload — after the 12-byte
//      header ([u32 db ~BE][f64 timestamp ~BE]) — is byte-identical to HTTP
//      GET /memory/db/<N>/offset/0/size/<size_bytes> (octet-stream). Both
//      reads originate from server_.readDbMemory.
//   3. A field-slice subscription (offset/size from the /registry field map)
//      frames exactly the requested window: payload bytes equal the
//      corresponding HTTP memory slice.
//   4. Header sanity: the u32 db prefix matches the subscribed DB number and
//      the f64 timestamp parses as a finite double (big-endian on the wire).
//   5. Dual-transport parity (websocket.py's axiom): one WS connection
//      carried both the JSON text frames and the binary RuntimeSync frames
//      for the same DB.
//
// All three formats (JSON twin, binary memory, HTTP raw memory) read from
// the same frozen PLC arena — no s7shell simulation is spawned, so the whole
// comparison is deterministic and can assert exact byte and tree equality
// instead of the Python suite's value-tolerant compares.
//
// Environment: gateway via sgrn::test::GatewayFixture (simulation
// "bottling", frozen arena). Missing binaries → SKIP (exit 0).

#include "gateway_fixture.hpp"

#include <ixwebsocket/IXWebSocket.h>
#include <json/json.h>

#include <httplib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace
{

int g_failures = 0;

#define CHECK(expr)                                                                                                                        \
    do {                                                                                                                                   \
        if (!(expr)) {                                                                                                                     \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);                                                                    \
        }                                                                                                                                  \
    } while (0)

Json::Value parseJson(const std::string& t_json, const char* t_what) {
    Json::Value v;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    if (!reader->parse(t_json.data(), t_json.data() + t_json.size(), &v, &errors)) {
        ++g_failures;
        std::printf("FAIL %s does not parse: %s (%.160s)\n", t_what, errors.c_str(), t_json.c_str());
        return {};
    }
    return v;
}

/// One captured WebSocket message (text or binary).
struct WsFrame {
    bool binary = false;
    std::vector<uint8_t> payload;

    bool isBinary() const {
        return binary;
    }
};

/// Minimal IXWebSocket client mirroring the s7shell GatewaySync connect
/// pattern (threaded transport; frames latched for the test thread).
class WsClient {
public:
    explicit WsClient(const std::string& t_url) {
        ws_.setUrl(t_url);
        ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& t_msg) {
            switch (t_msg->type) {
                case ix::WebSocketMessageType::Open: {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    m_open = true;
                    break;
                }
                case ix::WebSocketMessageType::Message: {
                    WsFrame frame;
                    frame.binary = t_msg->binary;
                    frame.payload.assign(t_msg->str.begin(), t_msg->str.end());
                    std::lock_guard<std::mutex> lk(m_mutex);
                    m_frames.push_back(std::move(frame));
                    break;
                }
                case ix::WebSocketMessageType::Error: {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    m_error = t_msg->errorInfo.reason;
                    break;
                }
                default:
                    break;
            }
            m_cv.notify_all();
        });
        ws_.start();
    }

    ~WsClient() {
        ws_.stop();
    }

    /// Waits until the handshake completes (true) or the timeout expires.
    bool waitOpen(int t_timeout_ms) {
        std::unique_lock<std::mutex> lk(m_mutex);
        return m_cv.wait_for(lk, std::chrono::milliseconds(t_timeout_ms), [&] { return m_open || !m_error.empty(); }) && m_open;
    }

    /// Waits for the n-th frame matching the transport flag (0-based).
    std::optional<WsFrame> waitFrame(bool t_binary, int t_index, int t_timeout_ms) {
        std::unique_lock<std::mutex> lk(m_mutex);
        m_cv.wait_for(lk, std::chrono::milliseconds(t_timeout_ms),
            [&] { return count(t_binary) > static_cast<std::size_t>(t_index) || !m_error.empty(); });
        std::optional<WsFrame> out;
        if (count(t_binary) > static_cast<std::size_t>(t_index)) {
            int seen = 0;
            for (const auto& f : m_frames) {
                if (f.isBinary() == t_binary && seen++ == t_index) {
                    out = f;
                    break;
                }
            }
        }
        return out;
    }

    bool sendText(const std::string& t_json) {
        (void)ws_.sendText(t_json);
        return true;
    }

    std::string error() const {
        std::lock_guard<std::mutex> lk(m_mutex);
        return m_error;
    }

private:
    std::size_t count(bool t_binary) const {
        std::size_t n = 0;
        for (const auto& f : m_frames)
            n += static_cast<std::size_t>(f.isBinary() == t_binary);
        return n;
    }

    ix::WebSocket ws_;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<WsFrame> m_frames;
    bool m_open = false;
    std::string m_error;
};

/// Parses the WS binary frame header: [u32 db ~BE][f64 timestamp ~BE].
struct BinaryHeader {
    uint32_t db = 0;
    double timestamp = 0.0;
    std::vector<uint8_t> body;
};

bool parseBinaryFrame(const WsFrame& t_frame, BinaryHeader& t_out) {
    if (t_frame.payload.size() < 12)
        return false;
    t_out.db = (static_cast<uint32_t>(t_frame.payload[0]) << 24) | (static_cast<uint32_t>(t_frame.payload[1]) << 16) |
               (static_cast<uint32_t>(t_frame.payload[2]) << 8) | static_cast<uint32_t>(t_frame.payload[3]);
    uint64_t raw = 0;
    for (int i = 0; i < 8; ++i)
        raw = (raw << 8) | t_frame.payload[4 + i]; // big-endian u64 → double bits
    static_assert(sizeof(raw) == sizeof(t_out.timestamp), "double must be 64-bit");
    std::memcpy(&t_out.timestamp, &raw, sizeof(t_out.timestamp));
    t_out.body.assign(t_frame.payload.begin() + 12, t_frame.payload.end());
    return true;
}

/// Primitive SCL backing-type widths (s7codec). Strings/UDTs are excluded —
/// their spans derive from capacity/layout rather than a fixed width. The
/// /registry wire renders type names in caps (s7TypeToString).
bool primitiveWidth(const std::string& t_type, std::size_t& t_out) {
    static const std::map<std::string, std::size_t> widths{
        {"BOOL", 1},
        {"BYTE", 1},
        {"USINT", 1},
        {"SINT", 1},
        {"CHAR", 1},
        {"WORD", 2},
        {"UINT", 2},
        {"INT", 2},
        {"DWORD", 4},
        {"UDINT", 4},
        {"DINT", 4},
        {"REAL", 4},
        {"LWORD", 8},
        {"LINT", 8},
        {"LREAL", 8},
    };
    std::string upper;
    upper.reserve(t_type.size());
    for (char c : t_type)
        upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const auto it = widths.find(upper);
    if (it == widths.end())
        return false;
    t_out = it->second;
    return true;
}

} // namespace

int main() {
    sgrn::test::GatewayFixture gw("bottling", {.spawn_simulation = false});
    if (!gw.available()) {
        std::printf("SKIP: gateway unavailable — %s\n", gw.skipReason().c_str());
        return 0;
    }

    httplib::Client http("127.0.0.1", gw.httpPort());
    http.set_connection_timeout(3);

    // ── 1. Registry: locate the first DB (frozen arena) ─────────────────────
    auto reg_res = http.Get("/registry");
    CHECK(reg_res && reg_res->status == 200);
    if (!(reg_res && reg_res->status == 200)) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    const Json::Value reg = parseJson(reg_res->body, "/registry");
    const Json::Value& dbs = reg["dbs"];
    CHECK(dbs.isArray() && !dbs.empty());
    if (!(dbs.isArray() && !dbs.empty())) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    const Json::Value db = dbs[0];
    const int db_num = db["number"].asInt();
    const std::string db_name = db["name"].asString();
    const std::size_t db_size = static_cast<std::size_t>(db["size_bytes"].asInt());
    CHECK(db_num >= 0 && db_num <= 65535);
    CHECK(!db_name.empty());
    CHECK(db_size > 0);
    CHECK(db.isMember("fields") && db["fields"].isArray());
    if (!(db_num >= 0 && db_size > 0)) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }

    // ── 2. HTTP ground truth: semantic twin + raw memory of the frozen arena
    auto data_res = http.Get("/data/" + db_name);
    CHECK(data_res && data_res->status == 200);
    if (!(data_res && data_res->status == 200)) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    const Json::Value http_tree = parseJson(data_res->body, "/data/<DbName>");

    auto mem_res = http.Get(std::string("/memory/db/") + std::to_string(db_num) + std::string("/offset/0/size/") + std::to_string(db_size));
    CHECK(mem_res && mem_res->status == 200);
    if (!(mem_res && mem_res->status == 200)) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    const std::vector<uint8_t> arena(mem_res->body.begin(), mem_res->body.end());
    CHECK(arena.size() == db_size);
    if (!(arena.size() == db_size)) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }

    // ── 3. WebSocket connect: seed JSON frame on open ───────────────────────
    WsClient ws(gw.wsUrl());
    CHECK(ws.waitOpen(5000));
    if (!(ws.waitOpen(5000))) {
        std::printf("FAIL websocket open: %s\n%d failure(s)\n", ws.error().c_str(), g_failures);
        return 1;
    }

    // The server pushes the full twin seed {<DbName>: {...}} immediately on
    // open, followed by the leaf dictionary ({"type":"dictionary",...}).
    // Latch the text frame that carries our DB.
    std::optional<WsFrame> seed_frame;
    for (int i = 0; i < 8 && !seed_frame; ++i) {
        auto frame = ws.waitFrame(false, static_cast<int>(i), 500);
        if (!frame)
            break;
        const Json::Value candidate = parseJson(std::string(frame->payload.begin(), frame->payload.end()), "ws text frame");
        if (candidate.isObject() && candidate.isMember(db_name))
            seed_frame = frame;
    }
    CHECK(seed_frame.has_value());
    if (!seed_frame) {
        std::printf("FAIL: no JSON seed frame containing '%s'\n%d failure(s)\n", db_name.c_str(), g_failures);
        return 1;
    }

    // ── 4. Check 1: WS seed ≥ structural parity with HTTP /data/<DbName> ────
    const Json::Value seed = parseJson(std::string(seed_frame->payload.begin(), seed_frame->payload.end()), "seed frame");
    const Json::Value& seed_tree = seed[db_name];
    const bool trees_equal = (seed_tree == http_tree);
    CHECK(trees_equal);
    if (!trees_equal) {
        // Render both for the failure log (bounded).
        Json::StreamWriterBuilder pretty;
        pretty["indentation"] = "  ";
        std::printf("FAIL seed != /data/%s\n--- seed ---\n%s\n--- http ---\n%s\n", db_name.c_str(),
            Json::writeString(pretty, seed_tree).substr(0, 2000).c_str(), Json::writeString(pretty, http_tree).substr(0, 2000).c_str());
    }

    // ── 5. Check 2: full-DB binary frame == HTTP raw memory (byte equality)
    const bool ok_full = ws.sendText(std::string(R"({"command":"subscribe_binary","db":)") + std::to_string(db_num) + std::string("}"));
    CHECK(ok_full);
    auto bin_full = ws.waitFrame(true, 0, 5000);
    CHECK(bin_full.has_value());
    if (!bin_full) {
        std::printf("FAIL: no binary frame after full-DB subscribe_binary\n%d failure(s)\n", g_failures);
        return 1;
    }
    BinaryHeader full;
    CHECK(parseBinaryFrame(*bin_full, full));
    CHECK(full.body.size() == arena.size());
    CHECK(full.db == static_cast<uint32_t>(db_num));
    CHECK(std::isfinite(full.timestamp));
    CHECK(full.body == arena); // exact byte parity: WS binary == HTTP memory

    // ── 6. Check 3: field-slice subscribe_binary == HTTP memory slice ───────
    // Parity with the Python suite's _first_field/_field_dtype plumbing: the
    // framing property is type-agnostic (the payload is raw memory), so the
    // FIRST field of the DB is used — its byte span is derived from the
    // /registry wire (struct_size for UDT/strings, primitive width × count
    // otherwise); when the span is not derivable a 32-byte window from the
    // field's offset is framed instead.
    const Json::Value& first_field = db["fields"][0];
    const std::size_t slice_offset = first_field.isMember("offset") ? static_cast<std::size_t>(first_field["offset"].asInt()) : 0u;
    std::size_t slice_size = 0;
    const std::string field_type = first_field.isMember("type") ? first_field["type"].asString() : std::string();
    std::size_t width = 0;
    const std::size_t count =
        first_field.isMember("count") ? static_cast<std::size_t>(first_field["count"].asInt() > 0 ? first_field["count"].asInt() : 1) : 1u;
    if (first_field.isMember("struct_size") && first_field["struct_size"].asInt() > 0)
        slice_size = static_cast<std::size_t>(first_field["struct_size"].asInt()) * count;
    else if (primitiveWidth(field_type, width))
        slice_size = width * count;
    if (slice_size == 0 || slice_offset + slice_size > db_size)
        slice_size = std::min<std::size_t>(32, db_size - slice_offset);
    CHECK(slice_size > 0);
    CHECK(slice_offset + slice_size <= db_size);
    std::printf("NOTE: field slice candidate '%s' offset=%zu size=%zu (type=%s)\n", first_field["name"].asString().c_str(), slice_offset,
        slice_size, field_type.c_str());
    if (!(slice_size > 0 && slice_offset + slice_size <= db_size)) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }

    const std::string slice_cmd = std::string(R"({"command":"subscribe_binary","db":)") + std::to_string(db_num) +
                                  std::string(",\"offset\":") + std::to_string(slice_offset) + std::string(",\"size\":") +
                                  std::to_string(slice_size) + std::string("}");
    CHECK(ws.sendText(slice_cmd));
    auto bin_slice = ws.waitFrame(true, 1, 5000);
    CHECK(bin_slice.has_value());
    if (!bin_slice) {
        std::printf("FAIL: no binary frame after slice subscribe_binary\n%d failure(s)\n", g_failures);
        return 1;
    }
    BinaryHeader slice_hdr;
    CHECK(parseBinaryFrame(*bin_slice, slice_hdr));
    CHECK(slice_hdr.body.size() == slice_size);
    CHECK(slice_hdr.db == static_cast<uint32_t>(db_num));
    CHECK(std::isfinite(slice_hdr.timestamp));
    CHECK(std::equal(slice_hdr.body.begin(), slice_hdr.body.end(), arena.begin() + static_cast<long>(slice_offset)));

    // ── 7. Check 5 (websocket.py axiom): dual transport on one connection ──
    auto bin_zero = ws.waitFrame(true, 0, 1); // already captured above
    CHECK(bin_zero.has_value());
    CHECK(seed_frame.has_value());
    CHECK(bin_zero->payload.size() == 12 + db_size);

    if (g_failures == 0)
        std::printf("binary_live_snapshot_test: all checks passed (db=%d '%s' size=%zu ws=%s)\n", db_num, db_name.c_str(), db_size,
            gw.wsUrl().c_str());
    else
        std::printf("%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
