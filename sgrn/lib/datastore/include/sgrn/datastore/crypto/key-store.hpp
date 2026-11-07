#pragma once

#include <sgrn/Result.hpp>
#include <sgrn/datastore/crypto/sqlite-key-provider.hpp>
#include <cstdint>
#include <memory>
#include <string>

namespace sgrn::datastore::crypto
{

struct KeyStoreConfig {
    std::string db_path;
    std::string garage_data_dir;
    std::string postgres_data_dir;
    uint32_t chunk_size{1024 * 1024};
};

/**
 * @brief Initialize the global key store singleton using a config struct.
 *
 * Checks file permissions (0600 on db, 0700 on dir) and path isolation
 * (not inside Garage or Postgres data directories).
 */
::sgrn::Result<void> initKeyStore(const KeyStoreConfig& t_config);

/**
 * @brief Initialize the global key store singleton with discrete arguments.
 */
::sgrn::Result<void> initKeyStore(const std::string& t_db_path = "", const std::string& t_garage_data_dir = "",
    const std::string& t_postgres_data_dir = "", uint32_t t_chunk_size = 1024 * 1024);

/**
 * @brief Get the global SqliteKeyProvider singleton.
 * Returns nullptr if the key store has not been initialized.
 */
SqliteKeyProvider* getKeyStore();

/**
 * @brief Check whether the global key store has been initialized.
 */
bool isKeyStoreInitialized();

/**
 * @brief Reset / close the global key store.
 */
void shutdownKeyStore();

/**
 * @brief Resolve default keys.db path given an optional base_dir (e.g. SGRN_DATA_DIR).
 */
std::string getDefaultKeyDbPath(const std::string& t_base_dir = "");

} // namespace sgrn::datastore::crypto
