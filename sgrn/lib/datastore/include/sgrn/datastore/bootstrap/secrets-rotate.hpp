#pragma once
// sgrn/lib/datastore/include/sgrn/datastore/bootstrap/secrets-rotate.hpp
//
// `--rotate KEY[=VALUE] [--rotate ...]` — safe secret rotation.
//
// Rotation is NOT a manual ".env edit + restart" ritual:
//   1. values resolve (operator entry, or CSPRNG generation on Enter / in CI)
//   2. POSTGRES_PASSWORD is applied to the ROLE FIRST via ALTER ROLE —
//      a datastore restart against an unchanged role would lock the role
//      out; the destructive --init-db path is never used for rotation
//   3. the old .env is archived under var/backup/env/ with a UTC audit line
//      (key names only — rotated values are never logged)
//   4. only then the .env is upserted and configs re-rendered
//      (generateConfigOnly: sgrn.json / garage.toml / staged units re-render
//      with the new values — the runtime does not expand ${VAR} at runtime,
//      so re-rendering is mandatory after every rotated env value)
//   5. restart / cluster-side instructions are printed (root-owned systemd
//      units are never touched from the unprivileged rotate path)
//
// GARAGE_RPC_SECRET is refuse-by-design: it is cluster/identity material;
// rotating it means re-provisioning the node (fresh install), not editing a
// credential. GARAGE_ACCESS_KEY/SECRET rotation prints the cluster-side
// `garage key` steps instead of running them — garage may be on another host.
//
// generateConfigOnly is declared in bootstrap.hpp AFTER this include; the
// forward declaration below keeps this header compilable standalone.
// Implementation lives in src/bootstrap/secrets-rotate.cpp.

#include <string>
#include <utility>
#include <vector>

namespace sgrn::datastore::bootstrap
{
bool generateConfigOnly(const std::string& t_base_dir);
}

namespace sgrn::datastore::bootstrap::rotate
{

/// Rotate secrets end-to-end. Returns 0 on success, 1 on refusal/abort.
int runRotation(const std::string& t_base_dir, const std::vector<std::pair<std::string, std::string>>& t_requests, bool t_non_interactive);

} // namespace sgrn::datastore::bootstrap::rotate
