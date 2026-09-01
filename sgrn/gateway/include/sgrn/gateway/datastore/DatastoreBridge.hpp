#pragma once

#include <sgrn/gateway/config/datastore.hpp>
#include <sgrn/gateway/core/snapshot.hpp>
#include <sgrn/gateway/database/GatewayDatabase.hpp>
#include <sgrn/sdk/SgrnClient.hpp>
#include <rapidjson/document.h>

#include <sgrn/Result.hpp>
#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace sgrn::gateway::backend
{

using namespace sgrn::gateway::config;

/**
 * @brief Manages the connection and data upload to the SGRN backend.
 *
 * ARCHITECTURAL ROLE
 * ──────────────────
 * DatastoreBridge is the *cloud uploader* in the gateway northbound stack.
 * It does NOT subscribe to TelemetryBroker or write any files itself.
 *
 * Instead, it acts as a pure uploader:
 *   1. PersistenceService writes .json.zst files to state_dir/unsynced/
 *   2. PersistenceService registers each file in the GatewayDatabase
 *   3. DatastoreBridge polls the database for pending batches
 *   4. DatastoreBridge uploads them to the SGRN backend
 *   5. DatastoreBridge moves them to state_dir/synced/ and marks as synced
 *
 * This separation ensures:
 *   - Single writer (PersistenceService) - no duplicate files
 *   - Offline-capable (PersistenceService works without backend)
 *   - Clean separation: persistence vs cloud sync
 */
class DatastoreBridge {
public:
    DatastoreBridge(asio::thread_pool* tp_heavy_pool)
        : heavy_pool_(tp_heavy_pool) {
    }

    ~DatastoreBridge();

    /**
     * @brief Configures the bridge and starts the uploader.
     */
    sgrn::Result<void> configure(const DatastoreConnectionConfig& t_cfg, const std::string& t_state_dir,
        const std::string& t_registry_source, std::shared_ptr<sgrn::gateway::database::GatewayDatabase> tsp_db);

    bool enabled() const;
    bool reconnectDue() const;

    void setReconnectBase(int t_ms);
    void updateConfig(const DatastoreConnectionConfig& t_cfg);
    void stop();

    /**
     * @brief Uploads a compressed log/database archive to the backend.
     */
    void uploadLogArchive(const std::string& t_file_path);

private:
    void startUploader();
    void scheduleUploaderTick();
    void tryConnect();
    void scheduleReconnect();
    void stopClient();

    void processPendingBatches();
    bool uploadRaw(const std::string& t_file_path);

    void moveToSynced(const std::string& t_file_path);
    void markSynced(int t_id, const std::string& t_file_path);

    [[maybe_unused]] asio::thread_pool* heavy_pool_ = nullptr;
    DatastoreConnectionConfig cfg_;
    std::string state_dir_;
    std::string unsynced_dir_;
    std::string synced_dir_;
    std::string registry_source_;
    std::unique_ptr<sgrn::sdk::SgrnClient> client_;
    std::chrono::steady_clock::time_point next_reconnect_{std::chrono::steady_clock::now()};
    int reconnect_ms_base_{5000};
    int cfg_reconnect_ms_{5000};

    std::shared_ptr<sgrn::gateway::database::GatewayDatabase> node_db_;
    std::atomic<int> pending_tasks_{0};
    [[maybe_unused]] const int max_pending_tasks_ = 100;

    [[maybe_unused]] int64_t batch_start_ts_{0};
    [[maybe_unused]] int64_t last_flush_ts_{0};
    std::atomic<bool> uploader_running_{false};
    std::atomic<bool> session_ready_{false};
    std::atomic<bool> connecting_{false};
};

} // namespace sgrn::gateway::backend
