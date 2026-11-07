#include "sgrn/datastore/crypto/key-store.hpp"
#include <fmt/format.h>
#include <sgrn/debug.hpp>
#include <sgrn/utils/env.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <filesystem>
#include <memory>
#include <mutex>

namespace fs = std::filesystem;

namespace sgrn::datastore::crypto
{

namespace
{
std::mutex g_key_store_mutex;
std::unique_ptr<SqliteKeyProvider> g_key_store_instance;
} // namespace

std::string getDefaultKeyDbPath(const std::string& t_base_dir) {
    if (!t_base_dir.empty()) {
        return (fs::path(t_base_dir) / "keys" / "keys.db").string();
    }
    const std::string env_data = sgrn::utils::env::get("SGRN_DATA_DIR", "");
    if (!env_data.empty()) {
        return (fs::path(env_data) / "keys" / "keys.db").string();
    }
    // Fallback to default user path
    return (fs::path(sgrn::utils::filesystem::expandUserPath("~/.sgrn")) / "keys" / "keys.db").string();
}

::sgrn::Result<void> initKeyStore(const KeyStoreConfig& t_config) {
    std::lock_guard lock(g_key_store_mutex);

    if (g_key_store_instance) {
        return {}; // already initialized
    }

    std::string effective_path = t_config.db_path.empty() ? getDefaultKeyDbPath() : t_config.db_path;

    // Check isolation
    std::string eff_garage = t_config.garage_data_dir;
    if (eff_garage.empty()) {
        eff_garage = sgrn::utils::env::get("GARAGE_DATA_DIR", "");
    }
    std::string eff_pg = t_config.postgres_data_dir;
    if (eff_pg.empty()) {
        eff_pg = sgrn::utils::env::get("PGDATA", "");
    }

    auto iso_res = verifyKeyDbIsolation(effective_path, eff_garage, eff_pg);
    if (iso_res.hasError()) {
        SGRN_ERROR("KeyStore", "Key DB isolation check failed: {}", iso_res.error());
        return ::sgrn::Error(iso_res.error());
    }

    auto provider_res = SqliteKeyProvider::open(effective_path, t_config.chunk_size);
    if (provider_res.hasError()) {
        SGRN_ERROR("KeyStore", "Failed to open key store at {}: {}", effective_path, provider_res.error());
        return ::sgrn::Error(provider_res.error());
    }

    g_key_store_instance = std::move(provider_res.value());
    SGRN_INFO("KeyStore", "Initialized SQLite key store at {}", effective_path);
    return {};
}

::sgrn::Result<void> initKeyStore(
    const std::string& t_db_path, const std::string& t_garage_data_dir, const std::string& t_postgres_data_dir, uint32_t t_chunk_size) {
    return initKeyStore(KeyStoreConfig{
        .db_path = t_db_path,
        .garage_data_dir = t_garage_data_dir,
        .postgres_data_dir = t_postgres_data_dir,
        .chunk_size = t_chunk_size,
    });
}

SqliteKeyProvider* getKeyStore() {
    std::lock_guard lock(g_key_store_mutex);
    return g_key_store_instance.get();
}

bool isKeyStoreInitialized() {
    std::lock_guard lock(g_key_store_mutex);
    return g_key_store_instance != nullptr;
}

void shutdownKeyStore() {
    std::lock_guard lock(g_key_store_mutex);
    g_key_store_instance.reset();
}

} // namespace sgrn::datastore::crypto
