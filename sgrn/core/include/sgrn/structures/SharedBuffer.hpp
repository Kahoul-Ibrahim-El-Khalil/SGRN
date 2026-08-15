#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>
namespace sgrn
{

/**
 * @brief A thread-safe buffer using a Reader-Writer lock (shared_mutex).
 *
 * @section Context Ideal Context
 * - Optimized for industrial Digital Twin scenarios: one high-frequency writer (e.g., a PLC Poller)
 *   and many asynchronous readers (APIs, UI, Logger).
 * - Ideal for "Process Image" blocks where the data size is fixed and readers
 *   need a consistent view without blocking the writer for long.
 *
 * @section Usage How to Use
 * 1. **Creation**: Instantiate with a size `SharedBuffer(1024)` for owned memory,
 *    or wrap existing memory with `SharedBuffer(ptr, size)`.
 * 2. **Reading**: Call `getSharedLock()` to obtain a `shared_lock`. While held,
 *    the data returned by `data()` is guaranteed not to be modified by other threads.
 * 3. **Writing**: Call `getUniqueLock()` to obtain a `unique_lock`. This blocks until
 *    all readers are finished, giving you exclusive access to update the buffer.
 * 4. **Timing**: Use `setTimestamp()` after a successful write to track the "freshness"
 *    of the data for downstream consumers.
 */
class SharedBuffer {
public:
    explicit SharedBuffer(size_t t_size)
        : data_ptr_(nullptr)
        , size_(t_size)
        , last_update_ts_(0) {
        owned_data_.assign(t_size, 0);
        data_ptr_ = owned_data_.data();
    }

    /**
     * @brief Create a view-only SharedBuffer pointing to external memory.
     * The caller is responsible for ensuring the memory remains valid.
     */
    SharedBuffer(uint8_t* tp_data, size_t t_size)
        : data_ptr_(tp_data)
        , size_(t_size)
        , last_update_ts_(0) {
    }

    // --- Accessors ---

    /**
     * @brief Get a shared lock for reading.
     */
    std::shared_lock<std::shared_mutex> getSharedLock() const {
        return std::shared_lock<std::shared_mutex>(mutex_);
    }

    /**
     * @brief Get a unique lock for writing.
     */
    std::unique_lock<std::shared_mutex> getUniqueLock() {
        return std::unique_lock<std::shared_mutex>(mutex_);
    }

    uint8_t* data() {
        return data_ptr_;
    }
    const uint8_t* data() const {
        return data_ptr_;
    }
    size_t size() const {
        return size_;
    }

    void setTimestamp(int64_t t_ts) {
        last_update_ts_.store(t_ts, std::memory_order_relaxed);
    }
    int64_t getTimestamp() const {
        return last_update_ts_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Raw access to the mutex if manual locking is needed (e.g. for Snap7 registration).
     */
    std::shared_mutex& getMutex() const {
        return mutex_;
    }

private:
    std::vector<uint8_t> owned_data_;
    uint8_t* data_ptr_;
    size_t size_;
    mutable std::shared_mutex mutex_;
    std::atomic<int64_t> last_update_ts_;
};

} // namespace sgrn
