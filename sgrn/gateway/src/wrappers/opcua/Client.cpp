#include <sgrn/gateway/wrappers/opcua/Client.hpp>

#include <open62541/client_config_default.h>
#include <open62541/client_highlevel.h>
#include <open62541/client_subscriptions.h>

#include <chrono>
#include <utility>

namespace sgrn::gateway::wrappers::opcua
{

// ── Private constructor ───────────────────────────────────────────────────────

Client::Client(UA_Client* tp_client, ClientConfig t_cfg)
    : client_(tp_client)
    , cfg_(std::move(t_cfg))
    , connected_(true) {
}

// ── Destructor ────────────────────────────────────────────────────────────────

Client::~Client() noexcept {
    disconnect();
}

// ── Move ──────────────────────────────────────────────────────────────────────

Client::Client(Client&& t_other) noexcept
    : client_(std::exchange(t_other.client_, nullptr))
    , cfg_(std::move(t_other.cfg_))
    , connected_(std::exchange(t_other.connected_, false))
    , mon_callbacks_(std::move(t_other.mon_callbacks_))
    , needs_reconnect_(t_other.needs_reconnect_)
    , reconnect_after_(t_other.reconnect_after_) {
}

Client& Client::operator=(Client&& t_other) noexcept {
    if (this != &t_other) {
        disconnect();
        client_ = std::exchange(t_other.client_, nullptr);
        cfg_ = std::move(t_other.cfg_);
        connected_ = std::exchange(t_other.connected_, false);
        mon_callbacks_ = std::move(t_other.mon_callbacks_);
        needs_reconnect_ = t_other.needs_reconnect_;
        reconnect_after_ = t_other.reconnect_after_;
    }
    return *this;
}

// ── Factory ───────────────────────────────────────────────────────────────────

sgrn::Result<Client> Client::connect(ClientConfig t_cfg) {
    UA_Client* p_raw = UA_Client_new();
    if (!p_raw)
        return "UA_Client_new( returned NULL";

    UA_ClientConfig* p_config = UA_Client_getConfig(p_raw);
    UA_ClientConfig_setDefault(p_config);
    p_config->timeout = t_cfg.timeout_ms;

    UA_StatusCode sc = UA_Client_connect(p_raw, t_cfg.endpoint_url.c_str());
    if (sc != UA_STATUSCODE_GOOD) {
        UA_Client_delete(p_raw);
        return fmt::format("UA_Client_connect({}: {}", t_cfg.endpoint_url, UA_StatusCode_name(sc));
    }

    return Client(p_raw, std::move(t_cfg));
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────

void Client::disconnect() noexcept {
    if (!client_)
        return;
    UA_Client_disconnect(client_);
    UA_Client_delete(client_);
    client_ = nullptr;
    connected_ = false;
}

// ── Read / Write ──────────────────────────────────────────────────────────────

sgrn::Result<DataValue> Client::readDataValue(const NodeId& t_node) {
    if (!client_ || !connected_)
        return "readDataValue: client not connected";

    UA_DataValue raw_dv{};
    UA_StatusCode sc = UA_Client_readValueAttribute(client_, t_node.get(), &raw_dv.value);
    if (sc != UA_STATUSCODE_GOOD) {
        UA_DataValue_clear(&raw_dv);
        return fmt::format("readDataValue: {}", UA_StatusCode_name(sc));
    }
    raw_dv.hasValue = true;

    // Move raw_dv into an owning DataValue wrapper.
    // DataValue's default constructor zero-inits; we overwrite dv_ directly
    // via get() (package-internal access).
    DataValue dv;
    dv.get() = raw_dv; // transfer ownership — raw_dv is now logically empty
    return dv;
}

sgrn::Result<void> Client::writeDataValue(const NodeId& t_node, const DataValue& t_value) {
    if (!client_ || !connected_)
        return "writeDataValue: client not connected";

    // UA_Client_writeValueAttribute takes a const UA_Variant* — copy the value
    // so we don't transfer ownership out of the DataValue wrapper.
    UA_Variant var_copy;
    UA_Variant_copy(&t_value.get().value, &var_copy);
    UA_StatusCode sc = UA_Client_writeValueAttribute(client_, t_node.get(), &var_copy);
    UA_Variant_clear(&var_copy);

    if (sc != UA_STATUSCODE_GOOD)
        return fmt::format("writeDataValue: {}", UA_StatusCode_name(sc));
    return {};
}

// ── Browse ────────────────────────────────────────────────────────────────────

sgrn::Result<std::vector<NodeId>> Client::browse(const NodeId& t_start, UA_BrowseDirection t_direction) {
    if (!client_ || !connected_)
        return "browse: client not connected";

    UA_BrowseRequest req{};
    UA_BrowseRequest_init(&req);
    req.requestedMaxReferencesPerNode = 0; // no limit
    req.nodesToBrowse = UA_BrowseDescription_new();
    req.nodesToBrowseSize = 1;
    UA_NodeId_copy(&t_start.get(), &req.nodesToBrowse[0].nodeId);
    req.nodesToBrowse[0].browseDirection = t_direction;
    req.nodesToBrowse[0].includeSubtypes = UA_TRUE;
    req.nodesToBrowse[0].resultMask = UA_BROWSERESULTMASK_TARGETINFO;

    UA_BrowseResponse resp = UA_Client_Service_browse(client_, req);
    UA_BrowseRequest_clear(&req);

    if (resp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        UA_StatusCode sc = resp.responseHeader.serviceResult;
        UA_BrowseResponse_clear(&resp);
        return fmt::format("browse: {}", UA_StatusCode_name(sc));
    }

    std::vector<NodeId> result;
    for (size_t i = 0; i < resp.resultsSize; ++i) {
        const UA_BrowseResult& br = resp.results[i];
        for (size_t j = 0; j < br.referencesSize; ++j)
            result.push_back(nodeIdFromRaw(br.references[j].nodeId.nodeId));
    }

    UA_BrowseResponse_clear(&resp);
    return result;
}

sgrn::Result<NodeId> Client::translateBrowsePath(const NodeId& t_start, const std::vector<std::string>& t_path_elements) {
    if (!client_ || !connected_)
        return "translateBrowsePath: client not connected";

    UA_TranslateBrowsePathsToNodeIdsRequest req{};
    UA_TranslateBrowsePathsToNodeIdsRequest_init(&req);
    req.browsePaths = UA_BrowsePath_new();
    req.browsePathsSize = 1;
    UA_NodeId_copy(&t_start.get(), &req.browsePaths[0].startingNode);

    const size_t n = t_path_elements.size();
    req.browsePaths[0].relativePath.elements =
        static_cast<UA_RelativePathElement*>(UA_Array_new(n, &UA_TYPES[UA_TYPES_RELATIVEPATHELEMENT]));
    req.browsePaths[0].relativePath.elementsSize = n;

    for (size_t i = 0; i < n; ++i) {
        UA_RelativePathElement& el = req.browsePaths[0].relativePath.elements[i];
        UA_RelativePathElement_init(&el);
        el.referenceTypeId = UA_NODEID_NUMERIC(0, UA_NS0ID_HIERARCHICALREFERENCES);
        el.includeSubtypes = UA_TRUE;
        el.targetName = UA_QUALIFIEDNAME_ALLOC(1, t_path_elements[i].c_str());
    }

    UA_TranslateBrowsePathsToNodeIdsResponse resp = UA_Client_Service_translateBrowsePathsToNodeIds(client_, req);
    UA_TranslateBrowsePathsToNodeIdsRequest_clear(&req);

    if (resp.responseHeader.serviceResult != UA_STATUSCODE_GOOD) {
        UA_StatusCode sc = resp.responseHeader.serviceResult;
        UA_TranslateBrowsePathsToNodeIdsResponse_clear(&resp);
        return fmt::format("translateBrowsePath: {}", UA_StatusCode_name(sc));
    }

    if (resp.resultsSize == 0 || resp.results[0].targetsSize == 0) {
        UA_TranslateBrowsePathsToNodeIdsResponse_clear(&resp);
        return "translateBrowsePath: path not found";
    }

    NodeId result = nodeIdFromRaw(resp.results[0].targets[0].targetId.nodeId);
    UA_TranslateBrowsePathsToNodeIdsResponse_clear(&resp);
    return result;
}

// ── Subscriptions ─────────────────────────────────────────────────────────────

sgrn::Result<UA_UInt32> Client::createSubscription(uint32_t t_publish_interval_ms) {
    if (!client_ || !connected_)
        return "createSubscription: client not connected";

    UA_CreateSubscriptionRequest req = UA_CreateSubscriptionRequest_default();
    req.requestedPublishingInterval = static_cast<UA_Double>(t_publish_interval_ms);

    UA_CreateSubscriptionResponse resp = UA_Client_Subscriptions_create(client_, req, nullptr, nullptr, nullptr);

    if (resp.responseHeader.serviceResult != UA_STATUSCODE_GOOD)
        return fmt::format("createSubscription: {}", UA_StatusCode_name(resp.responseHeader.serviceResult));

    return resp.subscriptionId;
}

sgrn::Result<UA_UInt32> Client::addMonitoredItem(
    UA_UInt32 t_subscription_id, const NodeId& t_node, std::function<void(const DataValueView&)> t_callback) {
    if (!client_ || !connected_)
        return "addMonitoredItem: client not connected";

    UA_MonitoredItemCreateRequest item_req = UA_MonitoredItemCreateRequest_default(t_node.get());

    // The monitored item id is returned in the response; we need it to key
    // the callback table. open62541 passes mon_context (void*) to the handler,
    // which we set to `this` so the trampoline can reach mon_callbacks_.
    UA_MonitoredItemCreateResult result =
        UA_Client_MonitoredItems_createDataChange(client_, t_subscription_id, UA_TIMESTAMPSTORETURN_BOTH, item_req,
            this, // monContext — available in the trampoline as mon_context
            &Client::dataChangeCallback, nullptr);

    if (result.statusCode != UA_STATUSCODE_GOOD)
        return fmt::format("addMonitoredItem: {}", UA_StatusCode_name(result.statusCode));

    UA_UInt32 t_mon_id = result.monitoredItemId;
    mon_callbacks_[t_mon_id] = std::move(t_callback);
    return t_mon_id;
}

// ── Trampoline ────────────────────────────────────────────────────────────────

void Client::dataChangeCallback(
    UA_Client* /*client*/, UA_UInt32 /*sub_id*/, void* /*sub_context*/, UA_UInt32 t_mon_id, void* tp_mon_context, UA_DataValue* tp_value) {
    // mon_context is `this` (set in addMonitoredItem above).
    auto* p_self = static_cast<Client*>(tp_mon_context);
    auto it = p_self->mon_callbacks_.find(t_mon_id);
    if (it == p_self->mon_callbacks_.end())
        return;

    // Wrap the raw pointer in a non-owning view — the callback never sees UA_*.
    // The UA_DataValue* is valid only for the duration of this callback.
    DataValueView view(tp_value);
    it->second(view);
}

// ── Event loop ────────────────────────────────────────────────────────────────

sgrn::Result<void> Client::runIterate(uint32_t t_timeout_ms) {
    if (!client_)
        return "runIterate: client not initialised";

    // Handle pending reconnect before iterating
    if (cfg_.auto_reconnect && needs_reconnect_) {
        auto now = std::chrono::steady_clock::now();
        if (now >= reconnect_after_) {
            needs_reconnect_ = false;
            UA_StatusCode sc = UA_Client_connect(client_, cfg_.endpoint_url.c_str());
            if (sc == UA_STATUSCODE_GOOD) {
                connected_ = true;
            } else {
                // Schedule the next attempt
                needs_reconnect_ = true;
                reconnect_after_ = now + std::chrono::milliseconds(cfg_.reconnect_delay_ms);
                // Not a hard error — caller can keep spinning
                return fmt::format("reconnect attempt failed: {}", UA_StatusCode_name(sc));
            }
        }
        // Delay not yet elapsed — nothing to do this tick
        return {};
    }

    UA_StatusCode sc = UA_Client_run_iterate(client_, t_timeout_ms);

    if (sc == UA_STATUSCODE_BADCONNECTIONCLOSED || sc == UA_STATUSCODE_BADSERVERNOTCONNECTED || sc == UA_STATUSCODE_BADNOTCONNECTED) {
        connected_ = false;
        if (cfg_.auto_reconnect) {
            needs_reconnect_ = true;
            reconnect_after_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(cfg_.reconnect_delay_ms);
        }
        return fmt::format("runIterate: disconnected ({}", UA_StatusCode_name(sc));
    }

    if (sc != UA_STATUSCODE_GOOD)
        return fmt::format("runIterate: {}", UA_StatusCode_name(sc));

    return {};
}

} // namespace sgrn::gateway::wrappers::opcua
