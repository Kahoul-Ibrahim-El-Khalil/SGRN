#pragma once

#include <cstdint>
#include <string>

namespace sgrn::gateway::config
{

struct DatastoreConnectionConfig {
    std::string url;
    std::string public_token;
    std::string private_token;
    std::string object_name{"gateway/default"};
    uint32_t sync_interval_s{30};
    bool telemetry_enabled{true};
    uint32_t batch_size{1000};
    uint32_t batch_interval_s{300};
    std::string upload_mode{"telemetry"};
    std::string snapshot_mode{"Anchored"};
    std::string vfs_remote_dir{"/gateway/snapshots"};
    uint8_t zstd_level{5};
    uint8_t aggressive_zstd_level{12}; // For Full Tree Anchors
    bool enable_aggressive_compression{true};
    bool offline_persistence{false};

    bool isConfigured() const {
        return !url.empty() && !public_token.empty() && !private_token.empty();
    }

    std::string getEffectiveVfsRemoteDir() const {
        return vfs_remote_dir;
    }
};

} // namespace sgrn::gateway::config
