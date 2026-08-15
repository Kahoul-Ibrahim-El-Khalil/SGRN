#include <sgrn/Result.hpp>
#include <sgrn/gateway/adapters/s7/callbacks.hpp>
#include <sgrn/gateway/core/ServerContext.hpp>

namespace sgrn::gateway::adapters::s7
{

void S7API onServerEvent(void* tp_server_context, PSrvEvent t_event, int /*Size*/) {
    auto p_srv_ctx = static_cast<sgrn::gateway::core::ServerContext*>(tp_server_context);
    if (!tp_server_context || !t_event) {
        return;
    }
    if (t_event->EvtCode == evcClientAdded) {
        // Log to SQLite
        auto res = p_srv_ctx->node_db->recordSouthConnection("unknown");
        if (!res.hasError()) {
            int sid = res.value();
            std::lock_guard<std::mutex> lk(p_srv_ctx->mu);
            p_srv_ctx->session_map[t_event->EvtSender] = sid;
        }
    } else if (t_event->EvtCode == evcClientDisconnected) {
        int sid = -1;
        {
            std::lock_guard<std::mutex> lk(p_srv_ctx->mu);
            auto it = p_srv_ctx->session_map.find(t_event->EvtSender);
            if (it != p_srv_ctx->session_map.end()) {
                sid = it->second;
                p_srv_ctx->session_map.erase(it);
            }
        }
        if (sid != -1) {
            (void)p_srv_ctx->node_db->recordClientDisconnect(sid, 0, 0);
        }
    }
}

} // namespace sgrn::gateway::adapters::s7
