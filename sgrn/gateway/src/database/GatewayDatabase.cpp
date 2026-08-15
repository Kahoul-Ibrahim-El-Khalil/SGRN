#include <fmt/color.h>
#include <fmt/core.h>
#include <sgrn/Result.hpp>
#include <sgrn/gateway/database/GatewayDatabase.hpp>
#include <sgrn/utils/time.hpp>
#include <filesystem>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <sqlite_modern_cpp.h>
#include <string_view>

namespace sgrn::gateway::database
{
namespace
{

constexpr std::string_view kDatabaseFileName = "gateway.sqlite3";

}
GatewayDatabase::GatewayDatabase() = default;

GatewayDatabase::~GatewayDatabase() = default;

static sgrn::Result<void> createLogsTable(sqlite::database& t_db) {
    try {
        t_db << "CREATE TABLE IF NOT EXISTS logs ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "timestamp INTEGER,"
                "level TEXT,"
                "message TEXT"
                ");";
        return {};
    } catch (const std::exception& e) {
        return fmt::format("Failed to create logs table: {}", e.what());
    }
}

static sgrn::Result<void> createEventsTable(sqlite::database& t_db) {
    try {
        t_db << "CREATE TABLE IF NOT EXISTS events ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "timestamp INTEGER,"
                "event_type TEXT,"
                "origin_ip TEXT,"
                "details TEXT"
                ");";
        return {};
    } catch (const std::exception& e) {
        return fmt::format("Failed to create events table: {}", e.what());
    }
}

static sgrn::Result<void> createPendingBatchesTable(sqlite::database& t_db) {
    try {
        t_db << "CREATE TABLE IF NOT EXISTS pending_batches ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "file_path TEXT UNIQUE,"
                "ts_start INTEGER,"
                "ts_end INTEGER,"
                "status TEXT"
                ");";
        return {};
    } catch (const std::exception& e) {
        return fmt::format("Failed to create pending_batches table: {}", e.what());
    }
}

static sgrn::Result<void> createClientsTable(sqlite::database& t_db) {
    try {
        t_db << "CREATE TABLE IF NOT EXISTS clients ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "ip TEXT,"
                "connect_time INTEGER,"
                "disconnect_time INTEGER DEFAULT 0,"
                "bytes_sent INTEGER DEFAULT 0,"
                "bytes_received INTEGER DEFAULT 0"
                ");";
        return {};
    } catch (const std::exception& e) {
        return fmt::format("Failed to create clients table: {}", e.what());
    }
}

static sgrn::Result<void> createMetricsTable(sqlite::database& t_db) {
    try {
        t_db << "CREATE TABLE IF NOT EXISTS metrics ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "timestamp INTEGER,"
                "name TEXT,"
                "value REAL"
                ");";
        return {};
    } catch (const std::exception& e) {
        return fmt::format("Failed to create metrics table: {}", e.what());
    }
}

static sgrn::Result<void> createConnectionsTable(sqlite::database& t_db) {
    try {
        t_db << "CREATE TABLE IF NOT EXISTS connections ("
                "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                "type TEXT,"
                "remote_ip TEXT,"
                "endpoint TEXT,"
                "first_seen INTEGER,"
                "last_seen INTEGER,"
                "event_count INTEGER DEFAULT 1,"
                "UNIQUE(type, remote_ip, endpoint)"
                ");";
        return {};
    } catch (const std::exception& e) {
        return fmt::format("Failed to create connections table: {}", e.what());
    }
}
sgrn::Result<std::filesystem::path, std::string> createDatabaseFile(const std::string& t_file_dir, const std::string_view t_file_name) {
    try {
        std::filesystem::create_directories(t_file_dir);
        return std::filesystem::path(t_file_dir) / t_file_name;

    } catch (const std::exception& e) {
        return fmt::format("Failed to create Database file: {}/{}, details: {}", t_file_dir, t_file_name, e.what());
    }
}
sgrn::Result<void, std::string> GatewayDatabase::initialize(const std::string& t_state_dir) {
    const auto db_path = createDatabaseFile(t_state_dir, kDatabaseFileName);
    if (db_path.hasError()) {
        return db_path.error();
    }
    std::lock_guard<std::mutex> lock(mu_);
    db_ = std::make_unique<sqlite::database>(db_path.value().string());

    if (auto res = createLogsTable(*db_); res.hasError())
        return res;
    if (auto res = createEventsTable(*db_); res.hasError())
        return res;
    if (auto res = createPendingBatchesTable(*db_); res.hasError())
        return res;
    if (auto res = createClientsTable(*db_); res.hasError())
        return res;
    if (auto res = createMetricsTable(*db_); res.hasError())
        return res;
    if (auto res = createConnectionsTable(*db_); res.hasError())
        return res;

    // LOW-3: Prune tables to a maximum of 10 000 rows on startup so the
    // SQLite file does not grow unboundedly on long-running edge devices.
    constexpr size_t kMaxRows = 10000;
    try {
        *db_ << "DELETE FROM logs WHERE id NOT IN (SELECT id FROM logs ORDER BY id DESC LIMIT ?);" << kMaxRows;
        *db_ << "DELETE FROM events WHERE id NOT IN (SELECT id FROM events ORDER BY id DESC LIMIT ?);" << kMaxRows;
        *db_ << "DELETE FROM metrics WHERE id NOT IN (SELECT id FROM metrics ORDER BY id DESC LIMIT ?);" << kMaxRows;
        *db_ << "VACUUM;";
    } catch (...) { /* non-fatal */
    }

    return {};
}

sgrn::Result<void> GatewayDatabase::logMessage(const std::string& t_level, const std::string& t_message) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_)
        return "Database not initialized";
    try {
        int64_t now = sgrn::utils::time::nowMilliseconds();
        *db_ << "INSERT INTO logs (timestamp, level, message) VALUES (?, ?, ?);" << now << t_level << t_message;
        return {};
    } catch (const std::exception& e) {
        return fmt::format("logMessage failed: {}", e.what());
    }
}

void GatewayDatabase::logInfo(const std::string& t_message) {
    (void)logMessage("INFO", t_message);
}

void GatewayDatabase::logWarning(const std::string& t_message) {
    (void)logMessage("WARNING", t_message);
}

void GatewayDatabase::logError(const std::string& t_message) {
    (void)logMessage("ERROR", t_message);
}

sgrn::Result<void> GatewayDatabase::logEvent(
    const std::string& t_event_type, const std::string& t_origin_ip, const std::string& t_details) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_)
        return "Database not initialized";
    try {
        int64_t now = sgrn::utils::time::nowMilliseconds();
        *db_ << "INSERT INTO events (timestamp, event_type, origin_ip, details) VALUES (?, ?, ?, ?);" << now << t_event_type << t_origin_ip
             << t_details;
        return {};
    } catch (const std::exception& e) {
        return fmt::format("logEvent failed: {}", e.what());
    }
}

sgrn::Result<void> GatewayDatabase::recordPendingBatch(const std::string& t_file_path, int64_t t_ts_start, int64_t t_ts_end) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_)
        return "Database not initialized";
    try {
        *db_ << "INSERT INTO pending_batches (file_path, ts_start, ts_end, status) VALUES (?, ?, ?, ?);" << t_file_path << t_ts_start
             << t_ts_end << "pending";
        return {};
    } catch (const std::exception& e) {
        return fmt::format("recordPendingBatch failed: {}", e.what());
    }
}

sgrn::Result<void> GatewayDatabase::markBatchSynced(int t_id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_)
        return "Database not initialized";
    try {
        *db_ << "UPDATE pending_batches SET status = 'synced' WHERE id = ?;" << t_id;
        return {};
    } catch (const std::exception& e) {
        return fmt::format("markBatchSynced failed: {}", e.what());
    }
}

sgrn::Result<void> GatewayDatabase::deleteBatch(int t_id) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_)
        return "Database not initialized";
    try {
        *db_ << "DELETE FROM pending_batches WHERE id = ?;" << t_id;
        return {};
    } catch (const std::exception& e) {
        return fmt::format("deleteBatch failed: {}", e.what());
    }
}

sgrn::Result<std::vector<BackendBatchRecord>> GatewayDatabase::getPendingBatches(int t_limit) {
    std::vector<BackendBatchRecord> batches;
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_)
        return "Database not initialized";
    try {
        *db_ << "SELECT id, file_path, ts_start, ts_end, status FROM pending_batches WHERE status = 'pending' ORDER BY id ASC LIMIT ?;"
             << t_limit >>
            [&batches](int t_id, std::string t_file_path, int64_t t_ts_start, int64_t t_ts_end, std::string status) {
                batches.push_back({t_id, t_file_path, t_ts_start, t_ts_end, status});
            };
        return batches;
    } catch (const std::exception& e) {
        return fmt::format("getPendingBatches failed: {}", e.what());
    }
}

sgrn::Result<int> GatewayDatabase::recordSouthConnection(const std::string& t_ip) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_)
        return "Database not initialized";
    try {
        int64_t now = sgrn::utils::time::nowMilliseconds();

        // 1. Log to legacy clients table
        *db_ << "INSERT INTO clients (ip, connect_time) VALUES (?, ?);" << t_ip << now;
        int cid = static_cast<int>(db_->last_insert_rowid());

        // 2. Log to unified connections table
        *db_ << "INSERT INTO connections (type, remote_ip, endpoint, first_seen, last_seen, event_count) "
                "VALUES ('South', ?, 'Snap7', ?, ?, 1) "
                "ON CONFLICT(type, remote_ip, endpoint) DO UPDATE SET "
                "last_seen = excluded.last_seen, "
                "event_count = event_count + 1;"
             << t_ip << now << now;

        return cid;
    } catch (const std::exception& e) {
        return fmt::format("recordSouthConnection failed: {}", e.what());
    }
}

sgrn::Result<void> GatewayDatabase::recordNorthConnection(const std::string& t_ip, const std::string& t_endpoint) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_)
        return "Database not initialized";
    try {
        int64_t now = sgrn::utils::time::nowMilliseconds();
        *db_ << "INSERT INTO connections (type, remote_ip, endpoint, first_seen, last_seen, event_count) "
                "VALUES ('North', ?, ?, ?, ?, 1) "
                "ON CONFLICT(type, remote_ip, endpoint) DO UPDATE SET "
                "last_seen = excluded.last_seen, "
                "event_count = event_count + 1;"
             << t_ip << t_endpoint << now << now;
        return {};
    } catch (const std::exception& e) {
        return fmt::format("recordNorthConnection failed: {}", e.what());
    }
}

sgrn::Result<void> GatewayDatabase::recordClientDisconnect(int t_session_id, uint64_t t_bytes_sent, uint64_t t_bytes_received) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_)
        return "Database not initialized";
    try {
        int64_t now = sgrn::utils::time::nowMilliseconds();
        *db_ << "UPDATE clients SET disconnect_time = ?, bytes_sent = ?, bytes_received = ? WHERE id = ?;" << now << t_bytes_sent
             << t_bytes_received << t_session_id;
        return {};
    } catch (const std::exception& e) {
        return fmt::format("recordClientDisconnect failed: {}", e.what());
    }
}

sgrn::Result<std::vector<ClientSession>> GatewayDatabase::getActiveSessions() {
    std::vector<ClientSession> sessions;
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_)
        return "Database not initialized";
    try {
        *db_ << "SELECT id, ip, connect_time, bytes_sent, bytes_received FROM clients WHERE disconnect_time = 0;" >>
            [&sessions](int t_id, std::string t_ip, int64_t connect, uint64_t sent, uint64_t recv) {
                sessions.push_back({t_id, t_ip, connect, 0, sent, recv});
            };
        return sessions;
    } catch (const std::exception& e) {
        return fmt::format("getActiveSessions failed: {}", e.what());
    }
}

sgrn::Result<std::vector<ConnectionRecord>> GatewayDatabase::getConnections() {
    std::vector<ConnectionRecord> records;
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_)
        return "Database not initialized";
    try {
        *db_ << "SELECT type, remote_ip, endpoint, first_seen, last_seen, event_count FROM connections ORDER BY last_seen DESC;" >>
            [&records](std::string type, std::string t_ip, std::string endp, int64_t first, int64_t last, int count) {
                records.push_back({type, t_ip, endp, first, last, count});
            };
        return records;
    } catch (const std::exception& e) {
        return fmt::format("getConnections failed: {}", e.what());
    }
}

sgrn::Result<void> GatewayDatabase::recordMetric(const std::string& t_name, double t_value) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_)
        return "Database not initialized";
    try {
        int64_t now = sgrn::utils::time::nowMilliseconds();
        *db_ << "INSERT INTO metrics (timestamp, name, value) VALUES (?, ?, ?);" << now << t_name << t_value;
        return {};
    } catch (const std::exception& e) {
        return fmt::format("recordMetric failed: {}", e.what());
    }
}

sgrn::Result<std::vector<GatewayDatabase::LogEntry>> GatewayDatabase::getLogs(int t_limit) {
    std::vector<LogEntry> logs;
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_)
        return "Database not initialized";
    try {
        *db_ << "SELECT timestamp, level, message FROM logs ORDER BY id DESC LIMIT ?;" << t_limit >>
            [&logs](int64_t ts, std::string t_level, std::string msg) { logs.push_back({ts, t_level, msg}); };
        return logs;
    } catch (const std::exception& e) {
        return fmt::format("getLogs failed: {}", e.what());
    }
}

sgrn::Result<std::string> GatewayDatabase::exportHistoricalDataToJson() {
    if (!db_)
        return "Database not initialized";

    try {
        // MED-8: Read each table in its own short lock window so logMessage /
        // logEvent callers are not starved during the full table scan.
        std::vector<std::tuple<int64_t, std::string, std::string>> logs;
        std::vector<std::tuple<int64_t, std::string, std::string, std::string>> events;
        std::vector<std::tuple<int64_t, std::string, double>> metrics;

        {
            std::lock_guard<std::mutex> lk(mu_);
            *db_ << "SELECT timestamp, level, message FROM logs;" >>
                [&](int64_t ts, std::string lvl, std::string msg) { logs.emplace_back(ts, std::move(lvl), std::move(msg)); };
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            *db_ << "SELECT timestamp, event_type, origin_ip, details FROM events;" >>
                [&](int64_t ts, std::string type, std::string t_ip, std::string det) {
                    events.emplace_back(ts, std::move(type), std::move(t_ip), std::move(det));
                };
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            *db_ << "SELECT timestamp, name, value FROM metrics;" >>
                [&](int64_t ts, std::string t_name, double val) { metrics.emplace_back(ts, std::move(t_name), val); };
        }

        rapidjson::StringBuffer sb;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(sb);
        writer.StartObject();

        writer.Key("logs");
        writer.StartArray();
        for (auto& [ts, lvl, msg] : logs) {
            writer.StartObject();
            writer.Key("ts");
            writer.Int64(ts);
            writer.Key("level");
            writer.String(lvl.c_str(), static_cast<rapidjson::SizeType>(lvl.length()));
            writer.Key("msg");
            writer.String(msg.c_str(), static_cast<rapidjson::SizeType>(msg.length()));
            writer.EndObject();
        }
        writer.EndArray();

        writer.Key("events");
        writer.StartArray();
        for (auto& [ts, type, t_ip, det] : events) {
            writer.StartObject();
            writer.Key("ts");
            writer.Int64(ts);
            writer.Key("type");
            writer.String(type.c_str(), static_cast<rapidjson::SizeType>(type.length()));
            writer.Key("ip");
            writer.String(t_ip.c_str(), static_cast<rapidjson::SizeType>(t_ip.length()));
            writer.Key("details");
            writer.String(det.c_str(), static_cast<rapidjson::SizeType>(det.length()));
            writer.EndObject();
        }
        writer.EndArray();

        writer.Key("metrics");
        writer.StartArray();
        for (auto& [ts, t_name, val] : metrics) {
            writer.StartObject();
            writer.Key("ts");
            writer.Int64(ts);
            writer.Key("name");
            writer.String(t_name.c_str(), static_cast<rapidjson::SizeType>(t_name.length()));
            writer.Key("val");
            writer.Double(val);
            writer.EndObject();
        }
        writer.EndArray();

        writer.EndObject();
        return sb.GetString();
    } catch (const std::exception& e) {
        return fmt::format("exportHistoricalDataToJson failed: {}", e.what());
    }
}

sgrn::Result<void> GatewayDatabase::clearHistoricalData() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!db_)
        return "Database not initialized";
    try {
        *db_ << "DELETE FROM logs;";
        *db_ << "DELETE FROM events;";
        *db_ << "DELETE FROM metrics;";
        *db_ << "VACUUM;";
        return {};
    } catch (const std::exception& e) {
        return fmt::format("clearHistoricalData failed: {}", e.what());
    }
}

} // namespace sgrn::gateway::database
