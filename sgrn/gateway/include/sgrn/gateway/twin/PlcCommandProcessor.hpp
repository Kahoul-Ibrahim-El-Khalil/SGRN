#pragma once
#include <sgrn/gateway/twin/PlcCommand.hpp>
#include <sgrn/gateway/twin/field_update.hpp>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace asio
{
class io_context;
class thread_pool;
} // namespace asio

namespace sgrn::gateway::twin
{
class PlcMemory;
// Existing single-notification callback — keep for WriteField path and
// call sites that only ever produce one notification at a time.
using FieldUpdateCallback = std::function<void(FieldUpdateNotification)>;

// NEW: batch callback for paths that can produce many notifications
// in one processCommands() pass (i.e. the coalesced binary write path).
using FieldUpdateBatchCallback = std::function<void(std::span<FieldUpdateNotification>)>;
class PlcCommandProcessor {
public:
    explicit PlcCommandProcessor(PlcMemory& t_memory)
        : memory_(t_memory) {
    }

    using FieldUpdateCallback = std::function<void(FieldUpdateNotification)>;

    void setOnFieldUpdate(FieldUpdateCallback t_cb) {
        on_field_update_ = std::move(t_cb);
    }
    void setDirtyHandler(std::function<void(std::vector<std::string>)> t_cb) {
        dirty_callback_ = std::move(t_cb);
    }
    void setContexts(asio::io_context* tp_light_ctx, asio::thread_pool* tp_heavy_pool) {
        light_ctx_ = tp_light_ctx;
        heavy_pool_ = tp_heavy_pool;
    }

    void setFieldUpdateCallback(FieldUpdateCallback cb) {
        on_field_update_ = std::move(cb);
    }
    void setFieldUpdateBatchCallback(FieldUpdateBatchCallback cb) {
        on_field_update_batch_ = std::move(cb);
    }
    void processCommands();
    void processDirty();
    void signalDirty();

    bool hasPendingCommands() const;

private:
    PlcMemory& memory_;
    asio::io_context* light_ctx_{nullptr};
    asio::thread_pool* heavy_pool_{nullptr};
    FieldUpdateCallback on_field_update_;
    std::function<void(std::vector<std::string>)> dirty_callback_;
    FieldUpdateBatchCallback on_field_update_batch_;
};
} // namespace sgrn::gateway::twin
