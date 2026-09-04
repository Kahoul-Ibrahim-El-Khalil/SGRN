#pragma once

#include <sgrn/Result.hpp>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace sqlite
{
class database;
}

namespace sgrn::gateway::database
{

struct BackendBatchRecord {
    int id;
    std::string file_path;
    int64_t ts_start;
    int64_t ts_end;
    std::string status;
};

struct ClientSession {
    int id;
    std::string ip;
    int64_t connect_time;
    int64_t disconnect_time{0};
    uint64_t bytes_sent{0};
    uint64_t bytes_received{0};
};

struct ConnectionRecord {
    std::string type;
    std::string remote_ip;
    std::string endpoint;
    int64_t first_seen;
    int64_t last_seen;
    int event_count;
};

class GatewayDatabase {
public:
    GatewayDatabase();
    ~GatewayDatabase();

    sgrn::Result<void> initialize(const std::string& t_state_dir);

    // Logging
    void logInfo(const std::string& t_message);
    void logWarning(const std::string& t_message);
    void logError(const std::string& t_message);

    // Events
    sgrn::Result<void> logEvent(const std::string& t_event_type, const std::string& t_origin_ip, const std::string& t_details);

    // Backend interactions
    sgrn::Result<void> recordPendingBatch(const std::string& t_file_path, int64_t t_ts_start, int64_t t_ts_end);
    sgrn::Result<void> markBatchSynced(int t_id);
    sgrn::Result<void> deleteBatch(int t_id);
    sgrn::Result<void> purgeSyncedBatches(int64_t t_older_than_ms = 0);
    sgrn::Result<std::vector<BackendBatchRecord>> getPendingBatches(int t_limit = 10);

    // Client Tracking
    sgrn::Result<int> recordSouthConnection(const std::string& t_ip);
    sgrn::Result<void> recordNorthConnection(const std::string& t_ip, const std::string& t_endpoint);
    sgrn::Result<void> recordClientDisconnect(int t_session_id, uint64_t t_bytes_sent, uint64_t t_bytes_received);
    sgrn::Result<std::vector<ClientSession>> getActiveSessions();
    sgrn::Result<std::vector<ConnectionRecord>> getConnections();

    struct LogEntry {
        int64_t timestamp;
        std::string level;
        std::string message;
    };
    sgrn::Result<std::vector<LogEntry>> getLogs(int t_limit = 100);

    // Metrics
    sgrn::Result<void> recordMetric(const std::string& t_name, double t_value);

    /**
     * @brief Exports historical tables (logs, events, metrics) to a JSON string.
     */
    sgrn::Result<std::string> exportHistoricalDataToJson();

    /**
     * @brief Clears historical tables and reclaims space.
     */
    sgrn::Result<void> clearHistoricalData();

private:
    sgrn::Result<void> logMessage(const std::string& t_level, const std::string& t_message);

    std::unique_ptr<sqlite::database> db_;
    std::mutex mu_;
};

using GatewayDatabaseSptr = std::shared_ptr<GatewayDatabase>;
} // namespace sgrn::gateway::database
