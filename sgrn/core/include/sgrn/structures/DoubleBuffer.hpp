
#pragma once
#include <atomic>
#include <mutex>
#include <vector>

namespace sgrn
{

/**
 * @brief A high-performance double-buffering utility for wait-free concurrency.
 *
 * This pattern allows one thread (the producer/IO) to write to a "back" buffer
 * while other threads (consumers/API) read from a "front" buffer.
 *
 * Switching between buffers is an atomic operation.
 */
template <typename T>
class DoubleBuffer {
public:
    DoubleBuffer() = default;

    explicit DoubleBuffer(size_t t_initial_size, T t_default_value = T{}) {
        buffers_[0].assign(t_initial_size, t_default_value);
        buffers_[1].assign(t_initial_size, t_default_value);
    }

    /**
     * @brief Resize both internal buffers.
     *
     * WARNING: Not thread-safe relative to other operations. Use during initialization.
     */
    void resize(size_t t_new_size, T t_default_value = T{}) {
        buffers_[0].assign(t_new_size, t_default_value);
        buffers_[1].assign(t_new_size, t_default_value);
    }

    // Manual move constructor since std::atomic is not movable
    DoubleBuffer(DoubleBuffer&& t_other) noexcept {
        buffers_[0] = std::move(t_other.buffers_[0]);
        buffers_[1] = std::move(t_other.buffers_[1]);
        front_idx_.store(t_other.front_idx_.load());
    }

    DoubleBuffer& operator=(DoubleBuffer&& t_other) noexcept {
        if (this != &t_other) {
            buffers_[0] = std::move(t_other.buffers_[0]);
            buffers_[1] = std::move(t_other.buffers_[1]);
            front_idx_.store(t_other.front_idx_.load());
        }
        return *this;
    }

    // Disable copy
    DoubleBuffer(const DoubleBuffer&) = delete;
    DoubleBuffer& operator=(const DoubleBuffer&) = delete;

    /**
     * @brief Access the front (read-only) buffer.
     */
    const std::vector<T>& front() const {
        return buffers_[front_idx_.load(std::memory_order_acquire)];
    }

    /**
     * @brief Access the back (write-only) buffer.
     */
    std::vector<T>& back() {
        return buffers_[1 - front_idx_.load(std::memory_order_relaxed)];
    }

    /**
     * @brief Atomically swap the front and back buffers.
     *
     * Correctness note: this is a publish operation, not a lock. The writer
     * must finish all mutation of back() *before* calling swap() — the
     * release store here is what makes those writes visible to readers
     * that subsequently acquire-load front_idx_ in front()/front_data().
     * Readers never see a partially-written buffer because they only ever
     * see a fully-written one that was already swapped in; there is no
     * in-place mutation of the buffer a reader currently holds a reference
     * to. This is the mechanism behind SGRN's "torn-read prevention" for
     * shadow-memory reads — a multi-byte value (float, DTL, String) is
     * either fully old or fully new, never half of each.
     */
    void swap() {
        int current = front_idx_.load(std::memory_order_relaxed);
        front_idx_.store(1 - current, std::memory_order_release);
    }
    size_t size() const {
        return buffers_[0].size();
    }

    const T* front_data() const {
        return buffers_[front_idx_.load(std::memory_order_acquire)].data();
    }

    T* front_data_rw() {
        return buffers_[front_idx_.load(std::memory_order_relaxed)].data();
    }

    T* back_data() {
        return buffers_[1 - front_idx_.load(std::memory_order_relaxed)].data();
    }

private:
    std::vector<T> buffers_[2];
    std::atomic<int> front_idx_{0};
};

} // namespace sgrn
