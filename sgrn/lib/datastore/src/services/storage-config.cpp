#include <sgrn/datastore/services/storage.hpp>

#include <drogon/HttpAppFramework.h>
#include <sgrn/debug.hpp>
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
StorageConfig StorageConfig::fromS3Json(const Json::Value& t_s3) {
    StorageConfig cfg;
    if (!t_s3.isObject())
        return cfg;

    if (t_s3.isMember("default_bucket") && t_s3["default_bucket"].isString())
        cfg.default_bucket = t_s3["default_bucket"].asString();
    if (t_s3.isMember("threshold_compress_ram_mb"))
        cfg.threshold_compress_ram_mb = t_s3["threshold_compress_ram_mb"].asUInt64();
    if (t_s3.isMember("max_file_size_mb"))
        cfg.max_file_size_mb = t_s3["max_file_size_mb"].asUInt64();
    if (t_s3.isMember("compression_level"))
        cfg.compression_level = static_cast<uint8_t>(t_s3["compression_level"].asUInt());
    if (t_s3.isMember("compression_size_threshold_kb"))
        cfg.compression_size_threshold_kb = t_s3["compression_size_threshold_kb"].asUInt64();
    if (t_s3.isMember("chunking_threshold_mb"))
        cfg.chunking_threshold_mb = t_s3["chunking_threshold_mb"].asUInt64();
    if (t_s3.isMember("chunk_part_size_mb"))
        cfg.chunk_part_size_mb = t_s3["chunk_part_size_mb"].asUInt64();

    if (cfg.chunk_part_size_mb < defaults::kMinPartSizeMb) {
        WARN_LOG(
            "s3.chunk_part_size_mb ({} MB) is below the {} MB S3 minimum — clamping.", cfg.chunk_part_size_mb, defaults::kMinPartSizeMb);
        cfg.chunk_part_size_mb = defaults::kMinPartSizeMb;
    }
    if (t_s3.isMember("allowed_extensions") && t_s3["allowed_extensions"].isArray()) {
        cfg.allowed_extensions.clear();
        for (const auto& ext : t_s3["allowed_extensions"])
            cfg.allowed_extensions.insert(ext.asString());
    }
    if (t_s3.isMember("prohibited_extensions") && t_s3["prohibited_extensions"].isArray()) {
        cfg.prohibited_extensions.clear();
        for (const auto& ext : t_s3["prohibited_extensions"])
            cfg.prohibited_extensions.insert(ext.asString());
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

StorageConfig StorageConfig::loadFromConfig() {
    try {
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
