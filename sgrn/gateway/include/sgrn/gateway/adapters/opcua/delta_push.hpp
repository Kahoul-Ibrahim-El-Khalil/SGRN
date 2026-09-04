#pragma once

#include <sgrn/gateway/adapters/opcua/NodeContext.hpp>
#include <sgrn/gateway/core/TelemetryBroker.hpp>
#include <sgrn/gateway/wrappers/opcua/DataValue.hpp>
#include <sgrn/gateway/wrappers/opcua/NodeId.hpp>
#include <ankerl/unordered_dense.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>

namespace sgrn::gateway::wrappers::opcua
{
class Server;
} // namespace sgrn::gateway::wrappers::opcua

namespace sgrn::gateway::adapters
{

class DeltaPushHandler {
public:
    using PendingWriteCallback = std::function<void(wrappers::opcua::NodeId, wrappers::opcua::DataValue)>;

    explicit DeltaPushHandler(wrappers::opcua::Server* tp_server = nullptr);

    void registerNode(const std::string& t_map_key, const wrappers::opcua::NodeId& t_node_id, const NodeContext* tp_ctx);

    void onTelemetryEvent(const core::TelemetryEvent& t_event);

    void setRunning(bool t_running);
    void setServer(wrappers::opcua::Server* tp_server) {
        server_ = tp_server;
    }
    void setPendingWriteCallback(PendingWriteCallback t_cb) {
        pending_write_ = std::move(t_cb);
    }

    void flushDirtyAggregates(uint64_t t_timestamp_ms);

    using AlarmCallback =
        std::function<void(uint16_t t_db_number, const std::string& t_path, const rapidjson::Value& t_alarm_obj, uint64_t t_timestamp_ms)>;
    void setAlarmCallback(AlarmCallback t_cb) {
        alarm_callback_ = std::move(t_cb);
    }

    using ActiveClientsCheck = std::function<bool()>;
    void setActiveClientsCheck(ActiveClientsCheck t_cb) {
        active_clients_check_ = std::move(t_cb);
    }

    void setNodeSubscribed(const std::string& t_map_key, bool t_subscribed);
    bool isNodeSubscribed(const std::string& t_map_key) const;

private:
    struct RegisteredNode {
        wrappers::opcua::NodeId node_id;
        const NodeContext* ctx{nullptr};
    };

    bool buildDataValueFromEvent(const core::TelemetryEvent& t_event, const NodeContext* tp_ctx, UA_DataValue& t_raw_dv);
    void enqueueDataValue(const wrappers::opcua::NodeId& t_node_id, UA_DataValue&& t_raw_dv, uint64_t t_timestamp_ms);
    void flattenAndPush(uint16_t t_db_number, const rapidjson::Value& t_obj, const std::string& t_path_prefix, uint64_t t_timestamp_ms);
    void pushSubtreeSnapshot(uint16_t t_db_number, const std::string& t_path_prefix, uint64_t t_timestamp_ms);
    void notifyAggregateAncestors(uint16_t t_db_number, const std::string& t_leaf_path, uint64_t /*timestamp_ms*/);

    wrappers::opcua::Server* server_{nullptr};
    std::atomic<bool> running_{false};
    ankerl::unordered_dense::map<std::string, RegisteredNode> nodes_;
    AlarmCallback alarm_callback_;
    PendingWriteCallback pending_write_;
    ActiveClientsCheck active_clients_check_;

    ankerl::unordered_dense::map<std::string, uint32_t> subscription_counts_;
    mutable std::mutex subscription_mutex_;

    ankerl::unordered_dense::set<std::string> dirty_aggregates_;
    std::mutex dirty_mutex_;
};

} // namespace sgrn::gateway::adapters
