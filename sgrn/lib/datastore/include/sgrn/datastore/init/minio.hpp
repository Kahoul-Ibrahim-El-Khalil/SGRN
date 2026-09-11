#pragma once
#include <drogon/drogon.h>
#include <sgrn/datastore/plugins/aws/S3Client.hpp>
#include <sgrn/debug.hpp>

namespace sgrn::datastore::plugins
{
inline void initMinio() {
    drogon::app().registerBeginningAdvice([]() {
        auto* p_s3 = drogon::app().getPlugin<sgrn::datastore::plugins::aws::S3Client>();
        if (!p_s3) {
            SGRN_ERROR("SGRN-Datastore", "S3Client plugin not available at startup");
            return;
        }

        const Json::Value& cfg = drogon::app().getCustomConfig();
        std::string bucket = cfg.get("s3", Json::Value{}).get("default_bucket", "sgrn-uploads").asString();

        // Run on the event loop as a one-shot coroutine
        drogon::async_run([p_s3, bucket]() -> drogon::Task<void> {
            auto exists_res = co_await p_s3->bucketExists(bucket);
            if (exists_res.hasError()) {
                SGRN_ERROR("SGRN-Datastore", "Could not check bucket '{}': {}", bucket, exists_res.error().message_);
                co_return;
            }
            if (!exists_res.value()) {
                SGRN_INFO("SGRN-Datastore", "Bucket '{}' not found, creating...", bucket);
                auto create_res = co_await p_s3->createBucket(bucket);
                if (create_res.hasError()) {
                    SGRN_ERROR("SGRN-Datastore", "Failed to create bucket '{}': {}", bucket, create_res.error().message_);
                } else {
                    SGRN_INFO("SGRN-Datastore", "Bucket '{}' created.", bucket);
                }
            } else {
                SGRN_INFO("SGRN-Datastore", "Bucket '{}' already exists.", bucket);
            }
        });
    });
}

} // namespace sgrn::datastore::plugins
