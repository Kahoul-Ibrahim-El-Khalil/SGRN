#include <sgrn/gateway/adapters/opcua.hpp>
#include <sgrn/gateway/adapters/opcua/NodeContext.hpp>
#include <sgrn/gateway/adapters/opcua/delta_push.hpp>
#include <sgrn/gateway/adapters/opcua/information_model.hpp>
#include <sgrn/gateway/adapters/opcua/schema_type_registry.hpp>
#include <sgrn/gateway/core/TelemetryBroker.hpp>
#include <sgrn/gateway/security/SecurityManager.hpp>
#include <sgrn/gateway/twin/PlcCommandProcessor.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <sgrn/gateway/wrappers/opcua/DataValue.hpp>
#include <sgrn/gateway/wrappers/opcua/NodeId.hpp>
#include <sgrn/gateway/wrappers/opcua/Server.hpp>
#include <sgrn/gateway/wrappers/opcua/SessionRegistry.hpp>
#include <sgrn/gateway/wrappers/opcua/TypeRegistry.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>

#include <sgrn/utils/time.hpp>

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

namespace sgrn::gateway::adapters
{

using PlcSchemaStore = ::sgrn::scl::PlcSchemaStore;

struct PendingWrite {
    wrappers::opcua::NodeId node_id;
    wrappers::opcua::DataValue value;
};

struct OpcUaAdapter::Impl {
    std::unique_ptr<wrappers::opcua::Server> server;
    std::atomic<bool> running{false};
    std::thread server_thread;
    twin::PlcMemory* p_s7_server{nullptr};
    std::shared_ptr<::sgrn::gateway::SecurityManager> ps_security_manager;

    std::vector<std::unique_ptr<NodeContext>> node_contexts;
    std::unordered_map<std::string, wrappers::opcua::NodeId> node_id_map;

    core::TelemetryBroker::SubscriberId broker_sub_id{0};
    UA_UInt64 pending_write_callback_id{0};

    std::mutex pending_mutex;
    std::queue<PendingWrite> pending_writes;

    DeltaPushHandler delta_push;
    wrappers::opcua::SessionRegistry session_registry;
    wrappers::opcua::TypeRegistry type_registry;
    wrappers::opcua::NodeId alarm_event_type_id;
};

OpcUaAdapter::OpcUaAdapter()
    : impl_(std::make_unique<Impl>()) {
}

OpcUaAdapter::~OpcUaAdapter() {
    stop();
}

sgrn::Result<void> OpcUaAdapter::start(const std::string& /*ip*/, uint16_t t_port, const PlcSchemaStore& t_registry,
    twin::PlcMemory& t_s7_server, std::shared_ptr<::sgrn::gateway::SecurityManager> tsp_security_manager) {

    impl_->p_s7_server = &t_s7_server;
    impl_->ps_security_manager = std::move(tsp_security_manager);
    impl_->server = std::make_unique<wrappers::opcua::Server>(t_port);
    impl_->server->suppressInfoLogs();

    auto type_res = populateTypeRegistryFromSchema(t_registry, impl_->type_registry);
    if (type_res.hasError())
        return type_res;
    impl_->server->installTypeRegistry(impl_->type_registry);

    impl_->session_registry.installAccessControl(impl_->server->raw());

    impl_->delta_push.setServer(impl_->server.get());
    impl_->delta_push.setPendingWriteCallback([this](wrappers::opcua::NodeId node_id, wrappers::opcua::DataValue value) {
        std::lock_guard lock(impl_->pending_mutex);
        impl_->pending_writes.push(PendingWrite{std::move(node_id), std::move(value)});
    });
    impl_->delta_push.setAlarmCallback([this](uint16_t db, const std::string& path, const rapidjson::Value& alarm, uint64_t ts) {
        triggerAlarmEvent(*impl_->server, impl_->alarm_event_type_id, db, path, alarm, ts);
    });

    buildAddressSpace(*impl_->server, t_registry, impl_->p_s7_server, impl_->ps_security_manager.get(), impl_->node_contexts,
        impl_->node_id_map, impl_->type_registry, impl_->alarm_event_type_id, &impl_->delta_push);

    std::promise<UA_StatusCode> startup_promise;
    auto startup_future = startup_promise.get_future();
    impl_->running = true;
    impl_->delta_push.setRunning(true);

    impl_->server_thread = std::thread([this, p = std::move(startup_promise)]() mutable {
        UA_Server* raw = impl_->server->raw();
        UA_StatusCode retval = UA_Server_run_startup(raw);
        p.set_value(retval);
        if (retval != UA_STATUSCODE_GOOD) {
            impl_->running = false;
            return;
        }

        UA_UInt64 cb_id = 0;
        UA_Server_addRepeatedCallback(
            raw,
            [](UA_Server*, void* data) {
                auto* self = static_cast<OpcUaAdapter*>(data);
                uint64_t now_ms = sgrn::utils::time::nowMilliseconds();
                {
                    std::lock_guard lock(self->impl_->pending_mutex);
                    while (!self->impl_->pending_writes.empty()) {
                        PendingWrite pw = std::move(self->impl_->pending_writes.front());
                        self->impl_->pending_writes.pop();
                        self->impl_->server->writeDataValue(pw.node_id, pw.value);
                    }
                }
                self->impl_->delta_push.flushDirtyAggregates(now_ms);
                if (self->impl_->p_s7_server && self->impl_->p_s7_server->processor()->hasPendingCommands())
                    self->impl_->p_s7_server->processor()->processCommands();
            },
            this, 50, &cb_id);
        impl_->pending_write_callback_id = cb_id;

        while (impl_->running.load())
            UA_Server_run_iterate(raw, true);

        UA_Server_removeCallback(raw, impl_->pending_write_callback_id);
        impl_->pending_write_callback_id = 0;
        UA_Server_run_shutdown(raw);
    });

    UA_StatusCode sc = startup_future.get();
    if (sc != UA_STATUSCODE_GOOD) {
        impl_->running = false;
        impl_->delta_push.setRunning(false);
        if (impl_->server_thread.joinable())
            impl_->server_thread.join();
        return fmt::format("OPCUA Startup failed: {}", UA_StatusCode_name(sc));
    }

    impl_->broker_sub_id =
        core::TelemetryBroker::instance().subscribe([this](const core::TelemetryEvent& ev) { impl_->delta_push.onTelemetryEvent(ev); });

    return {};
}

void OpcUaAdapter::stop() {
    if (!impl_)
        return;

    if (impl_->broker_sub_id != 0) {
        core::TelemetryBroker::instance().unsubscribe(impl_->broker_sub_id);
        impl_->broker_sub_id = 0;
    }

    impl_->delta_push.setRunning(false);
    impl_->running = false;
    if (impl_->server_thread.joinable())
        impl_->server_thread.join();

    if (impl_->server) {
        impl_->server->destroy();
        impl_->server.reset();
    }

    impl_->node_contexts.clear();
    impl_->node_id_map.clear();

    {
        std::lock_guard lock(impl_->pending_mutex);
        while (!impl_->pending_writes.empty())
            impl_->pending_writes.pop();
    }
}

std::size_t OpcUaAdapter::clientsCount() const {
    return impl_->session_registry.clientsCount();
}

} // namespace sgrn::gateway::adapters
