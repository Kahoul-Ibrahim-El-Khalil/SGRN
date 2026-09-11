#pragma once

#include <sgrn/gateway/database/GatewayDatabase.hpp>
#include <sgrn/gateway/twin/PlcMemory.hpp>
#include <map>
#include <memory>
#include <mutex>

namespace sgrn::gateway::core
{

struct ServerContext {
    std::shared_ptr<sgrn::gateway::database::GatewayDatabase> node_db;
    sgrn::gateway::twin::PlcMemory* server;
    std::map<int, int> session_map; // SenderID -> DB SessionID
    std::mutex mu;
};
} // namespace sgrn::gateway::core
