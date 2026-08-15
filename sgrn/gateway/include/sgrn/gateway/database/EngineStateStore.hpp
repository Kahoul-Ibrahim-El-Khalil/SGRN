#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/scl/types.hpp>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sqlite
{
class database;
}

namespace sgrn::gateway::database
{

/**
 * @brief SQLite-based state store for tracking S7 PLC snapshots and telemetry batches.
 *
 * This store handles the low-level persistence of telemetry payloads and snapshots
 * before they are synced to the backend.
 */
class EngineStateStore {
public:
    explicit EngineStateStore(std::filesystem::path t_db_path);
    ~EngineStateStore();

    sgrn::Result<void> open();
    sgrn::Result<void> ensureSchema();

    // Telemetry Batches
    sgrn::Result<int64_t> recordBatch(const std::string& t_hash_hex, const std::string& t_json_path, const std::string& t_compressed_path);
    sgrn::Result<void> markSynced(int64_t t_row_id);
    sgrn::Result<std::vector<std::pair<int64_t, std::string>>> listPendingPayloads() const;

    // Snapshots tracking
    sgrn::Result<int64_t> recordSnapshot(const std::string& t_json_path, std::optional<uint16_t> t_db_number = std::nullopt);
    sgrn::Result<void> markSnapshotSynced(int64_t t_row_id);
    sgrn::Result<std::vector<std::pair<int64_t, std::string>>> listPendingSnapshots() const;

    // Registry persistence
    sgrn::Result<void> upsertRegistry(const std::string& t_registry_json);
    static sgrn::Result<::sgrn::scl::PlcSchemaStore> loadRegistryFromSQLite(const std::string& t_db_path);

private:
    std::filesystem::path db_path_;
    std::shared_ptr<sqlite::database> db_;
};

} // namespace sgrn::gateway::database
