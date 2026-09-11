// =============================================================================
// PersistenceBridge.cpp — bridges PlcRuntime dirty events → TelemetryBroker
// =============================================================================

#include <sgrn/plcsim/runtime/PersistenceBridge.hpp>

#include <sgrn/gateway/core/GlobalContext.hpp>
#include <sgrn/gateway/core/TelemetryBroker.hpp>
#include <sgrn/gateway/database/GatewayDatabase.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/twin/PlcState.hpp>
#include <sgrn/plcsim/utils/PlcSimClock.hpp>

#include <fmt/color.h>
#include <fmt/format.h>
#include <stdexcept>
#include <string>

#include <filesystem>
#include <stdexcept>

namespace sgrn::plcsim::persistence
{

namespace fs = std::filesystem;
using sgrn::gateway::config::PersistenceConfig;
using sgrn::gateway::core::EventType;
using sgrn::gateway::core::GlobalContext;
using sgrn::gateway::core::TelemetryBroker;
using sgrn::gateway::core::TelemetryEvent;
using sgrn::gateway::database::FullDbReadFn;
using sgrn::gateway::database::PersistenceService;
using sgrn::gateway::twin::PlcMemory;

// ─────────────────────────────────────────────────────────────────────────────

PersistenceBridge::PersistenceBridge(std::shared_ptr<runtime::PlcRuntime> tsp_runtime)
    : runtime_(std::move(tsp_runtime)) {
}

PersistenceBridge::~PersistenceBridge() {
    if (active_.load(std::memory_order_relaxed))
        stop();
}

void PersistenceBridge::configure(const PersistenceBridgeConfig& t_cfg) {
    if (active_.load(std::memory_order_relaxed)) {
        fmt::print(stderr, fg(fmt::color::yellow), "[PersistenceBridge] configure() called while active — ignored.\n");
        return;
    }
    cfg_ = t_cfg;
}

// ─────────────────────────────────────────────────────────────────────────────
// ensureIoContext
// ─────────────────────────────────────────────────────────────────────────────

void PersistenceBridge::ensureIoContext() {
    // GlobalContext is a singleton. If the gateway process has already called
    // run(), it is already running — we do nothing and leave io_context_started_
    // false so stop() doesn't call GlobalContext::stop().
    //
    // If this is a pure S7Shell process (no gateway), no-one has called run()
    // yet, so we do it here and set io_context_started_ = true.
    //
    // We detect "already running" by checking whether the io_context has been
    // stopped (it hasn't even been started if no work guard exists yet). The
    // cleanest heuristic: try posting a trivial task; if it returns 0 handlers
    // run, the context is either stopped or has no work — start it ourselves.
    // Actually the simplest approach: keep a static flag.
    static std::atomic<bool> s_ctx_running{false};
    bool expected = false;
    if (s_ctx_running.compare_exchange_strong(expected, true)) {
        GlobalContext::instance().run(/*io_threads=*/1, /*worker_threads=*/1);
        io_context_started_ = true;
        fmt::print(fg(fmt::color::cyan), "[PersistenceBridge] Started GlobalContext io_context (1 thread).\n");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// makeDbReader — lambda that reads a full DB image from PlcMemory
// ─────────────────────────────────────────────────────────────────────────────

FullDbReadFn PersistenceBridge::makeDbReader() const {
    // Capture a weak_ptr to avoid keeping the runtime alive through the
    // PersistenceService lambda after stop() has been called.
    std::weak_ptr<runtime::PlcRuntime> weak_rt = runtime_;
    return [weak_rt](uint16_t t_db) -> sgrn::Result<std::vector<uint8_t>> {
        auto rt = weak_rt.lock();
        if (!rt)
            return sgrn::Result<std::vector<uint8_t>>::Error("runtime expired");

        // Look up the DB size from the schema.
        auto& schema = rt->getSchema();
        auto db_res = schema.getDb(t_db);
        if (db_res.hasError() || !db_res.value() || db_res.value()->size_bytes <= 0)
            return sgrn::Result<std::vector<uint8_t>>::Error(fmt::format("DB {} not found in schema", t_db));

        const size_t db_size = static_cast<size_t>(db_res.value()->size_bytes);
        std::vector<uint8_t> buf(db_size, 0);

        auto& mem = rt->getMemory();
        auto read_res = mem.readDbMemory(t_db, 0, db_size, buf.data());
        if (read_res.hasError())
            return sgrn::Result<std::vector<uint8_t>>::Error(
                fmt::format("readDbMemory DB{} failed: {}", t_db, static_cast<int>(read_res.error())));

        return sgrn::Result<std::vector<uint8_t>>(std::move(buf));
    };
}

// ─────────────────────────────────────────────────────────────────────────────
// start
// ─────────────────────────────────────────────────────────────────────────────

void PersistenceBridge::start() {
    if (active_.exchange(true)) {
        fmt::print(stderr, fg(fmt::color::yellow), "[PersistenceBridge] start() called when already active — ignored.\n");
        return;
    }

    // 1. Ensure the io_context / TelemetryBroker strand is running.
    ensureIoContext();

    // 2. Create output directories.
    const fs::path state_dir = fs::path(cfg_.out_dir) / "state";
    const fs::path unsynced = state_dir / "unsynced";
    fs::create_directories(unsynced);

    // 3. Build PersistenceConfig from our simplified config.
    PersistenceConfig pcfg;
    pcfg.enabled = true;
    pcfg.format = cfg_.format;
    pcfg.mode = cfg_.mode;
    pcfg.atomic_window_ms = cfg_.atomic_window_ms;
    pcfg.batch_interval_s = cfg_.rotation_interval_s;
    pcfg.anchor_interval_s = cfg_.anchor_interval_s;
    pcfg.anchor_change_count = 0; // disable count-based anchoring for shell use

    // 4. Build schema JSON string for the WAL header.
    std::string schema_json = runtime_->getSchema().toJson(std::nullopt, false, true);

    // 5. Instantiate PersistenceService.
    //    Pass nullptr for GatewayDatabase — the shell doesn't need DB upload
    //    tracking; we only need local WAL files.
    persistence_ = std::make_unique<PersistenceService>(&GlobalContext::instance().worker_pool());

    auto res = persistence_->configure(pcfg, state_dir.string(),
        /*db=*/nullptr, schema_json,
        /*schema_store=*/&runtime_->getSchema(), makeDbReader());

    if (res.hasError()) {
        fmt::print(stderr, fg(fmt::color::red), "[PersistenceBridge] PersistenceService configure failed: {}\n", res.error());
        persistence_.reset();
        active_.store(false);
        return;
    }

    // 6. Register dirty observer on the runtime.
    //    The observer fires on any thread that calls markDirty().
    //    It reads the current delta from PlcMemory and publishes a
    //    DeltaSnapshot TelemetryEvent to the broker.
    observer_id_ = runtime_->addDirtyObserver([this](uint16_t db, uint32_t offset, uint32_t length) { onDirty(db, offset, length); });

    fmt::print(
        fg(fmt::color::green), "[PersistenceBridge] Recording to {} (format={}, mode={})\n", unsynced.string(), cfg_.format, cfg_.mode);
}

// ─────────────────────────────────────────────────────────────────────────────
// onDirty — called from PlcRuntime dirty observer
// ─────────────────────────────────────────────────────────────────────────────

void PersistenceBridge::onDirty(uint16_t t_db_num, uint32_t /*t_offset*/, uint32_t /*t_length*/) {
    if (!persistence_ || !active_.load(std::memory_order_relaxed))
        return;

    // Use the simulated clock for timestamps so replayed data has temporally
    // coherent timestamps (monotonically increasing at timestep_ms intervals)
    // rather than real wall-clock time which would cluster all events at the
    // same instant during fast-forwarded simulation.
    const uint64_t sim_ts = ::sgrn::plcsim::utils::g_plc_clock.nowMs();

    TelemetryEvent evt;
    evt.type = EventType::DeltaSnapshot;
    evt.db = t_db_num;
    evt.is_flat = false;
    evt.timestamp = sim_ts;

    // For JSONL mode: attach the full DB JSON so PersistenceService can extract
    // individual leaf paths and values.
    // For binary mode: PersistenceService calls read_db_fn_(db) directly and
    // does NOT use json_value — so we only build the JSON string when needed.
    if (cfg_.format != "binary" && cfg_.format != "bin.zst") {
        auto& mem = runtime_->getMemory();
        std::string delta_json = mem.getDbJsonString(t_db_num);
        if (delta_json.empty())
            return;
        evt.json_value = std::make_shared<std::string>(std::move(delta_json));
    }

    TelemetryBroker::instance().publish(std::move(evt));
}

// ─────────────────────────────────────────────────────────────────────────────
// flush / stop
// ─────────────────────────────────────────────────────────────────────────────

void PersistenceBridge::flush() {
    if (!active_.load(std::memory_order_relaxed) || !persistence_) {
        fmt::print(stderr, fg(fmt::color::yellow), "[PersistenceBridge] flush() called while not active — ignored.\n");
        return;
    }
    // PersistenceService does not expose a public flush(); the WAL rotates
    // automatically on batch_interval_s. For an explicit rotation, inject a
    // FullSnapshot event — PersistenceService treats it as an anchor and
    // initiates a file rotation.
    auto& mem = runtime_->getMemory();
    std::string full_json = mem.getDigitalTwinJsonString();
    std::string flat_json; // not used in JSONL mode
    persistence_->ingestFullTree(full_json, flat_json);
    fmt::print(fg(fmt::color::green), "[PersistenceBridge] WAL anchor written.\n");
}

void PersistenceBridge::stop() {
    if (!active_.exchange(false))
        return;

    // Unregister dirty observer first so no new events fire after stop.
    if (observer_id_ != 0) {
        runtime_->removeDirtyObserver(observer_id_);
        observer_id_ = 0;
    }

    // Finalise the WAL (writes footer, rotates file).
    if (persistence_) {
        persistence_->stop();
        persistence_.reset();
    }

    // Stop the io_context if WE started it.
    if (io_context_started_) {
        GlobalContext::instance().stop();
        io_context_started_ = false;
    }

    fmt::print(fg(fmt::color::green), "[PersistenceBridge] Recording stopped.\n");
}

} // namespace sgrn::plcsim::persistence
