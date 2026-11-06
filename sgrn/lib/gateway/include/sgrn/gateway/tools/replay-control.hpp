#pragma once
// ReplayControl — runtime replay pacing state shared between the replay
// thread (GatewayReplayer) and the HTTP handlers (HttpAdapter /replay/*).
//
// SIMULATED DATA ONLY. No real-plant semantics.
//
// Cheapest sync that fits: replay thread is the only writer of cur_ts/frames
// (fetch_add/store relaxed); HTTP handlers and any thread may store
// speed/paused/unpaced (relaxed) and load status. Hot path does one relaxed
// load per frame — no mutex, no lock contention.
#include <atomic>
#include <cstdint>
#include <memory>

namespace sgrn::gateway
{

struct ReplayControl {
    // Simulated seconds per wall-clock second. 1.0 = real time.
    // <=0 is treated as paused pacing (no sleep debt accumulates).
    std::atomic<double> speed{1.0};
    std::atomic<bool> paused{false};
    // Explicit unpaced mode (== --no-delay): skip all timestamp sleeps.
    std::atomic<bool> unpaced{false};
    // Last archive ts handed to the twin (ms, simulated time base).
    std::atomic<int64_t> cur_ts{-1};
    std::atomic<uint64_t> frames{0};
};

using ReplayControlPtr = std::shared_ptr<ReplayControl>;

} // namespace sgrn::gateway
