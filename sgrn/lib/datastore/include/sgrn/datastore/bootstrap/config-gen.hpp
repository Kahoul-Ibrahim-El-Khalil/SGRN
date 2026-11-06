#pragma once
/**
 * @file  sgrn/datastore/bootstrap/config-gen.hpp
 * @brief Config-file and certificate generation helpers for the SGRN bootstrap.
 *
 * Covers:
 *   - Creating the on-disk directory tree (Postgres, garage, nginx buffers)
 *   - Writing a default .env template on first run
 *   - Generating a self-signed TLS certificate via openssl
 *   - Decompressing and writing embedded config assets (nginx, garage.toml, …)
 *   - Rendering systemd unit files from embedded templates
 *
 * All functions stay inside namespace sgrn::datastore::bootstrap and are
 * called by the generateConfigOnly / configureSystemd orchestrators in
 * bootstrap.hpp. Definitions live in src/bootstrap/config-gen.cpp.
 */

#include <filesystem>
#include <functional>
#include <string>

namespace sgrn::datastore::bootstrap
{

// ---------------------------------------------------------------------------
// Directory layout
// ---------------------------------------------------------------------------

void createDirectoryStructure(const std::string& t_base_dir);

// ---------------------------------------------------------------------------
// Default .env generation
// ---------------------------------------------------------------------------

void generateDefaultEnvFile(const std::filesystem::path& t_env_path, const std::string& t_user, const std::string& t_base_dir);

// ---------------------------------------------------------------------------
// TLS certificate generation
// ---------------------------------------------------------------------------

void generateSelfSignedCert(const std::filesystem::path& t_cert_dir);

// ---------------------------------------------------------------------------
// Embedded config asset extraction
// ---------------------------------------------------------------------------

void extractConfigAssets(const std::string& t_base_dir, const std::function<std::string(std::string)>& t_tmpl);

// ---------------------------------------------------------------------------
// Systemd unit file generation from embedded templates
// ---------------------------------------------------------------------------

void generateSystemdServices(const std::string& t_base_dir, const std::function<std::string(std::string)>& t_tmpl);

} // namespace sgrn::datastore::bootstrap
