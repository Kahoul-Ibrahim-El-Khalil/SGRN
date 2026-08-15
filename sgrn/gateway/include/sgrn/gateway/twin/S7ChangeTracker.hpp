#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// S7ChangeTracker.hpp  –  sgrn/s7 domain
//
// Reusable, thread-safe dirty-tracking component.
//
// Replaces the ad-hoc dirty flags previously scattered across:
//   • DbEntry::is_dirty
//   • DbSnapshot::is_dirty_
//   • PlcTagTable::Block (unnamed atomics)
//
// Usage pattern:
//   S7ChangeTracker tracker;
//   tracker.captureBaseline(initial_bytes, size);        // after first PLC read
//   ...
//   if (tracker.checkDirty(current_bytes, size)) { ... } // compare vs baseline
//   tracker.markClean();                                  // after successful PLC write
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

class S7ChangeTracker {
public:
    S7ChangeTracker() = default;

    /// Pre-allocate a baseline of a known size (optional optimisation).
    explicit S7ChangeTracker(size_t t_baseline_size)
        : baseline_(t_baseline_size, 0u) {
    }

    // ── Baseline management ─────────────────────────────────────────────────

    /// Capture data as the new clean baseline and clear the dirty flag.
    void captureBaseline(const uint8_t* tp_data, size_t t_size) {
        baseline_.assign(tp_data, tp_data + t_size);
        dirty_.store(false, std::memory_order_release);
    }

    void captureBaseline(const std::vector<uint8_t>& t_data) {
        captureBaseline(t_data.data(), t_data.size());
    }

    // ── Dirty detection ─────────────────────────────────────────────────────

    /// Compare current against an explicit baseline.
    /// Sets the dirty flag when a difference is found; returns true if dirty.
    bool checkDirty(const uint8_t* tp_current, const uint8_t* tp_baseline, size_t t_size) {
        bool changed = (baseline_.size() != t_size) || (std::memcmp(tp_current, tp_baseline, t_size) != 0);
        if (changed)
            dirty_.store(true, std::memory_order_release);
        return changed;
    }

    /// Compare current against the internally stored baseline.
    bool checkDirty(const uint8_t* tp_current, size_t t_size) {
        if (baseline_.empty()) {
            // No baseline yet – treat as dirty so the first write always goes.
            dirty_.store(true, std::memory_order_release);
            return true;
        }
        return checkDirty(tp_current, baseline_.data(), t_size);
    }

    bool checkDirty(const std::vector<uint8_t>& t_current) {
        return checkDirty(t_current.data(), t_current.size());
    }

    // ── Flag manipulation ───────────────────────────────────────────────────

    void markDirty() {
        dirty_.store(true, std::memory_order_release);
    }
    void markClean() {
        dirty_.store(false, std::memory_order_release);
    }

    bool isDirty() const {
        return dirty_.load(std::memory_order_acquire);
    }

    /// Atomically read and clear the dirty flag.  Returns the previous value.
    bool getAndClearDirty() {
        return dirty_.exchange(false, std::memory_order_acq_rel);
    }

    // ── Accessors ───────────────────────────────────────────────────────────

    const std::vector<uint8_t>& tp_baseline() const noexcept {
        return baseline_;
    }
    size_t baseline_size() const noexcept {
        return baseline_.size();
    }

private:
    std::atomic<bool> dirty_{false};
    std::vector<uint8_t> baseline_;
};
