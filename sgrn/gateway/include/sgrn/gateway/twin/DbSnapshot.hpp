#pragma once
// REVAMP-1: DbSnapshot.hpp — renamed from DataBlock.hpp

#include <sgrn/gateway/wrappers/s7/S7Client.hpp>
#include <sgrn/scl/types.hpp>
#include <sgrn/structures/DoubleBuffer.hpp>
#include <atomic>
#include <chrono>
#include <mutex>

namespace sgrn::gateway::twin
{
using ::sgrn::gateway::wrappers::s7::S7Client;
using ::sgrn::scl::DataType;
using ::sgrn::scl::DbData;
using ::sgrn::scl::DbField;
using ::sgrn::scl::DbRawBuffer;
using ::sgrn::scl::DbSchema;
using ::sgrn::scl::ErrorCode;

// ---------------------------------------------------------------------------
// Free functions — decodeValue / encodeValue (BUG-6)
// Not static members; they don't need 'this'.
// ---------------------------------------------------------------------------

/**
 * @brief Decode a typed field or array from raw bytes using S7 encoding rules.
 *
 * Free function (BUG-6: was wrongly declared static member but implemented as non-static).
 */
sgrn::Result<std::string, ::sgrn::scl::Error> decodeValue(const DbField& t_field, const uint8_t* tp_buffer_ptr, size_t t_buffer_size);

/**
 * @brief Encode a typed field or array into raw bytes using S7 encoding rules.
 *
 * Free function (BUG-6).
 */
sgrn::Result<void, ::sgrn::scl::Error> encodeValue(
    const DbField& t_field, const std::string& t_value, uint8_t* tp_buffer_ptr, size_t t_buffer_size);

// ---------------------------------------------------------------------------
// DbSnapshot — replaces DataBlock
// ---------------------------------------------------------------------------

/**
 * @brief Per-call snapshot buffer for S7 DB read/write operations.
 *
 * Renamed from DataBlock. Owns a heap-allocated raw buffer matching a DbSchema.
 * Use readCache_ in S7Connection to reuse instances across calls. (Section 3.3)
 */
class DbSnapshot {
public:
    explicit DbSnapshot(DbSchema t_registry); // REVAMP-5: DbSchema (was DataBlockRegistry)
    ~DbSnapshot();

    // Manual copy/move due to std::atomic member
    DbSnapshot(const DbSnapshot& t_other);
    DbSnapshot& operator=(const DbSnapshot& t_other);
    DbSnapshot(DbSnapshot&& t_other) noexcept;
    DbSnapshot& operator=(DbSnapshot&& t_other) noexcept;

    /// Reads the entire DB span from the PLC into the back buffer
    sgrn::Result<void, ::sgrn::scl::Error> read(S7Client& t_client);
    sgrn::Result<void, ::sgrn::scl::Error> beginAsyncRead(S7Client& t_client);

    /**
     * @brief Commit the results of an async read.
     *
     * Swaps the back buffer (where Snap7 wrote) into the front buffer
     * (where the API reads). Updates last_sync_time.
     */
    void commitAsyncRead();

    /// Writes the current front buffer back to the PLC
    sgrn::Result<void, ::sgrn::scl::Error> write(S7Client& t_client);

    /// Decodes the current raw_buffer_ into JSON (supports nested UDTs)
    sgrn::Result<DbData, ::sgrn::scl::Error> decode() const; // REVAMP-8: DbData (was DataBlockData)

    /// Encodes a single top-level field by name into the raw_buffer_.
    sgrn::Result<void, ::sgrn::scl::Error> updateField(const std::string& t_field_name, const std::string& t_value);

    /**
     * @brief Apply a JSON patch string to the raw buffer using RapidJSON.
     *
     * High-performance alternative to the Jsoncpp version.
     */
    sgrn::Result<void, ::sgrn::scl::Error> updateFromJson(const std::string& t_json_patch);

    /// Returns the timestamp of the last successful PLC sync
    std::chrono::system_clock::time_point lastSyncTime() const {
        return std::chrono::system_clock::time_point(std::chrono::milliseconds(last_sync_time_ms_.load()));
    }

    /// Returns true if local raw_buffer_ has been modified but not written back
    bool isDirty() const {
        return is_dirty_;
    }

    /// Reads a single field from the PLC, updating only those bytes in raw_buffer_
    sgrn::Result<void, ::sgrn::scl::Error> readField(S7Client& t_client, const std::string& t_field_path);

    /// Writes a single field to the PLC, encoding it first into raw_buffer_
    sgrn::Result<void, ::sgrn::scl::Error> writeField(S7Client& t_client, const std::string& t_field_path, const std::string& t_value);

    /// Decodes a single field from the current raw_buffer_
    sgrn::Result<std::string, ::sgrn::scl::Error> getFieldValue(const std::string& t_field_path) const;

    /**
     * @brief Patch a raw byte region in both buffers from an inbound server-side S7 PUT write.
     *
     * Called by PlcMemory::patchSnapshotRegion() immediately after writeMemory() succeeds,
     * so that any consumer using this DbSnapshot (e.g. DbIOProvider polling sessions) sees
     * the write instead of the last-polled state. Mirrors the two-buffer sync pattern used
     * in updateField() and writeField().
     *
     * Thread-safety: acquires state_mutex_ internally. If an async PLC read is in-flight the
     * patch is skipped with a warning, because commitAsyncRead() is about to overwrite the
     * back-buffer anyway and the arena already carries the authoritative value.
     *
     * @param offset  Byte offset relative to the start of this DB's buffer.
     * @param src     Source bytes to copy (must be at least @p bytes long).
     * @param bytes   Number of bytes to copy.
     */
    void updateRawRegion(size_t t_offset, const uint8_t* tp_src, size_t t_bytes);
    const DbSchema& getRegistry() const {
        return registry_;
    }
    const DbRawBuffer& getRawBuffer() const {
        return raw_buffer_.front();
    }

private:
    DbSchema registry_;
    DoubleBuffer<uint8_t> raw_buffer_;
    mutable std::mutex state_mutex_;
    std::atomic<bool> async_in_progress_{false};
    std::vector<uint8_t> async_buffer_;
    S7Client* last_client_{nullptr};
    std::atomic<int64_t> last_sync_time_ms_{0};
    std::atomic<bool> is_dirty_{false};
};

// ---------------------------------------------------------------------------
// Backward-compat alias
// ---------------------------------------------------------------------------
using DataBlock = DbSnapshot; // REVAMP-14: alias for unupdated callers

} // namespace sgrn::gateway::twin
