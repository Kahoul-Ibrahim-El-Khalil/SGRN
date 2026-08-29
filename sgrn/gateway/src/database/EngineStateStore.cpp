#include <sgrn/gateway/database/EngineStateStore.hpp>
#include <sgrn/utils/time.hpp>
#include <chrono>
#include <sqlite_modern_cpp.h>

namespace sgrn::gateway::database
{

EngineStateStore::EngineStateStore(std::filesystem::path t_db_path)
    : db_path_(std::move(t_db_path)) {
}

EngineStateStore::~EngineStateStore() = default;

sgrn::Result<void> EngineStateStore::open() {
    try {
        db_ = std::make_unique<sqlite::database>(db_path_.string());
        return {};
    } catch (const std::exception& e) {
        return fmt::format("Failed to open EngineStateStore {}: {}", db_path_.string(), e.what());
    }
}

sgrn::Result<void> EngineStateStore::ensureSchema() {
    try {
        if (!db_) {
            return "SQLite database is not open";
        }

        (*db_) << R"SQL(
            CREATE TABLE IF NOT EXISTS engine_registry (
                name TEXT PRIMARY KEY,
                registry_json TEXT NOT NULL,
                updated_at INTEGER NOT NULL
            )
        )SQL";

        (*db_) << R"SQL(
            CREATE TABLE IF NOT EXISTS engine_payloads (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                hash_hex TEXT NOT NULL UNIQUE,
                json_path TEXT NOT NULL,
                compressed_path TEXT NOT NULL,
                synced INTEGER NOT NULL DEFAULT 0,
                created_at INTEGER NOT NULL,
                synced_at INTEGER
            )
        )SQL";

        (*db_) << R"SQL(
            CREATE TABLE IF NOT EXISTS engine_deltas (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                plc_id TEXT NOT NULL,
                db_number INTEGER NOT NULL,
                field_path TEXT NOT NULL,
                value_json TEXT NOT NULL,
                timestamp_ms INTEGER NOT NULL,
                batch_id INTEGER,
                synced INTEGER NOT NULL DEFAULT 0,
                created_at INTEGER NOT NULL
            )
        )SQL";

        (*db_) << R"SQL(
            CREATE TABLE IF NOT EXISTS engine_snapshots (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                db_number INTEGER NOT NULL,
                json_path TEXT NOT NULL,
                synced INTEGER NOT NULL DEFAULT 0,
                created_at INTEGER NOT NULL,
                synced_at INTEGER
            )
        )SQL";

        return {};
    } catch (const std::exception& e) {
        return fmt::format("Failed to ensure EngineStateStore schema: {}", e.what());
    }
}

sgrn::Result<int64_t> EngineStateStore::recordBatch(
    const std::string& t_hash_hex, const std::string& t_json_path, const std::string& t_compressed_path) {
    try {
        if (!db_) {
            return "SQLite database is not open";
        }

        const int64_t created_at = sgrn::utils::time::nowMilliseconds();
        (*db_) << "INSERT INTO engine_payloads(hash_hex, json_path, compressed_path, synced, created_at) VALUES (?, ?, ?, 0, ?)"
               << t_hash_hex << t_json_path << t_compressed_path << created_at;
        return static_cast<int64_t>(db_->last_insert_rowid());
    } catch (const std::exception& e) {
        return fmt::format("Failed to record batch: {}", e.what());
    }
}

sgrn::Result<void> EngineStateStore::markSynced(int64_t t_row_id) {
    try {
        if (!db_) {
            return "SQLite database is not open";
        }
        (*db_) << "UPDATE engine_payloads SET synced = 1, synced_at = ? WHERE id = ?" << sgrn::utils::time::nowMilliseconds() << t_row_id;
        return {};
    } catch (const std::exception& e) {
        return fmt::format("Failed to mark batch synced: {}", e.what());
    }
}

sgrn::Result<std::vector<std::pair<int64_t, std::string>>> EngineStateStore::listPendingPayloads() const {
    try {
        if (!db_) {
            return "SQLite database is not open";
        }
        std::vector<std::pair<int64_t, std::string>> rows;
        for (auto row : *db_ << "SELECT id, compressed_path FROM engine_payloads WHERE synced = 0 ORDER BY id") {
            int64_t id = 0;
            std::string t_compressed_path;
            row >> id >> t_compressed_path;
            rows.emplace_back(id, std::move(t_compressed_path));
        }
        return rows;
    } catch (const std::exception& e) {
        return fmt::format("Failed to list pending payloads: {}", e.what());
    }
}

sgrn::Result<int64_t> EngineStateStore::recordSnapshot(const std::string& t_json_path, std::optional<uint16_t> t_db_number) {
    try {
        if (!db_)
            return "SQLite database is not open";
        const int64_t created_at = sgrn::utils::time::nowMilliseconds();
        int32_t db_val = t_db_number ? static_cast<int32_t>(*t_db_number) : -1;
        (*db_) << "INSERT INTO engine_snapshots(db_number, json_path, synced, created_at) VALUES (?, ?, 0, ?)" << db_val << t_json_path
               << created_at;
        return static_cast<int64_t>(db_->last_insert_rowid());
    } catch (const std::exception& e) {
        return fmt::format("Failed to record snapshot: {}", e.what());
    }
}

sgrn::Result<void> EngineStateStore::markSnapshotSynced(int64_t t_row_id) {
    try {
        if (!db_)
            return "SQLite database is not open";
        (*db_) << "UPDATE engine_snapshots SET synced = 1, synced_at = ? WHERE id = ?" << sgrn::utils::time::nowMilliseconds() << t_row_id;
        return {};
    } catch (const std::exception& e) {
        return fmt::format("Failed to mark snapshot synced: {}", e.what());
    }
}

sgrn::Result<std::vector<std::pair<int64_t, std::string>>> EngineStateStore::listPendingSnapshots() const {
    try {
        if (!db_)
            return "SQLite database is not open";
        std::vector<std::pair<int64_t, std::string>> rows;
        for (auto row : *db_ << "SELECT id, json_path FROM engine_snapshots WHERE synced = 0 ORDER BY id") {
            int64_t id = 0;
            std::string path;
            row >> id >> path;
            rows.emplace_back(id, std::move(path));
        }
        return rows;
    } catch (const std::exception& e) {
        return fmt::format("Failed to list pending snapshots: {}", e.what());
    }
}

sgrn::Result<void> EngineStateStore::upsertRegistry(const std::string& t_registry_json) {
    try {
        if (!db_) {
            return "SQLite database is not open";
        }
        (*db_) << "INSERT INTO engine_registry(name, registry_json, updated_at) VALUES (?, ?, ?) "
                  "ON CONFLICT(name) DO UPDATE SET registry_json = excluded.registry_json, updated_at = excluded.updated_at"
               << "current" << t_registry_json << sgrn::utils::time::nowMilliseconds();
        return {};
    } catch (const std::exception& e) {
        return fmt::format("Failed to upsert registry: {}", e.what());
    }
}

sgrn::Result<::sgrn::scl::PlcSchemaStore> EngineStateStore::loadRegistryFromSQLite(const std::string& t_db_path) {
    try {
        sqlite::database db_(t_db_path);
        std::string t_registry_json;
        db_ << "SELECT registry_json FROM engine_registry WHERE name = 'current' LIMIT 1" >> t_registry_json;

        if (t_registry_json.empty()) {
            return "SQLite registry table is empty or missing 'current' entry";
        }

        rapidjson::Document root;
        root.Parse(t_registry_json.c_str());
        if (root.HasParseError()) {
            return "Failed to parse JSON from SQLite";
        }

        auto res = ::sgrn::scl::PlcSchemaStore::loadFromJson(root);
        if (res.hasError()) {
            return fmt::format("Failed to parse registry: {}", toString(res.error()));
        }
        return res.value();
    } catch (const std::exception& e) {
        return fmt::format("SQLite error loading registry: {}", e.what());
    }
}

} // namespace sgrn::gateway::database
