#include <sgrn/gateway/gateway.hpp>

using sgrn::Result;

namespace fs = std::filesystem;
using sgrn::utils::filesystem::expandUserPath;
namespace sgrn::gateway
{
Result<void, std::string> GatewayApplication::loadConfig(int t_argc, char** tp_argv) {
    bool gen_config = false;
    std::string config_out = "gateway.json";

    for (int i = 1; i < t_argc; ++i) {
        std::string arg = tp_argv[i];
        if (arg == "--generate-config") {
            gen_config = true;
        } else if (arg == "-o" && i + 1 < t_argc) {
            config_out = tp_argv[i + 1];
            i++;
        } else if (arg == "--schema" && i + 1 < t_argc) {
            schema_override_ = tp_argv[i + 1];
            i++;
        } else if (arg == "--policy" && i + 1 < t_argc) {
            policy_script_ = tp_argv[i + 1];
            i++;
        }
    }

    if (gen_config) {
        if (sgrn::utils::filesystem::writeStringToFile(config_out, sgrn::gateway::config::example)) {
            fmt::print("Successfully generated default config at {}\n", config_out);
            exit(EXIT_SUCCESS);
        } else {
            return fmt::format("Failed to write default config to {}", config_out);
        }
    }

    SGRN_RETURN_IF(t_argc < 2, "Usage: gateway <config.json> OR gateway --generate-config -o <config.json>");

    const fs::path config_path = expandUserPath(tp_argv[1]);

    SGRN_RETURN_IF(
        !fs::exists(config_path) || !fs::is_regular_file(config_path), fmt::format("Config File does not exist: {}", config_path.string()));

    auto config_res = parseNodeConfig(config_path.string());
    if (config_res.hasError()) {
        return fmt::format("Config Loading SclError: {}", config_res.error());
    }
    config_ = std::move(config_res.value());

    return {};
}

Result<void, std::string> GatewayApplication::loadSchema() {
    std::string reg_arg;
    if (!schema_override_.empty()) {
        reg_arg = expandUserPath(schema_override_);
    } else {
        const bool has_reg = !config_.schema_file.empty();
        const bool has_dir = !config_.symbols_dir.empty();
        if (!has_reg && !has_dir) {
            return "SclError: 'schema' or 'symbols_dir' must be specified in gateway.json or passed via --schema";
        }
        reg_arg = expandUserPath(has_reg ? config_.schema_file : config_.symbols_dir);
    }

    auto symbolic_store_res =
        fs::is_directory(reg_arg) ? PlcSchemaStore::loadFromDirectory(reg_arg) : PlcSchemaStore::loadFromFile(reg_arg);

    if (symbolic_store_res.hasError()) {
        return fmt::format("Registry load failed: {}", toString(symbolic_store_res.error()));
    }

    if (!symbolic_store_res.value().warnings().empty()) {
        bool has_critical = false;
        for (const auto& w : symbolic_store_res.value().warnings()) {
            if (w.find("unresolved") != std::string::npos || w.find("Conflict") != std::string::npos) {
                has_critical = true;
                SGRN_ERROR_LOG("CRITICAL — symbol parsing error: {}", w);
            } else {
                SGRN_WARN_LOG("Symbol Registry: {}", w);
            }
        }
        if (has_critical) {
            return "Gateway cannot start due to critical schema inconsistencies.";
        }
    }
    for (uint16_t db : config_.declared_db_numbers) {
        auto r = symbolic_store_res.value().getDb(db);
        if (!r.has_value() || !r.value()) {
            return fmt::format("DB{} declared but not in registry", db);
        }
    }
    symbolic_store_ = std::move(symbolic_store_res.value());
    return {};
}

Result<void, std::string> GatewayApplication::initSecurity() {
    // Always construct the SecurityManager, even in Relaxed mode — every
    // protocol adapter (S7, HTTP, WebSocket, Modbus, OPC-UA, EtherNet/IP)
    // holds a non-owning-in-spirit shared_ptr<SecurityManager> and calls
    // straight into it with no null check. Skipping construction here
    // leaves that pointer null and segfaults on the first authorized
    // request, regardless of what authorize*() would have returned.
    security_manager_ = std::make_shared<sgrn::gateway::SecurityManager>();

    if (!policy_script_.empty()) {
        if (auto r = security_manager_->loadPolicyScript(policy_script_); r.hasError()) {
            return fmt::format("Failed to load policy script '{}': {}", policy_script_, r.error());
        }
    } else if (!config_.security_script.empty()) {
        if (auto r = security_manager_->loadPolicyScript(config_.security_script); r.hasError()) {
            SGRN_WARN_LOG("Policy script '{}' failed: {} — falling back to legacy ACL", config_.security_script, r.error());
        }
    }

    security_manager_->setPolicy(config_.security_policy);
    for (const auto& [db, ip] : config_.db_acls) {
        security_manager_->setAllowedIp(db, ip);
    }
    return {};
}
Result<void, std::string> GatewayApplication::initTwin() {

    server_.setCacheEnabled(config_.cache_json_north);
    server_.attachState(plc_state_);
    if (auto r = server_.loadRegistry(symbolic_store_); r.hasError()) {
        return fmt::format("Server memory config failed: {}", toString(r.error()));
    }

    // A schema (re)load may have turned fields into symbolic enums — drop any
    // cached pre-rebuild JSON blobs so the first read re-serializes fresh.
    sgrn::gateway::core::TreeCacheEngine::instance().clear();

    if (server_.state()) {
        auto recovery_res = sgrn::gateway::core::recoverStateFromArchives(config_.state_dir, *server_.state(), symbolic_store_);
        if (recovery_res.hasError()) {
            SGRN_WARN_LOG("Recovery skipped: {}", recovery_res.error());
        } else {
            SGRN_INFO_LOG("Restored {} leaf values from {} ({} skipped)", recovery_res.value().leaves_restored,
                recovery_res.value().archive_used, recovery_res.value().leaves_skipped);
        }
    }

    srv_ctx_.server = &server_;

    return {};
}

Result<void, std::string> GatewayApplication::initThreading() {
    sgrn::gateway::core::GlobalContext::instance().run(kLightThreads, kLightThreads);

    heavy_pool_ = std::make_unique<asio::thread_pool>(kHeavyPoolThreads);
    light_work_.emplace(asio::make_work_guard(light_ctx_));
    light_thread_ = std::thread([this]() { light_ctx_.run(); });

    server_.processor()->setContexts(&light_ctx_, heavy_pool_.get());

    return {};
}

Result<void, std::string> GatewayApplication::wireTelemetry() {
    // ── Leaf-level updates (for UI/REST caching) ──────────────────────────
    // These are individual field updates published as LeafUpdate events.
    // TreeCacheEngine subscribes to these to invalidate cached JSON paths.
    server_.processor()->setOnFieldUpdate([](const sgrn::gateway::twin::FieldUpdateNotification& note) {
        TelemetryEvent ev{.type = sgrn::gateway::core::EventType::LeafUpdate,
            .db = note.db,
            .path = note.path,
            .json_value = std::make_shared<std::string>(note.json_value),
            .typed_leaf = note.typed_leaf,
            .timestamp = note.timestamp};
        TelemetryBroker::instance().publish(std::move(ev));
    });

    // ── Dirty path handler (for WebSocket/Persistence/Backend) ─────────────
    // When a dirty path is detected, this handler:
    //   1. Publishes individual LeafUpdate events for each dirty leaf
    //   2. Generates a DeltaSnapshot JSON for the dirty path
    //   3. Publishes a DeltaSnapshot event to the TelemetryBroker
    //
    // ARCHITECTURAL NOTE: The DeltaSnapshot JSON is broadcast to ALL subscribers
    // (WebSocketAdapter, PersistenceService, DatastoreBridge, OPC-UA). Each subscriber
    // receives a shared_ptr to the SAME string. They may parse it independently.
    // This is an intentional trade-off for loose coupling.
    //
    // The JSON is serialized ONCE here by PlcState::getDeltaSnapshot(), then:
    //   - WebSocket (firehose): sends directly, zero-copy, ~0μs overhead
    //   - WebSocket (field-filtered): parses once, filters, re-serializes
    //   - Persistence: parses, filters by namespace, re-serializes fields, compresses
    //   - DatastoreBridge: parses, batches, compresses
    //   - OPC-UA: parses, converts to OPC-UA types
    //
    // If multiple subscribers need to inspect the JSON, it may be parsed multiple
    // times. This is acceptable because:
    //   - Each subscriber runs on its own thread pool (light vs heavy)
    //   - Compression dominates the cost (~500μs vs ~200μs for parsing)
    //   - Batching amortizes the overhead across many fields
    server_.processor()->setDirtyHandler([this](std::vector<uint16_t> dirty_dbs) {
        // Publish individual leaf updates for TreeCacheEngine and other subscribers.
        for (const auto& db_num : dirty_dbs) {
            for (auto& note : server_.collectTypedDirtyLeaves(db_num)) {
                TelemetryEvent leaf;
                leaf.type = sgrn::gateway::core::EventType::LeafUpdate;
                leaf.db = note.db;
                leaf.path = note.path;
                leaf.typed_leaf = std::move(note.typed_leaf);
                leaf.timestamp = note.timestamp;
                TelemetryBroker::instance().publish(std::move(leaf));
            }
        }

        // Generate ONE DeltaSnapshot for all dirty DBs.
        // When a LeafDictionary is available (dictionary mode active), emit a
        // flat numeric-keyed snapshot directly: {"<leaf_id>": value, ...}.
        // This removes flattenNestedTree() from the WebSocket hot path entirely.
        // When no dictionary is configured (legacy/firehose mode), fall back to
        // the nested form: {"ReactorCore": {"field": value}}.
        const bool dict_ready = !leaf_dict_.path_to_id.empty();
        std::string snapshot =
            dict_ready ? server_.getDeltaSnapshotFlat(leaf_dict_.path_to_id, dirty_dbs) : server_.getDeltaSnapshot(dirty_dbs);
        if (snapshot.empty() || snapshot == "{}") {
            return;
        }

        TelemetryEvent ev;
        ev.type = sgrn::gateway::core::EventType::DeltaSnapshot;
        ev.json_value = std::make_shared<std::string>(std::move(snapshot));
        ev.timestamp = sgrn::utils::time::nowMilliseconds();
        ev.is_flat = dict_ready; // tells WebSocket adapter to skip flattenNestedTree

        // Build dirty_paths from DB numbers (reverse lookup to names for TreePath).
        for (const auto& db_num : dirty_dbs) {
            const auto* seg = server_.state()->findSegmentById(db_num);
            if (seg) {
                for (const auto& [name, entry] : server_.state()->segments()) {
                    if (entry.get() == seg) {
                        ev.dirty_paths.push_back(TreePath::fromDotted(name));
                        break;
                    }
                }
            }
        }
        TelemetryBroker::instance().publish(std::move(ev));
    });

    return {};
}

Result<void, std::string> GatewayApplication::initInfrastructure() {
    node_db_ = std::make_shared<sgrn::gateway::database::GatewayDatabase>();
    if (auto res = node_db_->initialize(config_.state_dir); res.hasError()) {
        return fmt::format("Failed to initialize GatewayDatabase in {}: {}", config_.state_dir, res.error());
    }

    srv_ctx_.node_db = node_db_;

    // Build the shared LeafDictionary once from the schema store.
    // Both PersistenceService and WebSocketAdapter reuse this single
    // instance so ID assignment never drifts between the two consumers.
    leaf_dict_ = sgrn::gateway::twin::LeafDictionary::buildFrom(symbolic_store_);

    persistence_service_ = std::make_unique<sgrn::gateway::database::PersistenceService>(heavy_pool_.get());
    if (config_.persistence.enabled) {
        persistence_service_->setLeafDictionary(leaf_dict_);
        if (auto res = persistence_service_->configure(
                config_.persistence, config_.state_dir, node_db_, symbolic_store_.toJson(), &symbolic_store_);
            res.hasError()) {
            return fmt::format("PersistenceService configuration failed: {}", res.error());
        }
    }

    if (config_.datastore.has_value()) {
        bridge_.emplace(heavy_pool_.get());
        bridge_->setReconnectBase(config_.reconnect_ms);
        std::string reg_arg = fs::is_directory(config_.symbols_dir) ? config_.symbols_dir : config_.schema_file;
        if (auto res = bridge_->configure(*config_.datastore, config_.state_dir, reg_arg, node_db_); res.hasError()) {
            return fmt::format("DatastoreBridge configuration failed: {}", res.error());
        }
    }

    return {};
}

Result<void, std::string> GatewayApplication::startAdapters() {
    if (config_.s7.has_value()) {
        s7_adapter_.emplace(server_, security_manager_);
        s7_adapter_->setMaxClientsConfig(config_.s7->max_clients);
        s7_adapter_->setPduSizeConfig(config_.s7->pdu_size);

        if (auto r = s7_adapter_->bindToPlcMemory(); r.hasError()) {
            return fmt::format("S7 Adapter memory bind failed: {}", toString(r.error()));
        }

        startAdapter("S7 Adapter", config_.s7->port, [this]() { return s7_adapter_->start(config_.s7->ip, config_.s7->port); });

        s7_adapter_->setEventsCallback(sgrn::gateway::adapters::s7::onServerEvent, &srv_ctx_);
    } else {
        SGRN_WARN_LOG("Gateway is starting WITHOUT a southbound S7 connection. It will act as a northbound-only server.");
    }

    if (config_.opcua.has_value()) {
        opc_adapter_.emplace();
        startAdapter("OPC-UA Adapter", config_.opcua->port,
            [this]() { return opc_adapter_->start(config_.opcua->ip, config_.opcua->port, symbolic_store_, server_, security_manager_); });
    }

    if (config_.modbus.has_value()) {
        modbus_adapter_.emplace(server_, security_manager_);
        startAdapter("Modbus Adapter", config_.modbus->port,
            [this]() { return modbus_adapter_->start(config_.modbus->ip, config_.modbus->port, symbolic_store_); });
    }

    if (config_.ethernetip.has_value()) {
        eip_adapter_.emplace(server_, security_manager_);
        startAdapter("EtherNet/IP Adapter", config_.ethernetip->port,
            [this]() { return eip_adapter_->start(config_.ethernetip->ip, config_.ethernetip->port, symbolic_store_); });
    }

    if (config_.http.has_value()) {
        http_adapter_.emplace();
        auto vmap = modbus_adapter_ ? &modbus_adapter_->virtualMap() : nullptr;
        uint16_t ws_port = config_.websocket.has_value() ? config_.websocket->port : 0;
        startAdapter("HTTP Adapter", config_.http->port, [this, vmap, ws_port]() {
            return http_adapter_->start(
                config_.http->ip, config_.http->port, symbolic_store_, server_, node_db_, security_manager_, vmap, ws_port);
        });
    }

    if (config_.websocket.has_value()) {
        ws_facade_.emplace();
        ws_facade_->setLeafDictionary(leaf_dict_);
        startAdapter("WebSocket Facade", config_.websocket->port, [this]() {
            // Seed every connecting client with the current full plant state
            // (which includes the state recovered from disk at boot), so the
            // WebSocket stream reclaims the twin's initial state instead of
            // showing an empty image until the first PLC delta.
            return ws_facade_->start(
                config_.websocket->ip, config_.websocket->port, security_manager_, &symbolic_store_,
                [this]() { return server_.getDigitalTwinJsonString(); },
                [this](uint16_t db, size_t offset, size_t size, uint8_t* out) -> Result<void, std::string> {
                    if (auto r = server_.readDbMemory(db, offset, size, out); !r) {
                        return std::string(toString(r.error()));
                    }
                    return {};
                });
        });
    }

    if (active_protocols_ == 0) {
        return fmt::format("No network protocols are enabled in the configuration! At least one protocol (S7, HTTP, OPC-UA, etc.) "
                           "must be enabled to start.");
    }

    return {};
}

void GatewayApplication::feedInitialAnchor() {
    std::string full_snapshot = server_.getDigitalTwinJsonString();
    if (!full_snapshot.empty() && full_snapshot != "{}") {
        // PersistenceService is the sole writer - it handles both local persistence
        // and creates the files that DatastoreBridge will later upload.
        if (persistence_service_ && persistence_service_->isActive()) {
            persistence_service_->ingestFullTree(full_snapshot);
        }
    }
}

void GatewayApplication::run() {
    std::promise<void> shutdown_promise;
    auto shutdown_future = shutdown_promise.get_future();

    asio::signal_set signals(light_ctx_, SIGINT, SIGTERM);
    signals.async_wait([&](auto, int signal_number) {
        SGRN_WARN_LOG("Shutdown signal received ({}).", signal_number);
        shutdown_promise.set_value();
    });

#ifdef SIGHUP
    // Handle SIGHUP for hot-reloading policy script
    // Note: Using a shared pointer to keep signals alive if needed, but since it's on stack in run(),
    // it lives as long as the run loop does.
    auto hup_signals = std::make_shared<asio::signal_set>(light_ctx_, SIGHUP);
    std::function<void(std::error_code, int)> hup_handler;
    hup_handler = [this, hup_signals, &hup_handler](std::error_code ec, int) {
        if (ec)
            return;
        SGRN_INFO_LOG("SIGHUP received. Reloading policy script...");
        if (security_manager_) {
            std::string script_path = !policy_script_.empty() ? policy_script_ : config_.security_script;
            if (!script_path.empty()) {
                if (auto r = security_manager_->loadPolicyScript(script_path); r.hasError()) {
                    SGRN_ERROR_LOG("Failed to reload policy script: {}", r.error());
                } else {
                    SGRN_INFO_LOG("Policy script successfully reloaded.");
                }
            }
        }
        hup_signals->async_wait(hup_handler);
    };
    hup_signals->async_wait(hup_handler);
#endif

    shutdown_future.wait();
}

void GatewayApplication::shutdown() {
    SGRN_WARN_LOG("Shutting down...");
    if (modbus_adapter_.has_value()) {
        modbus_adapter_->stop();
    }
    if (persistence_service_) {
        persistence_service_->stop();
    }
    GlobalContext::instance().stop();
    light_work_.reset();
    light_ctx_.stop();

    if (light_thread_.joinable()) {
        light_thread_.join();
    }

    if (heavy_pool_) {
        heavy_pool_->stop();
        heavy_pool_->join();
    }
    SGRN_INFO_LOG("Done.");
}

} // namespace sgrn::gateway
