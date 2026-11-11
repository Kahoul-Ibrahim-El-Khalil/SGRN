#include <sgrn/datastore/services/storage.hpp>

#include <drogon/HttpAppFramework.h>
#include <sgrn/datastore/config/config.hpp>
#include <sgrn/debug.hpp>
#include <fstream>
#include <mutex>

#ifdef DEBUG_STORAGE_SERVICE
#define WARN_LOG(msg, ...) SGRN_WARN("StorageService", msg __VA_OPT__(, ) __VA_ARGS__)
#define INFO_LOG(msg, ...) SGRN_INFO("StorageService", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define WARN_LOG(...) ((void)0)
#define INFO_LOG(...) ((void)0)
#endif

namespace sgrn::datastore::services::storage
{
namespace config_detail
{
struct CustomConfigWire {
    StorageConfig::StorageConfigJson s3;
};

struct ConfigFileWire {
    CustomConfigWire custom_config;
};

StorageConfig normalize(StorageConfig cfg) {
    if (cfg.chunk_part_size_mb < defaults::kMinPartSizeMb) {
        WARN_LOG(
            "s3.chunk_part_size_mb ({} MB) is below the {} MB S3 minimum — clamping.", cfg.chunk_part_size_mb, defaults::kMinPartSizeMb);
        cfg.chunk_part_size_mb = defaults::kMinPartSizeMb;
    }
    if (cfg.chunking_threshold_mb >= cfg.max_file_size_mb) {
        WARN_LOG("s3.chunking_threshold_mb ({} MB) is at/above max_file_size_mb ({} MB) — multipart is unreachable.",
            cfg.chunking_threshold_mb, cfg.max_file_size_mb);
    }
    INFO_LOG("Loaded storage config: bucket={}, compress_ram={}MB, max_size={}MB, compression_level={}, allowed_exts={}, "
             "chunking_threshold={}MB, chunk_part_size={}MB",
        cfg.default_bucket, cfg.threshold_compress_ram_mb, cfg.max_file_size_mb, static_cast<int>(cfg.compression_level),
        cfg.allowed_extensions.size(), cfg.chunking_threshold_mb, cfg.chunk_part_size_mb);
    return cfg;
}
} // namespace config_detail

StorageConfig StorageConfig::fromS3Json(const Json::Value& t_s3) {
    StorageConfig cfg;
    if (!t_s3.isObject())
        return cfg;

    const auto parsed = sgrn::json::read<StorageConfig::StorageConfigJson>(t_s3.toStyledString());
    if (parsed.hasError()) {
        WARN_LOG("Invalid storage config shape; using defaults: {}", parsed.error().message);
        return cfg;
    }
    return config_detail::normalize(StorageConfig::fromJsonConfig(parsed.value()));
}

StorageConfig StorageConfig::loadFromConfig() {
    try {
        const std::string config_path = sgrn::datastore::config::currentConfigPath();
        std::ifstream config_file(config_path, std::ios::binary);
        if (config_file) {
            const std::string config_text((std::istreambuf_iterator<char>(config_file)), std::istreambuf_iterator<char>());
            const auto parsed = sgrn::json::read<config_detail::ConfigFileWire>(config_text);
            if (!parsed.hasError()) {
                return config_detail::normalize(StorageConfig::fromJsonConfig(parsed.value().custom_config.s3));
            }
            WARN_LOG("Failed to parse typed storage configuration from {}: {}", config_path, parsed.error().message);
        }

        // Fallback for embedders that configure Drogon programmatically or
        // whose config path is unavailable; this remains the dynamic boundary.
        const Json::Value& custom_config = drogon::app().getCustomConfig();
        if (custom_config.isMember("s3"))
            return fromS3Json(custom_config["s3"]);
    } catch (const std::exception& ex) {
        WARN_LOG("Failed to load config, using defaults: {}", ex.what());
    }
    return StorageConfig{};
}

namespace
{
std::mutex g_storage_config_mutex;
StorageConfig g_storage_config;
} // namespace

void publishStorageConfig(StorageConfig t_cfg) {
    std::lock_guard lock(g_storage_config_mutex);
    g_storage_config = std::move(t_cfg);
}

StorageConfig currentStorageConfig() {
    std::lock_guard lock(g_storage_config_mutex);
    return g_storage_config;
}
} // namespace sgrn::datastore::services::storage

#undef WARN_LOG
#undef INFO_LOG
