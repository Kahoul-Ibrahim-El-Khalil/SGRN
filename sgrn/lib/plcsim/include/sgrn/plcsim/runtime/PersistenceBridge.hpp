#pragma once
// =============================================================================
// PersistenceBridge — bridges PlcRuntime dirty observers → TelemetryBroker
//
// Connects an S7Shell PlcRuntime to the canonical SGRN persistence pipeline.
// When active, every PlcRuntime::markDirty() call (from scripts, SimEngine,
// proxy deltas, etc.) is serialised and published as a TelemetryEvent into
// TelemetryBroker::instance(), where PersistenceService consumes it exactly
// as it would a real S7 adapter event.
//
// Architectural invariant:
//   Synthetic telemetry (S7Shell) → PersistenceBridge → TelemetryBroker
//   Real S7 telemetry            → PlcMemory           → TelemetryBroker
//   Both produce identical persisted representations.
//
// Lifecycle:
//   1. Construct with a shared PlcRuntime.
//   2. Call configure() with the output directory and desired options.
//   3. Call start() — registers the dirty observer and starts io_context.
//   4. Run simulation / scripts. Every markDirty() triggers serialization.
//   5. Call flush() at any time to force WAL rotation.
//   6. Call stop() when done — unregisters the observer and finalises WAL.
// =============================================================================

#include <sgrn/gateway/config/persistence.hpp>
#include <sgrn/gateway/database/PersistenceService.hpp>
#include <sgrn/plcsim/runtime/PlcRuntime.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace sgrn::plcsim::persistence
{

/**
 * @brief Configuration for PersistenceBridge.
 *
 * A simplified view over PersistenceConfig that exposes only the knobs
 * that make sense for scripted simulation use-cases (output path, format,
 * mode). The full PersistenceConfig is constructed from these fields.
 */
struct PersistenceBridgeConfig {
    /// Output directory. WAL archives are written under <out_dir>/unsynced/.
    std::string out_dir{"."};

    /// WAL format: "binary" (default, compact .bin.zst) or "jsonl" (.jsonl.zst).
    std::string format{"binary"};

    /// Archive mode: "changes_with_timestamp" | "full_tree" | "full_tree_with_anchor".
    std::string mode{"changes_with_timestamp"};

    /// Atomic merge window in milliseconds (0 = no merging).
    uint32_t atomic_window_ms{10};

    /// How many seconds between WAL rotations (batch_interval_s).
    uint32_t rotation_interval_s{300};

    /// Anchor interval in seconds for full_tree_with_anchor mode.
    uint32_t anchor_interval_s{3600};
};

/**
 * @brief Bridges PlcRuntime dirty-region events into the canonical
 *        TelemetryBroker → PersistenceService pipeline.
 *
 * Not copyable or movable; holds a shared_ptr to the runtime and owns
 * the PersistenceService instance. Designed to be heap-allocated and
 * wrapped by a ref-counted AngelScript handle (PersistenceBridgeWrapper).
 */
class PersistenceBridge {
public:
    explicit PersistenceBridge(std::shared_ptr<runtime::PlcRuntime> tsp_runtime);
    ~PersistenceBridge();

    PersistenceBridge(const PersistenceBridge&) = delete;
    PersistenceBridge& operator=(const PersistenceBridge&) = delete;

    // ── Lifecycle ────────────────────────────────────────────────────────────

    /**
     * @brief Configure the output directory and WAL options.
     * Must be called before start(). Can be called multiple times before
     * start() to change options; calling after start() is a no-op.
     */
    void configure(const PersistenceBridgeConfig& t_cfg);

    /**
     * @brief Start recording.
     *
     * Ensures GlobalContext::instance().run() has been called (starts the
     * io_context if not yet running), instantiates PersistenceService,
     * subscribes it to TelemetryBroker, and registers the dirty observer
     * on the runtime.
     */
    void start();

    /**
     * @brief Force WAL rotation — finalises the current archive and opens
     * a fresh one. Safe to call from a script at any time while active.
     */
    void flush();

    /**
     * @brief Stop recording and finalise the WAL archive.
     *
     * Unregisters the dirty observer, stops PersistenceService (finalises
     * the archive), and drains in-flight io_context tasks. After stop(),
     * archive files are complete and readable by sgrn_dataset / RecoveryEngine.
     */
    void stop();

    /// True if start() has been called and stop() has not.
    bool isActive() const {
        return active_.load(std::memory_order_relaxed);
    }

    /// Path to the output directory (as set by configure()).
    std::string outDir() const {
        return cfg_.out_dir;
    }

private:
    // Called from dirty observer (any thread, post PlcRuntime::markDirty).
    // Reads the modified DB region from PlcMemory and publishes a
    // TelemetryEvent (DeltaSnapshot) to TelemetryBroker.
    void onDirty(uint16_t t_db_num, uint32_t t_offset, uint32_t t_length);

    // Ensures GlobalContext io_context threads are running.
    // Called once inside start(); sets io_context_started_ = true if WE
    // launched them (so stop() knows to call GlobalContext::stop() too).
    void ensureIoContext();

    // Builds a FullDbReadFn lambda that reads from our PlcMemory.
    gateway::database::FullDbReadFn makeDbReader() const;

    // ── Members ──────────────────────────────────────────────────────────────
    std::shared_ptr<runtime::PlcRuntime> runtime_;
    PersistenceBridgeConfig cfg_;

    // PersistenceService consumes events on the TelemetryBroker strand.
    std::unique_ptr<gateway::database::PersistenceService> persistence_;

    // Observer handle — used to unregister on stop().
    size_t observer_id_{0};
    std::atomic<bool> active_{false};
    bool io_context_started_{false}; ///< true if WE called GlobalContext::run()
};

} // namespace sgrn::plcsim::persistence
