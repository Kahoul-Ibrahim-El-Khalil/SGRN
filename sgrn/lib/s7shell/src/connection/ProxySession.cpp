#include <sgrn/s7shell/connection/ProxySession.hpp>

#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/s7shell/connection/S7Connection.hpp>
#include <cstring>

namespace sgrn::s7shell::connection
{

ProxySession::ProxySession(::sgrn::s7shell::shell::ScriptS7Client* tp_src, ::sgrn::s7shell::shell::ScriptS7Client* tp_hub)
    : src_client_(tp_src)
    , hub_client_(tp_hub) {
}

ProxySession::~ProxySession() {
    stop();
}

void ProxySession::addMapping(uint16_t t_src_db, uint16_t t_dst_db, int t_interval_ms, uint32_t t_size_bytes) {
    if (running_)
        return;

    auto tsp_map = std::make_shared<ProxyMapping>();
    tsp_map->src_db = t_src_db;
    tsp_map->dst_db = t_dst_db;
    tsp_map->interval_ms = t_interval_ms;
    tsp_map->size_bytes = static_cast<size_t>(t_size_bytes);
    tsp_map->first_run = true;
    mappings_.push_back(std::move(tsp_map));
}

void ProxySession::start() {
    if (running_ || mappings_.empty())
        return;
    running_ = true;

    pool_ = std::make_unique<asio::thread_pool>(2);
    work_guard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(asio::make_work_guard(io_));

    for (auto& tsp_map : mappings_) {
        tsp_map->timer = std::make_shared<asio::steady_timer>(io_);
        tick(tsp_map);
    }

    io_thread_ = std::thread([this]() { io_.run(); });
}

void ProxySession::stop() {
    if (!running_)
        return;
    running_ = false;

    // Cancel all timers
    for (auto& tsp_map : mappings_) {
        if (tsp_map->timer)
            tsp_map->timer->cancel();
    }

    work_guard_.reset(); // allow io_ to drain
    io_.stop();

    if (io_thread_.joinable())
        io_thread_.join();

    if (pool_) {
        pool_->stop();
        pool_->join();
        pool_.reset();
    }
}

void ProxySession::tick(ProxyMappingSPtr tsp_map) {
    if (!running_)
        return;

    tsp_map->timer->expires_after(std::chrono::milliseconds(tsp_map->interval_ms));
    tsp_map->timer->async_wait([self = shared_from_this(), tsp_map](std::error_code t_ec) {
        if (!t_ec && self->running_)
            self->execute(tsp_map);
    });
}

void ProxySession::execute(ProxyMappingSPtr tsp_map) {
    if (!running_ || !pool_)
        return;

    asio::post(*pool_, [self = shared_from_this(), tsp_map]() {
        if (!self->running_)
            return;
        self->do_poll_and_push(tsp_map);
        if (self->running_) {
            // reschedule back on the io_ thread
            asio::post(tsp_map->timer->get_executor(), [self, tsp_map]() { self->tick(tsp_map); });
        }
    });
}

void ProxySession::do_poll_and_push(ProxyMappingSPtr tsp_map) {
    std::lock_guard<std::mutex> lock(src_mutex_);

    auto* p_src_conn = src_client_->internalConn();
    auto* p_hub_conn = hub_client_->internalConn();
    auto& src_wire = p_src_conn->client_;
    auto& hub_wire = p_hub_conn->client_;

    // Resolve size from the src client's schema if the caller didn't pin
    // one explicitly (previously this just skipped the mapping — the
    // runtime's schema makes "0 = whole DB" actually work).
    if (tsp_map->size_bytes == 0) {
        auto db_res = p_src_conn->schema_.getDb(tsp_map->src_db);
        if (db_res.hasError()) {
            fmt::print(stderr, fg(fmt::color::yellow),
                "[S7Proxy] DB{}: size unknown and no schema entry found — call addMapping with an explicit size or load a "
                "schema first\n",
                tsp_map->src_db);
            return;
        }
        tsp_map->size_bytes = static_cast<size_t>(db_res.value()->size_bytes);
    }

    if (tsp_map->buffer.size() != tsp_map->size_bytes) {
        tsp_map->buffer.resize(tsp_map->size_bytes, 0);
    }

    // 1. Poll Source PLC
    auto read_res = src_wire.readDB(tsp_map->src_db, 0, static_cast<int>(tsp_map->size_bytes), tsp_map->buffer.data());
    if (!read_res) {
        fmt::print(stderr, fg(fmt::color::red), "[S7Proxy] Poll SchemaError (DB{}): {}\n", tsp_map->src_db, toString(read_res.error()));
        return;
    }

    // 2. Change detection against the src client's shared PlcRuntime
    //    baseline (runtime->dbSnapshots()) — the same baseline ScriptDataBlock
    //    uses for its own dirty tracking — instead of a private buffer only
    //    this ProxyMapping could see.
    auto& baseline = p_src_conn->runtime_->getDbSnapshots()[tsp_map->src_db];
    bool changed = tsp_map->first_run || baseline.size() != tsp_map->buffer.size() ||
                   memcmp(tsp_map->buffer.data(), baseline.data(), tsp_map->buffer.size()) != 0;
    if (!changed)
        return;

    baseline.assign(tsp_map->buffer.begin(), tsp_map->buffer.end());
    p_src_conn->runtime_->markDirty(tsp_map->src_db, 0, static_cast<uint32_t>(tsp_map->size_bytes));

    // 3. Push to Hub PLC
    auto write_res = hub_wire.writeDB(tsp_map->dst_db, 0, static_cast<int>(tsp_map->size_bytes), tsp_map->buffer.data());
    if (!write_res) {
        fmt::print(
            stderr, fg(fmt::color::red), "[S7Proxy] Push SchemaError (-> Hub DB{}): {}\n", tsp_map->dst_db, toString(write_res.error()));
        return;
    }

    // Flushed successfully — clear the dirty region we just pushed. (A
    // future Runtime scheduler is what should be driving this push instead
    // of ProxySession calling it inline; for now this just makes sure the
    // bookkeeping is consistent with what a scheduler would see.)
    p_src_conn->runtime_->takeDirty(tsp_map->src_db);
    tsp_map->first_run = false;
}

} // namespace sgrn::s7shell::connection
