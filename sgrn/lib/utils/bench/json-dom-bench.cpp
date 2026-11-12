// json-dom-bench — micro-benchmark comparing the glaze-backed sgrn::json::dom
// shim against rapidjson on the JSON patterns SGRN actually uses.
//
// Motivation: SGRN migrated from rapidjson to glaze via the sgrn::json::dom
// shim (see docs/json-glaze-decisions.md). This benchmark verifies the hard
// requirement that the adoption does not hurt performance. It is a standalone
// translation unit on purpose: it does not touch the repo build system, and it
// links against whatever glaze/rapidjson sources the CPM cache holds, so it can
// be re-run at any time to re-validate the migration.
//
// Patterns measured (all shaped like the S7 decode/telemetry hot paths):
//   1. write_fresh — streaming build of a synthetic S7-decode payload into a
//      StringBuffer that is freshly constructed per iteration (the pattern used
//      by decodeDbBuffer / serializeFieldTo, which construct a local
//      StringBuffer per call).
//   2. write_reuse — same build, but the StringBuffer is reused across
//      iterations (the thread_local pattern used by TelemetryBroker). For the
//      dom writer this exercises sgrn::json::writeTo's capacity-preserving
//      append; rapidjson Writer<StringBuffer> also reuses the buffer.
//   3. accept — serialize an already-built DOM tree (the pattern used by
//      Registry::getFullSnapshot/getDeltaSnapshot via Writer::writeValue /
//      Value::accept). rapidjson uses Document::Accept(Writer<StringBuffer>&).
//   4. parse — parse a ~10-20 KB document and walk it reading fields
//      (the json-patch / HTTP handler pattern).
//
// Build (from repo root, paths depend on the CPM cache layout):
//   clang++ -std=c++23 -O3 -march=native -DNDEBUG \
//     -I sgrn/lib/utils/include \
//     -I ~/.cache/CPM/glaze/edc9/include \
//     -I ~/.cache/CPM/rapidjson/2383/include \
//     sgrn/lib/utils/bench/json-dom-bench.cpp -o /tmp/opencode/json-dom-bench
//
// All measurements report best-of-N wall time per operation (lower is better)
// plus a checksum so the compiler cannot eliminate the work.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include <sgrn/utils/json-dom.hpp>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace
{

// ---------------------------------------------------------------------------
// Deterministic synthetic workload
// ---------------------------------------------------------------------------

// Pseudo-random generator with a fixed seed: keeps every iteration identical so
// timings are comparable and the run is reproducible.
struct XorShift {
    uint64_t state = 0x243F6A8885A308D3ull;
    uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
};

constexpr int kTags = 128;
constexpr int kTrend = 8;

// The event stream both writers are fed. One "tag record" mixes scalars,
// strings, a numeric array and a nested object — the shape produced by
// S7SerializationUtils::serializeFieldTo for a decoded PLC snapshot.
template <typename Writer>
void emitPayload(Writer& w, uint64_t seed) {
    XorShift rng{seed};
    w.startObject();
    w.key("device");
    w.string("plc-station-01");
    w.key("tags");
    w.startArray();
    for (int i = 0; i < kTags; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "Tag_%04d", i);
        w.startObject();
        w.key("name");
        w.string(name);
        w.key("type");
        w.string(i % 4 == 0 ? "Real" : i % 4 == 1 ? "DInt" : i % 4 == 2 ? "String" : "Bool");
        w.key("value");
        switch (i % 5) {
            case 0:
                w.nullValue();
                break;
            case 1:
                w.int64(static_cast<int64_t>(rng.next() >> 1));
                break;
            case 2:
                w.uint64(rng.next());
                break;
            case 3:
                w.float64(static_cast<double>(rng.next() % 1000000) / 1024.0);
                break;
            default: {
                char text[24];
                std::snprintf(text, sizeof(text), "v%llu", static_cast<unsigned long long>(rng.next() % 100000));
                w.string(text);
                break;
            }
        }
        w.key("trend");
        w.startArray();
        for (int j = 0; j < kTrend; ++j) {
            w.float64(static_cast<double>(rng.next() % 65536) / 256.0);
        }
        w.endArray();
        w.key("quality");
        w.startObject();
        w.key("code");
        w.int32(static_cast<int>(rng.next() % 4));
        w.key("ts");
        w.int64(static_cast<int64_t>(rng.next()));
        w.key("ok");
        w.boolean((rng.next() & 1) != 0);
        w.endObject();
        w.endObject();
    }
    w.endArray();
    w.endObject();
}

// Prevent the optimizer from hoisting loop-invariant work out of the timed
// loops (relevant for the accept/parse benchmarks, whose inputs never change).
template <class T>
inline void doNotOptimize(T& value) {
    asm volatile("" : "+r,m"(value) : : "memory");
}

// Eight rotating seeds per timed loop: keeps the emitted payload identical in
// size but different in content, so the compiler cannot hoist the build out of
// the loop.
constexpr int kSeedCount = 8;
constexpr uint64_t kSeeds[kSeedCount] = {42, 43, 44, 45, 46, 47, 48, 49};
inline uint64_t seedAt(int i) {
    return kSeeds[i % kSeedCount];
}

// Walk the parsed result of emitPayload so parse benchmarks do real work.
// rapidjson and sgrn::json::dom use different array-iteration APIs, so each
// library gets its own walker.
uint64_t walkRapidJson(const rapidjson::Document& d) {
    uint64_t sum = 0;
    auto tagsIt = d.FindMember("tags");
    if (tagsIt == d.MemberEnd())
        return 0;
    for (auto it = tagsIt->value.Begin(); it != tagsIt->value.End(); ++it) {
        auto vIt = it->FindMember("value");
        if (vIt != it->MemberEnd()) {
            const auto& v = vIt->value;
            if (v.IsInt64())
                sum += static_cast<uint64_t>(v.GetInt64());
            else if (v.IsDouble())
                sum += static_cast<uint64_t>(v.GetDouble());
        }
    }
    return sum;
}

uint64_t walkDom(const sgrn::json::dom::Document& d) {
    uint64_t sum = 0;
    auto tagsIt = d.findMember("tags");
    if (tagsIt == d.memberEnd())
        return 0;
    for (auto e : tagsIt->value.elements()) {
        auto vIt = e.findMember("value");
        if (vIt != e.memberEnd()) {
            const auto& v = vIt->value;
            if (v.isInt64())
                sum += static_cast<uint64_t>(v.getInt64());
            else if (v.isDouble())
                sum += static_cast<uint64_t>(v.getDouble());
        }
    }
    return sum;
}

// ---------------------------------------------------------------------------
// rapidjson adapter — emitPayload is written against the dom writer's spelling
// so both libraries execute exactly the same event stream.
// ---------------------------------------------------------------------------

template <typename RjWriter>
struct RapidJsonAdapter {
    RjWriter& w;
    bool startObject() {
        return w.StartObject();
    }
    bool endObject() {
        return w.EndObject();
    }
    bool startArray() {
        return w.StartArray();
    }
    bool endArray() {
        return w.EndArray();
    }
    bool key(const char* k) {
        return w.Key(k);
    }
    bool string(const char* s) {
        return w.String(s);
    }
    bool boolean(bool b) {
        return w.Bool(b);
    }
    bool int32(int v) {
        return w.Int(v);
    }
    bool int64(int64_t v) {
        return w.Int64(v);
    }
    bool uint64(uint64_t v) {
        return w.Uint64(v);
    }
    bool float64(double v) {
        return w.Double(v);
    }
    bool nullValue() {
        return w.Null();
    }
};

// ---------------------------------------------------------------------------
// Timing harness — best-of-reps wall time per call
// ---------------------------------------------------------------------------

volatile uint64_t g_sink = 0;

template <typename F>
double bestOf(int iters, int reps, F&& f) {
    // warm-up (JIT-ish effects, allocator arenas, branch predictors)
    for (int i = 0; i < iters / 4 + 1; ++i)
        f(i);
    double best = 1e30;
    for (int r = 0; r < reps; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i)
            f(i);
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        best = std::min(best, ms / iters);
    }
    return best * 1e6; // ns per call
}

void report(const char* label, double nsPerCall, double bytesPerCall) {
    if (bytesPerCall > 0) {
        char perTag[32];
        std::snprintf(perTag, sizeof(perTag), "  %.2f GB/s", bytesPerCall / nsPerCall);
        std::printf("  %-28s %10.1f ns/call%s\n", label, nsPerCall, perTag);
    } else {
        std::printf("  %-28s %10.1f ns/call\n", label, nsPerCall);
    }
}

// ---------------------------------------------------------------------------
// Benchmarks
// ---------------------------------------------------------------------------

void benchWriteFresh() {
    std::printf("build: fresh StringBuffer per call (decodeDbBuffer pattern)\n");

    double rj = bestOf(2000, 5, [&](int i) {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        RapidJsonAdapter aw{w};
        emitPayload(aw, seedAt(i));
        doNotOptimize(sb);
        g_sink += static_cast<uint64_t>(sb.GetSize());
    });

    double dom = bestOf(2000, 5, [&](int i) {
        sgrn::json::dom::StringBuffer sb;
        sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> w(sb);
        emitPayload(w, seedAt(i));
        doNotOptimize(sb);
        g_sink += static_cast<uint64_t>(sb.size());
    });

    // reference output size for the bandwidth figure
    rapidjson::StringBuffer ref;
    rapidjson::Writer<rapidjson::StringBuffer> rw(ref);
    RapidJsonAdapter arw{rw};
    emitPayload(arw, kSeeds[0]);
    const double bytes = static_cast<double>(ref.GetSize());
    std::printf("  payload: %.1f KB\n", bytes / 1024.0);
    report("rapidjson Writer", rj, bytes);
    report("dom::Writer (glaze)", dom, bytes);
}

void benchWriteReuse() {
    std::printf("build: reused StringBuffer (TelemetryBroker pattern)\n");

    rapidjson::StringBuffer rjBuf;
    double rj = bestOf(2000, 5, [&](int i) {
        rjBuf.Clear();
        rapidjson::Writer<rapidjson::StringBuffer> w(rjBuf);
        RapidJsonAdapter aw{w};
        emitPayload(aw, seedAt(i));
        doNotOptimize(rjBuf);
        g_sink += rjBuf.GetSize();
    });

    sgrn::json::dom::StringBuffer domBuf;
    double dom = bestOf(2000, 5, [&](int i) {
        domBuf.clear();
        sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> w(domBuf);
        emitPayload(w, seedAt(i));
        doNotOptimize(domBuf);
        g_sink += static_cast<uint64_t>(domBuf.size());
    });

    rapidjson::StringBuffer ref;
    rapidjson::Writer<rapidjson::StringBuffer> rw(ref);
    RapidJsonAdapter arw{rw};
    emitPayload(arw, kSeeds[0]);
    const double bytes = static_cast<double>(ref.GetSize());
    report("rapidjson Writer", rj, bytes);
    report("dom::Writer (glaze)", dom, bytes);
}

void benchAccept() {
    std::printf("accept: serialize an already-built tree (Registry snapshot pattern)\n");

    rapidjson::StringBuffer ref;
    rapidjson::Writer<rapidjson::StringBuffer> rw(ref);
    RapidJsonAdapter arw{rw};
    emitPayload(arw, kSeeds[0]);

    rapidjson::Document rjDoc;
    rjDoc.Parse(ref.GetString(), ref.GetSize());

    sgrn::json::dom::Document domDoc;
    domDoc.parse(ref.GetString(), ref.GetSize());

    double rj = bestOf(2000, 5, [&](int) {
        rapidjson::StringBuffer sb;
        rapidjson::Writer<rapidjson::StringBuffer> w(sb);
        rjDoc.Accept(w);
        doNotOptimize(sb);
        g_sink += sb.GetSize();
    });

    double dom = bestOf(2000, 5, [&](int) {
        sgrn::json::dom::StringBuffer sb;
        sgrn::json::dom::Writer<sgrn::json::dom::StringBuffer> w(sb);
        domDoc.accept(w);
        doNotOptimize(sb);
        g_sink += static_cast<uint64_t>(sb.size());
    });

    const double bytes = static_cast<double>(ref.GetSize());
    report("rapidjson Accept", rj, bytes);
    report("dom accept (glaze)", dom, bytes);
}

void benchParse() {
    std::printf("parse + read: json-patch / HTTP handler pattern\n");

    rapidjson::StringBuffer ref;
    rapidjson::Writer<rapidjson::StringBuffer> rw(ref);
    RapidJsonAdapter arw{rw};
    emitPayload(arw, kSeeds[0]);

    const char* json = ref.GetString();
    const auto size = static_cast<size_t>(ref.GetSize());
    std::printf("  payload: %.1f KB\n", static_cast<double>(size) / 1024.0);

    double rj = bestOf(2000, 5, [&](int) {
        rapidjson::Document d;
        d.Parse(json, size);
        g_sink += walkRapidJson(d);
        doNotOptimize(d);
    });

    double dom = bestOf(2000, 5, [&](int) {
        sgrn::json::dom::Document d;
        d.parse(json, size);
        g_sink += walkDom(d);
        doNotOptimize(d);
    });

    report("rapidjson Parse", rj, static_cast<double>(size));
    report("dom::Document::parse (glaze)", dom, static_cast<double>(size));
}

} // namespace

int main() {
    std::printf("sgrn::json::dom (glaze) vs rapidjson — ns/call, best of 5, lower is better\n");
    benchWriteFresh();
    benchWriteReuse();
    benchAccept();
    benchParse();
    std::printf("sink: %llu\n", static_cast<unsigned long long>(g_sink));
    return 0;
}
