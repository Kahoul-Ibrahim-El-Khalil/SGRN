#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/common/ErrorClass.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace sgrn::common
{

/// One DB-scoped byte range for the batch read API. Mirrors the twin's
/// DbMemorySpan field-for-field, but lives here so protocol adapters can be
/// built without including any twin header. Buffers stay caller-owned;
/// spans only borrow the pointers for the duration of the call.
struct DbMemorySpan {
    uint16_t db;
    size_t offset;
    size_t size;
    uint8_t* p_buffer;
};

/**
 * @brief Abstract memory-access port for protocol adapters.
 *
 * Dependency-inversion boundary between the southbound protocol adapters
 * (s7, modbus, ethernetip) and whatever backs plant memory — in production
 * the gateway's PlcMemory (see sgrn/gateway/adapters/ports/TwinPorts.hpp),
 * in tests a fake. Failures use the protocol-agnostic ErrorClass vocabulary
 * so adapters never name a backend error enum.
 *
 * All methods are thread-safe to call (the production twin serializes
 * internally); batch reads resolve and lock each touched segment once.
 */
struct IMemoryPort {
    virtual ~IMemoryPort() = default;

    /// False before the backing store is attached (reads/writes fail).
    virtual bool isReady() const = 0;
    /// DB numbers currently present; empty when not ready.
    virtual std::vector<uint16_t> topLevelDbNumbers() const = 0;
    /// Byte size of a DB, or nullopt when missing/not ready.
    virtual std::optional<size_t> dbSize(uint16_t t_db_number) const = 0;

    /// Read [t_offset, t_offset + t_size) of one DB into t_buffer.
    virtual sgrn::Result<void, ErrorClass> readDbMemory(uint16_t t_db_number, size_t t_offset, size_t t_size, uint8_t* tp_buffer) = 0;
    /// Batch variant: spans may name different DBs.
    virtual sgrn::Result<void, ErrorClass> readDbMemory(std::span<const DbMemorySpan> t_spans) = 0;
    /// Write t_buffer into [t_offset, t_offset + t_size) of one DB.
    virtual sgrn::Result<void, ErrorClass> writeDbMemory(
        uint16_t t_db_number, size_t t_offset, size_t t_size, const uint8_t* tp_buffer) = 0;
    /// Write one bit of a DB byte.
    virtual sgrn::Result<void, ErrorClass> writeBit(uint16_t t_db_number, size_t t_byte_offset, int t_bit_index, bool t_value) = 0;
    /// Schema-checked field write from a JSON value (marks dirty/telemetry).
    virtual sgrn::Result<void, ErrorClass> updateField(
        uint16_t t_db_number, const std::string& t_field_path, const std::string& t_value_json) = 0;
    /// Drain queued field writes so they become visible to reads. Backends
    /// that queue updateField() (command-queue twin) apply them here; raw
    /// writeDbMemory()/writeBit() apply synchronously and need no flush.
    /// Call once after a batch of updateField() calls, not per field.
    virtual void flushCommands() = 0;
    /**
     * @brief Monotonic per-DB write generation for pollers.
     *
     * Bumped on EVERY write to the DB (raw or field-level). Consumers keep
     * the last-seen value per DB and skip re-reading while it matches —
     * seqlock discipline: record the generation read BEFORE syncing, never
     * after, so a write racing the sync only ever causes one redundant
     * re-sync, never a permanently missed update. 0 means "unknown"
     * (backend cannot track it): treat as always-dirty and sync.
     */
    virtual uint64_t dbVersion(uint16_t t_db_number) const = 0;
};

} // namespace sgrn::common
