
#include <sgrn/gateway/config/gateway.hpp>
#include <sgrn/utils/json.hpp>
#include <sgrn/utils/strings.hpp>
#include <fstream>

namespace sgrn::gateway::config
{

sgrn::Result<GatewayConfig> parseNodeConfig(const std::string& t_path) {
    auto parsed = sgrn::utils::json::deserializeFromFile(t_path);
    if (parsed.hasError()) {
        return fmt::format("Failed to deserialize config: {}", parsed.error());
    }

    const rapidjson::Value& root = parsed.value();
    GatewayConfig cfg;

    if (root.HasMember("listen") && root["listen"].IsObject()) {
        const auto& listen = root["listen"];
        if (listen.HasMember("s7") && listen["s7"].IsObject()) {
            S7Config s7;
            const auto& s7_node = listen["s7"];
            if (s7_node.HasMember("ip") && s7_node["ip"].IsString())
                s7.ip = s7_node["ip"].GetString();
            if (s7_node.HasMember("port"))
                s7.port = static_cast<uint16_t>(s7_node["port"].GetUint());
            if (s7_node.HasMember("max_clients"))
                s7.max_clients = s7_node["max_clients"].GetUint();
            if (s7_node.HasMember("pdu_size"))
                s7.pdu_size = s7_node["pdu_size"].GetUint();
            if (s7_node.HasMember("little_endian"))
                s7.little_endian = s7_node["little_endian"].GetBool();
            cfg.s7 = s7;
        }
        if (listen.HasMember("opcua") && listen["opcua"].IsObject()) {
            OpcUaConfig opcua;
            const auto& opc = listen["opcua"];
            if (opc.HasMember("ip") && opc["ip"].IsString())
                opcua.ip = opc["ip"].GetString();
            if (opc.HasMember("port"))
                opcua.port = static_cast<uint16_t>(opc["port"].GetUint());
            cfg.opcua = opcua;
        }
        if (listen.HasMember("http") && listen["http"].IsObject()) {
            HttpConfig http;
            const auto& ht = listen["http"];
            if (ht.HasMember("ip") && ht["ip"].IsString())
                http.ip = ht["ip"].GetString();
            if (ht.HasMember("port"))
                http.port = static_cast<uint16_t>(ht["port"].GetUint());
            cfg.http = http;
        }
        if (listen.HasMember("websocket") && listen["websocket"].IsObject()) {
            WebSocketConfig websocket;
            const auto& ws = listen["websocket"];
            if (ws.HasMember("ip") && ws["ip"].IsString())
                websocket.ip = ws["ip"].GetString();
            if (ws.HasMember("port"))
                websocket.port = static_cast<uint16_t>(ws["port"].GetUint());
            cfg.websocket = websocket;
        }
        if (listen.HasMember("modbus") && listen["modbus"].IsObject()) {
            ModbusConfig modbus;
            const auto& mb = listen["modbus"];
            if (mb.HasMember("ip") && mb["ip"].IsString())
                modbus.ip = mb["ip"].GetString();
            if (mb.HasMember("port"))
                modbus.port = static_cast<uint16_t>(mb["port"].GetUint());
            cfg.modbus = modbus;
        }

        if (listen.HasMember("ethernetip") && listen["ethernetip"].IsObject()) {
            EthernetIpConfig eip;
            const auto& ep = listen["ethernetip"];
            if (ep.HasMember("ip") && ep["ip"].IsString())
                eip.ip = ep["ip"].GetString();
            if (ep.HasMember("port") && ep["port"].IsUint())
                eip.port = static_cast<uint16_t>(ep["port"].GetUint());
            cfg.ethernetip = eip;
        }
    }

    if (root.HasMember("security_policy") && root["security_policy"].IsString() &&
        sgrn::utils::strings::toLower(root["security_policy"].GetString()) == "strict")
        cfg.security_policy = ::sgrn::scl::SecurityPolicy::Strict;
    if (root.HasMember("security_script") && root["security_script"].IsString())
        cfg.security_script = root["security_script"].GetString();
    if (root.HasMember("state_dir") && root["state_dir"].IsString())
        cfg.state_dir = root["state_dir"].GetString();
    if (root.HasMember("schema") && root["schema"].IsString())
        cfg.schema_file = root["schema"].GetString();
    if (root.HasMember("symbols_dir") && root["symbols_dir"].IsString())
        cfg.symbols_dir = root["symbols_dir"].GetString();
    if (root.HasMember("reconnect_ms"))
        cfg.reconnect_ms = root["reconnect_ms"].GetInt();
    if (root.HasMember("cache_json_north"))
        cfg.cache_json_north = root["cache_json_north"].GetBool();
    if (root.HasMember("verbose"))
        cfg.verbose = root["verbose"].GetBool();

    if (root.HasMember("debug") && root["debug"].IsObject()) {
        const auto& d = root["debug"];
        if (d.HasMember("incoming"))
            cfg.debug_incoming = d["incoming"].GetBool();
        if (d.HasMember("tree"))
            cfg.debug_tree = d["tree"].GetBool();
    }

    if (root.HasMember("database_rotation_interval_s"))
        cfg.database_rotation_interval_s = root["database_rotation_interval_s"].GetUint();

    // ── Persistence (historian / local archiver) ─────────────────────────────
    if (root.HasMember("persistence") && root["persistence"].IsObject()) {
        const auto& p = root["persistence"];
        if (p.HasMember("enabled"))
            cfg.persistence.enabled = p["enabled"].GetBool();
        if (p.HasMember("mode") && p["mode"].IsString())
            cfg.persistence.mode = p["mode"].GetString();
        if (p.HasMember("namespaces") && p["namespaces"].IsArray()) {
            for (const auto& ns : p["namespaces"].GetArray()) {
                if (ns.IsString())
                    cfg.persistence.namespaces.push_back(ns.GetString());
            }
        }
        if (p.HasMember("atomic_window_ms"))
            cfg.persistence.atomic_window_ms = p["atomic_window_ms"].GetUint();
        if (p.HasMember("batch_size"))
            cfg.persistence.batch_size = p["batch_size"].GetUint();
        if (p.HasMember("batch_interval_s"))
            cfg.persistence.batch_interval_s = p["batch_interval_s"].GetUint();
        if (p.HasMember("anchor_interval_s"))
            cfg.persistence.anchor_interval_s = p["anchor_interval_s"].GetUint();
        if (p.HasMember("anchor_change_count"))
            cfg.persistence.anchor_change_count = p["anchor_change_count"].GetUint();
        if (p.HasMember("zstd_level"))
            cfg.persistence.zstd_level = static_cast<uint8_t>(p["zstd_level"].GetUint());
        if (p.HasMember("anchor_zstd_level"))
            cfg.persistence.anchor_zstd_level = static_cast<uint8_t>(p["anchor_zstd_level"].GetUint());
    }

    if (root.HasMember("nodes") && root["nodes"].IsObject()) {
        for (auto it = root["nodes"].MemberBegin(); it != root["nodes"].MemberEnd(); ++it) {
            const auto& node = it->value;
            const int db_val =
                node.HasMember("number") ? node["number"].GetInt() : (node.HasMember("db_number") ? node["db_number"].GetInt() : -1);
            if (db_val > 0 && sgrn::scl::isInRange<uint16_t>(db_val)) {
                const uint16_t db = static_cast<uint16_t>(db_val);
                cfg.declared_db_numbers.insert(db);
                if (node.HasMember("allowed_ip") && node["allowed_ip"].IsString())
                    cfg.db_acls[db] = node["allowed_ip"].GetString();
                if (node.HasMember("dirty_tracker") && node["dirty_tracker"].IsString())
                    cfg.dirty_trackers[db] = node["dirty_tracker"].GetString();
            }
        }
    }
    DatastoreConnectionConfig datastore;
    bool has_datastore_or_buffer = false;

    if (root.HasMember("datastore") && root["datastore"].IsObject()) {
        has_datastore_or_buffer = true;
        const auto& b = root["datastore"];
        if (b.HasMember("url") && b["url"].IsString())
            datastore.url = b["url"].GetString();
        if (b.HasMember("public_token") && b["public_token"].IsString())
            datastore.public_token = b["public_token"].GetString();
        if (b.HasMember("private_token") && b["private_token"].IsString())
            datastore.private_token = b["private_token"].GetString();
        if (b.HasMember("object_name") && b["object_name"].IsString())
            datastore.object_name = b["object_name"].GetString();
        if (b.HasMember("sync_interval_s"))
            datastore.sync_interval_s = b["sync_interval_s"].GetUint();
        if (b.HasMember("telemetry_enabled"))
            datastore.telemetry_enabled = b["telemetry_enabled"].GetBool();
        if (b.HasMember("upload_mode") && b["upload_mode"].IsString())
            datastore.upload_mode = b["upload_mode"].GetString();
        if (b.HasMember("vfs_remote_dir") && b["vfs_remote_dir"].IsString())
            datastore.vfs_remote_dir = b["vfs_remote_dir"].GetString();

        // Also check if local buffer settings are in datastore block for backward compatibility
        if (b.HasMember("batch_size"))
            datastore.batch_size = b["batch_size"].GetUint();
        if (b.HasMember("batch_interval_s"))
            datastore.batch_interval_s = b["batch_interval_s"].GetUint();
        if (b.HasMember("snapshot_mode") && b["snapshot_mode"].IsString())
            datastore.snapshot_mode = b["snapshot_mode"].GetString();
        if (b.HasMember("zstd_level"))
            datastore.zstd_level = static_cast<uint8_t>(b["zstd_level"].GetUint());
        if (b.HasMember("aggressive_zstd_level"))
            datastore.aggressive_zstd_level = static_cast<uint8_t>(b["aggressive_zstd_level"].GetUint());
        if (b.HasMember("enable_aggressive_compression"))
            datastore.enable_aggressive_compression = b["enable_aggressive_compression"].GetBool();
        if (b.HasMember("offline_persistence"))
            datastore.offline_persistence = b["offline_persistence"].GetBool();
    }

    if (root.HasMember("telemetry_buffer") && root["telemetry_buffer"].IsObject()) {
        has_datastore_or_buffer = true;
        const auto& buf = root["telemetry_buffer"];
        if (buf.HasMember("batch_size"))
            datastore.batch_size = buf["batch_size"].GetUint();
        if (buf.HasMember("batch_interval_s"))
            datastore.batch_interval_s = buf["batch_interval_s"].GetUint();
        if (buf.HasMember("snapshot_mode") && buf["snapshot_mode"].IsString())
            datastore.snapshot_mode = buf["snapshot_mode"].GetString();
        if (buf.HasMember("zstd_level"))
            datastore.zstd_level = static_cast<uint8_t>(buf["zstd_level"].GetUint());
        if (buf.HasMember("aggressive_zstd_level"))
            datastore.aggressive_zstd_level = static_cast<uint8_t>(buf["aggressive_zstd_level"].GetUint());
        if (buf.HasMember("enable_aggressive_compression"))
            datastore.enable_aggressive_compression = buf["enable_aggressive_compression"].GetBool();
        if (buf.HasMember("offline_persistence"))
            datastore.offline_persistence = buf["offline_persistence"].GetBool();
    }

    if (has_datastore_or_buffer) {
        cfg.datastore = datastore;
    }

    return cfg;
}

} // namespace sgrn::gateway::config
