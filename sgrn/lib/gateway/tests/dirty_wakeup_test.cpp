// Lost-wakeup regression test for PlcCommandProcessor::processDirty().
//
// Each round, producer threads hammer signalDirty() while a memory writer
// thread streams an in-order sequence through writeDbMemory(), and the
// heavy_pool dirty callback records the newest value it observed. After the
// producers stop, the test quiesces and asserts the round's final value was
// observed: every dirty batch must reach the callback.
//
// Failure mode under test: a signalDirty() landing between the recheck-flag
// consumption and the single-flight release (in the heavy_pool completion
// handler), or between checkDirty() and the release (in the idle early
// return), posts nothing (single-flight looks busy) while its dirty state is
// then discarded — orphaning the batch until some later write happens to
// re-signal. When that interleaving swallows the tail of a burst, the final
// value is never observed and telemetry stalls on an idle plant. Hammering
// many rounds keeps a completion (or idle pass) in flight at every burst
// tail, so the race cannot hide for long.
//
// The heavy callback reads the value and clears the segment dirty flag under
// the segment's shared lock (the same atomicity the production
// getDeltaSnapshot() sweep provides), so a clear can never slip between a
// write's memcpy and its markDirty.

#include <sgrn/gateway/twin/PlcCommandProcessor.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/PlcState.hpp>

#include <asio.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <shared_mutex>
#include <thread>
#include <vector>

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

using sgrn::gateway::twin::PlcMemory;
using sgrn::gateway::twin::PlcState;

constexpr uint16_t kDb = 7;
constexpr size_t kDbSize = 16;
constexpr int kRounds = 60;
constexpr int kStormThreads = 4;
constexpr int kStormSignalsPerThread = 10000;
constexpr int kOrderedWrites = 300; // single writer per round: final value is deterministic
constexpr int kRoundTimeoutMs = 2000;

void writeValue(PlcMemory& t_mem, uint32_t t_value) {
    uint8_t buf[4];
    std::memcpy(buf, &t_value, sizeof(buf)); // LE on both sides; byte-exact is all that matters
    if (t_mem.writeDbMemory(kDb, 0, sizeof(buf), buf).hasError()) {
        ++g_failures;
        std::printf("FAIL writeDbMemory(%u) failed\n", t_value);
    }
}

} // namespace

int main() {
    PlcState state;
    PlcMemory mem;
    mem.attachState(state);
    CHECK(!mem.registerDb(kDb, kDbSize).hasError());
    if (g_failures != 0)
        return 1;

    asio::io_context light_ctx;
    auto light_guard = asio::make_work_guard(light_ctx);
    std::thread light_thread([&]() { light_ctx.run(); });
    asio::thread_pool heavy_pool(2);

    mem.processor()->setContexts(&light_ctx, &heavy_pool);

    std::atomic<uint32_t> observed{0};
    std::atomic<int> callbacks{0};

    // Production-shaped consumer: observe the newest value, then clear the
    // dirty flag while still holding the segment lock.
    mem.processor()->setDirtyHandler([&](std::vector<uint16_t> t_dbs) {
        for (uint16_t db : t_dbs) {
            auto* p_seg = state.findSegmentById(db);
            if (!p_seg)
                continue;
            std::shared_lock<std::shared_mutex> lk(p_seg->mutex_);
            uint32_t value = 0;
            std::memcpy(&value, state.arenaData() + p_seg->offset, sizeof(value));
            uint32_t prev = observed.load(std::memory_order_relaxed);
            while (value > prev && !observed.compare_exchange_weak(prev, value, std::memory_order_relaxed)) {
            }
            p_seg->getAndClearDirty();
        }
        callbacks.fetch_add(1, std::memory_order_relaxed);
    });

    for (int round = 0; round < kRounds && g_failures == 0; ++round) {
        observed.store(0, std::memory_order_relaxed);
        std::atomic<bool> go{false};

        // Raw signalDirty() storm: maximum collision rate against the
        // recheck/scheduled sequences under test.
        std::vector<std::thread> storm;
        for (int t = 0; t < kStormThreads; ++t) {
            storm.emplace_back([&]() {
                while (!go.load(std::memory_order_acquire)) {
                }
                for (int i = 0; i < kStormSignalsPerThread; ++i)
                    mem.processor()->signalDirty();
            });
        }

        // Single ordered writer: every value is unique and increasing, so
        // the round's final memory value — and therefore the required
        // observation — is deterministic (no cross-thread inversion).
        std::thread writer([&]() {
            while (!go.load(std::memory_order_acquire)) {
            }
            for (int seq = 1; seq <= kOrderedWrites; ++seq)
                writeValue(mem, static_cast<uint32_t>(seq));
        });

        go.store(true, std::memory_order_release);
        writer.join();
        for (auto& t : storm)
            t.join();
        if (g_failures != 0)
            break;

        // Quiesce: the final value must be observed. An orphaned tail shows
        // up here — nothing after it can rescue it.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kRoundTimeoutMs);
        while (observed.load(std::memory_order_relaxed) != static_cast<uint32_t>(kOrderedWrites)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::printf("FAIL round %d: final value %d never observed (observed=%u callbacks=%d) — lost wakeup\n", round,
                    kOrderedWrites, observed.load(std::memory_order_relaxed), callbacks.load(std::memory_order_relaxed));
                ++g_failures;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    light_guard.reset();
    light_ctx.stop();
    if (light_thread.joinable())
        light_thread.join();
    heavy_pool.join();

    if (g_failures == 0)
        std::printf("dirty_wakeup_test: ALL CHECKS PASSED (%d rounds x (%d storm signals + %d ordered writes), %d callbacks)\n", kRounds,
            kStormThreads * kStormSignalsPerThread, kOrderedWrites, callbacks.load());
    else
        std::printf("dirty_wakeup_test: %d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
