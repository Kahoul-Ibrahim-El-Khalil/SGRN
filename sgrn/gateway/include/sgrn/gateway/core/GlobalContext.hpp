#pragma once
#include <asio.hpp>
#include <memory>
#include <thread>
#include <vector>

namespace sgrn::gateway::core
{

class GlobalContext {
public:
    static GlobalContext& instance() {
        static GlobalContext inst;
        return inst;
    }

    asio::io_context& io_context() {
        return io_ctx_;
    }
    asio::thread_pool& worker_pool() {
        return worker_pool_;
    }

    void run(size_t t_io_threads = 2, size_t t_worker_threads = 2) {
        // Use work guard to keep io_context running
        work_guard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(asio::make_work_guard(io_ctx_));
        for (size_t i = 0; i < t_io_threads; ++i) {
            io_threads_.emplace_back([this]() { io_ctx_.run(); });
        }
    }

    void stop() {
        work_guard_.reset();
        io_ctx_.stop();
        for (auto& t : io_threads_)
            if (t.joinable())
                t.join();
        worker_pool_.stop();
        worker_pool_.join();
    }

private:
    GlobalContext()
        : worker_pool_(2) {
    }
    asio::io_context io_ctx_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;
    std::vector<std::thread> io_threads_;
    asio::thread_pool worker_pool_;
};

} // namespace sgrn::gateway::core
