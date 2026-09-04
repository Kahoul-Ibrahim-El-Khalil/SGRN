#include <sgrn/Result.hpp>
#include <sgrn/gateway/adapters/ethernetip/EipAdapter.hpp>
#include <sgrn/gateway/adapters/http.hpp>
#include <sgrn/gateway/adapters/modbus/ModbusAdapter.hpp>
#include <sgrn/gateway/adapters/opcua.hpp>
#include <sgrn/gateway/adapters/s7/S7ProtocolAdapter.hpp>
#include <sgrn/gateway/adapters/s7/callbacks.hpp>
#include <sgrn/gateway/adapters/websocket/WebSocketAdapter.hpp>
#include <sgrn/gateway/config/gateway.hpp>
#include <sgrn/gateway/core/GlobalContext.hpp>
#include <sgrn/gateway/core/RecoveryEngine.hpp>
#include <sgrn/gateway/core/ServerContext.hpp>
#include <sgrn/gateway/core/TelemetryBroker.hpp>
#include <sgrn/gateway/core/TreeCacheEngine.hpp>
#include <sgrn/gateway/core/snapshot.hpp>
#include <sgrn/gateway/database/GatewayDatabase.hpp>
#include <sgrn/gateway/database/PersistenceService.hpp>
#include <sgrn/gateway/datastore/DatastoreBridge.hpp>
#include <sgrn/gateway/twin/DbSnapshot.hpp>
#include <sgrn/gateway/twin/LeafDictionary.hpp>
#include <sgrn/gateway/twin/PlcCommandProcessor.hpp>
#include <sgrn/gateway/twin/field_update.hpp>
#include <sgrn/gateway/twin/utils.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/schema/SchemaSerializer.hpp>
#include <sgrn/sdk/SgrnClient.hpp>
#include <sgrn/utils/app.hpp>
#include <sgrn/utils/compression.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <sgrn/utils/strings.hpp>
#include <sgrn/utils/threading.hpp>
#include <sgrn/utils/time.hpp>

#include <fmt/core.h>
#include <fmt/ranges.h>
#include <sgrn/debug.hpp>
#include <asio.hpp>
#include <asio/thread_pool.hpp>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <atomic>
#include <chrono>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace sgrn::gateway
{
using namespace sgrn::gateway::twin;
using namespace sgrn::gateway::adapters::websocket;
using namespace ::sgrn::scl;
using namespace sgrn::gateway::config;
using namespace sgrn::gateway::backend;
using namespace sgrn::gateway::core;

constexpr int kLightThreads = 2;
constexpr int kHeavyPoolThreads = 2; // worker threads for compression/disk I/O

class GatewayApplication {
public:
    GatewayApplication() = default;

    sgrn::Result<void, std::string> loadConfig(int t_argc, char** tp_argv);
    sgrn::Result<void, std::string> loadSchema();
    sgrn::Result<void, std::string> initSecurity();
    sgrn::Result<void, std::string> initTwin();
    sgrn::Result<void, std::string> initThreading();

    sgrn::Result<void, std::string> wireTelemetry();
    sgrn::Result<void, std::string> initInfrastructure();
    sgrn::Result<void, std::string> startAdapters();
    void feedInitialAnchor();
    void run();
    void shutdown();

private:
    template <typename StarterFunc>
    bool startAdapter(const char* name, uint16_t port, StarterFunc starter) {
        try {
            auto res = starter();
            if (res.hasError()) {
                std::string err_msg;
                if constexpr (std::is_same_v<std::decay_t<decltype(res.error())>, std::string>) {
                    err_msg = res.error();
                } else if constexpr (requires { res.error().string(); }) {
                    err_msg = res.error().string();
                } else if constexpr (requires { res.error().message_; }) {
                    err_msg = res.error().message_;
                } else if constexpr (requires { res.error().message; }) {
                    err_msg = res.error().message;
                } else if constexpr (requires { toString(res.error()); }) {
                    // Adapter-scoped error enums (http/modbus/ethernetip/errors.hpp,
                    // opcua/errors.hpp) each ship their own toString() translator.
                    err_msg = toString(res.error());
                } else {
                    err_msg = "Unknown error";
                }
                SGRN_ERROR_LOG("{} failed to start on port {}: {}", name, port, err_msg);
                return false;
            }
            active_protocols_++;
            return true;
        } catch (const std::exception& e) {
            SGRN_ERROR_LOG("{} failed to bind port {}: {}", name, port, e.what());
            return false;
        }
    }

    GatewayConfig config_;
    bool gui_mode_{false};
    std::string schema_override_;
    std::string policy_script_;

    sgrn::scl::PlcSchemaStore symbolic_store_;
    sgrn::gateway::twin::PlcState plc_state_;
    sgrn::gateway::twin::PlcMemory server_;
    sgrn::gateway::core::ServerContext srv_ctx_;
    sgrn::gateway::twin::LeafDictionary leaf_dict_; ///< shared dictionary for persistence + live channels

    asio::io_context light_ctx_;
    std::optional<asio::executor_work_guard<asio::io_context::executor_type>> light_work_;
    std::thread light_thread_;
    std::unique_ptr<asio::thread_pool> heavy_pool_;

    std::shared_ptr<sgrn::gateway::database::GatewayDatabase> node_db_;
    std::unique_ptr<sgrn::gateway::database::PersistenceService> persistence_service_;
    std::optional<sgrn::gateway::backend::DatastoreBridge> bridge_;
    std::shared_ptr<sgrn::gateway::SecurityManager> security_manager_;

    std::optional<sgrn::gateway::adapters::s7::S7ProtocolAdapter> s7_adapter_;
    std::optional<sgrn::gateway::adapters::OpcUaAdapter> opc_adapter_;
    std::optional<sgrn::gateway::adapters::modbus::ModbusAdapter> modbus_adapter_;
    std::optional<sgrn::gateway::adapters::ethernetip::EipAdapter> eip_adapter_;
    std::optional<sgrn::gateway::adapters::HttpAdapter> http_adapter_;
    std::optional<sgrn::gateway::adapters::websocket::WebSocketAdapter> ws_facade_;

    uint8_t active_protocols_{0};
};
} // namespace sgrn::gateway
