#pragma once

#include <cstddef>
#include <mutex>
#include <optional>
#include <vector>

namespace sgrn
{

/**
 * @brief Thread-safe circular buffer for high-speed data sampling or telemetry.
 *
 * @section Context Ideal Context
 * - High-speed data sampling where only the last N samples are relevant (e.g., oscilloscope).
 * - Decoupling fixed-rate producers from bursty consumers.
 * - Implementing moving average filters or windowed algorithms.
 *
 * @section Usage How to Use
 * 1. **Instantiate**: `CircularBuffer<float> buffer(1024);`
 * 2. **Push**: `buffer.push(3.14f);` — If full, the oldest element is overwritten.
 * 3. **Pop**: `auto val = buffer.pop();` — Returns oldest element or std::nullopt.
 * 4. **Iterate**: `buffer.forEach([](const T& val) { ... });` — Processes elements from oldest to newest.
 *
 * @tparam T The type of items stored in the buffer.
 */
template <typename T>
class CircularBuffer {
public:
    explicit CircularBuffer(std::size_t t_capacity)
        : buffer_(t_capacity)
        , capacity_(t_capacity) {
    }

    /**
     * @brief Add an item to the buffer. If the buffer is full, the oldest item is overwritten.
     */
    void push(T t_item) {
        std::lock_guard<std::mutex> lk(mu_);
        buffer_[write_idx_] = std::move(t_item);

        if (full_) {
            read_idx_ = (read_idx_ + 1) % capacity_;
        }

        write_idx_ = (write_idx_ + 1) % capacity_;
        full_ = (write_idx_ == read_idx_);
    }

    /**
     * @brief Remove and return the oldest item from the buffer.
     * @return The item, or std::nullopt if the buffer is empty.
     */
    std::optional<T> pop() {
        std::lock_guard<std::mutex> lk(mu_);
        if (empty_internal()) {
            return std::nullopt;
        }

        T t_item = std::move(buffer_[read_idx_]);
        full_ = false;
        read_idx_ = (read_idx_ + 1) % capacity_;

        return t_item;
    }

    /**
     * @brief Check if the buffer is empty.
     */
    bool empty() const {
        std::lock_guard<std::mutex> lk(mu_);
        return empty_internal();
    }

    /**
     * @brief Current number of items in the buffer.
     */
    std::size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        if (full_)
            return capacity_;
        if (write_idx_ >= read_idx_)
            return write_idx_ - read_idx_;
        return capacity_ + write_idx_ - read_idx_;
    }

    /**
     * @brief Maximum capacity of the buffer.
     */
    std::size_t capacity() const {
        return capacity_;
    }

    /**
     * @brief Clear the buffer.
     */
    void clear() {
        std::lock_guard<std::mutex> lk(mu_);
        read_idx_ = 0;
        write_idx_ = 0;
        full_ = false;
    }

    /**
     * @brief Execute a function for each element in the buffer, from oldest to newest.
     */
    template <typename F>
    void forEach(F&& t_func) const {
        std::lock_guard<std::mutex> lk(mu_);
        if (empty_internal())
            return;

        std::size_t count = size_internal();
        for (std::size_t i = 0; i < count; ++i) {
            t_func(buffer_[(read_idx_ + i) % capacity_]);
        }
    }

private:
    bool empty_internal() const {
        return !full_ && (write_idx_ == read_idx_);
    }

    std::size_t size_internal() const {
        if (full_)
            return capacity_;
        if (write_idx_ >= read_idx_)
            return write_idx_ - read_idx_;
        return capacity_ + write_idx_ - read_idx_;
    }

    std::vector<T> buffer_;
    std::size_t capacity_;
    std::size_t read_idx_{0};
    std::size_t write_idx_{0};
    bool full_{false};
    mutable std::mutex mu_;
};

} // namespace sgrn
