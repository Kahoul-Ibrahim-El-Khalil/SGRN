#include <sgrn/gateway/config/gateway.hpp>
#include <sgrn/utils/json-serialization.hpp>
#include <sgrn/utils/strings.hpp>

using sgrn::Result;
namespace sgrn::gateway::config
{

namespace detail
{

// File shape of gateway.json. The leaf configs (S7Config, HttpConfig,
// PersistenceConfig, DatastoreConnectionConfig, ...) are read directly by
// Glaze's aggregate reflection: JSON key == member name, defaults come from
// the structs themselves, unknown keys are ignored (sgrn::json::kReadOpts).
// Only what differs from GatewayConfig's layout is declared here.

struct ProtocolsFile {
    std::optional<S7Config> s7;
    std::optional<OpcUaConfig> opcua;
    std::optional<HttpConfig> http;
    std::optional<WebSocketConfig> websocket;
    std::optional<ModbusConfig> modbus;
    std::optional<EthernetIpConfig> ethernetip;
};

struct DebugFile {
    std::optional<bool> incoming;
    std::optional<bool> tree;
};

struct ThreadingFile {
    std::optional<uint16_t> light_threads;
    std::optional<uint16_t> heavy_threads;
};

struct NodeFile {
    std::optional<uint32_t> number;
    std::optional<uint16_t> db_number; // legacy alias of "number"
    std::optional<std::string> allowed_ip;
    std::optional<std::string> dirty_tracker;
};

struct GatewayFile {
    std::optional<ProtocolsFile> southbound;
    std::optional<ProtocolsFile> northbound;
    std::optional<ProtocolsFile> listen; // legacy: shared block for both directions
    std::optional<std::string> security_policy;
    std::optional<std::string> security_script;
    std::optional<std::string> state_dir;
    std::optional<std::string> schema;
    std::optional<std::string> symbols_dir;
    std::optional<int> reconnect_ms;
    std::optional<bool> cache_json_north;
    std::optional<bool> verbose;
    std::optional<DebugFile> debug;
    std::optional<uint32_t> database_rotation_interval_s;
    std::optional<ThreadingFile> threading;
    std::optional<PersistenceConfig> persistence;
    std::optional<DatastoreConnectionConfig> datastore;
    std::optional<std::map<std::string, NodeFile>> nodes;
};

// Second pass over the same text: the "persistence" keys that PersistenceConfig
// cannot express (alias, zstd cascade, nested cloud_sync block).
struct CloudSyncFile {
    std::optional<bool> enabled;
    std::optional<std::string> url;
    std::optional<std::string> public_token;
    std::optional<std::string> private_token;
    std::optional<uint32_t> sync_interval_s;
};

struct PersistenceQuirks {
    std::optional<uint32_t> max_events; // alias of batch_size, wins when both are given
    std::optional<uint8_t> zstd_level;  // also sets anchor_zstd_level unless that is given
    std::optional<uint8_t> anchor_zstd_level;
    std::optional<CloudSyncFile> cloud_sync;
};

struct GatewayQuirksFile {
    std::optional<PersistenceQuirks> persistence;
};

template <class T>
void assign(T& t_dst, const std::optional<T>& t_src) {
    if (t_src.has_value())
        t_dst = t_src.value();
}

} // namespace detail

Result<GatewayConfig> parseNodeConfig(const std::string& t_path) {
    auto text = sgrn::json::readFileText(t_path);

    RETURN_ERROR_IF(text.hasError(), fmt::format("Failed to read config: {}", text.error().what()));

    auto file_res = sgrn::json::read<detail::GatewayFile>(text.value());
    RETURN_ERROR_IF(file_res.hasError(), fmt::format("Failed to deserialize config: {}", file_res.error().what()));

    auto quirks_res = sgrn::json::read<detail::GatewayQuirksFile>(text.value());
    RETURN_ERROR_IF(quirks_res.hasError(), fmt::format("Failed to deserialize config: {}", quirks_res.error().what()));

    using detail::assign;
    const auto& f = file_res.value();
    GatewayConfig cfg;

    // ── Protocols ─────────────────────────────────────────────────────────────
    // "southbound"/"northbound" fall back to the legacy shared "listen" block.
    // s7 is southbound-only, http/websocket northbound-only; for opcua, modbus
    // and ethernetip the southbound declaration wins, northbound fills the gap.
    const detail::ProtocolsFile none;
    const auto& south = f.southbound ? *f.southbound : (f.listen ? *f.listen : none);
    const auto& north = f.northbound ? *f.northbound : (f.listen ? *f.listen : none);

    cfg.s7 = south.s7;
    cfg.opcua = south.opcua ? south.opcua : north.opcua;
    cfg.modbus = south.modbus ? south.modbus : north.modbus;
    cfg.ethernetip = south.ethernetip ? south.ethernetip : north.ethernetip;
    cfg.http = north.http;
    cfg.websocket = north.websocket;

    // ── Scalars & paths ───────────────────────────────────────────────────────
    if (f.security_policy && sgrn::utils::strings::toLower(*f.security_policy) == "strict")
        cfg.security_policy = ::sgrn::scl::SecurityPolicy::Strict;
    if (f.security_script)
        cfg.security_script = SGRN_CONFIG_PATH(*f.security_script);
    if (f.state_dir)
        cfg.state_dir = SGRN_CONFIG_PATH(*f.state_dir);
    if (f.schema)
        cfg.schema_file = SGRN_CONFIG_PATH(*f.schema);
    if (f.symbols_dir)
        cfg.symbols_dir = SGRN_CONFIG_PATH(*f.symbols_dir);

    assign(cfg.reconnect_ms, f.reconnect_ms);
    assign(cfg.cache_json_north, f.cache_json_north);
    assign(cfg.verbose, f.verbose);
    assign(cfg.database_rotation_interval_s, f.database_rotation_interval_s);

    if (f.debug) {
        assign(cfg.debug_incoming, f.debug->incoming);
        assign(cfg.debug_tree, f.debug->tree);
    }
    // Out-of-range thread counts are clamped later in initThreading().
    if (f.threading.has_value()) {
        assign(cfg.light_threads, f.threading->light_threads);
        assign(cfg.heavy_threads, f.threading->heavy_threads);
    }

    // ── Persistence & datastore ───────────────────────────────────────────────
    assign(cfg.persistence, f.persistence);

    if (const auto& q = quirks_res.value().persistence) {

        assign(cfg.persistence.batch_size, q->max_events);

        assign(cfg.persistence.zstd_level, q->zstd_level);
        assign(cfg.persistence.anchor_zstd_level, q->zstd_level);
        assign(cfg.persistence.anchor_zstd_level, q->anchor_zstd_level);

        if (q->cloud_sync) {
            const auto& cs = *q->cloud_sync;
            DatastoreConnectionConfig ds;
            assign(ds.telemetry_enabled, cs.enabled);
            assign(ds.url, cs.url);
            assign(ds.public_token, cs.public_token);
            assign(ds.private_token, cs.private_token);
            assign(ds.sync_interval_s, cs.sync_interval_s);
            cfg.datastore = ds;
        }
    }

    // The top-level "datastore" block wins over a nested persistence.cloud_sync
    // block; when it is absent the cloud_sync-parsed values stand.
    if (f.datastore)
        cfg.datastore = f.datastore.value();

    // ── Declared DBs ──────────────────────────────────────────────────────────
    if (f.nodes) {
        for (const auto& entry : *f.nodes) {
            const auto& node = entry.second;
            const int db_val = node.number ? *node.number : (node.db_number ? *node.db_number : -1);
            if (db_val <= 0 || !sgrn::scl::isInRange<uint16_t>(db_val))
                continue;
            const auto db = static_cast<uint16_t>(db_val);
            cfg.declared_db_numbers.insert(db);
            if (node.allowed_ip)
                cfg.db_acls[db] = *node.allowed_ip;
            if (node.dirty_tracker)
                cfg.dirty_trackers[db] = SGRN_CONFIG_PATH(*node.dirty_tracker);
        }
    }

    return cfg;
}

} // namespace sgrn::gateway::config
