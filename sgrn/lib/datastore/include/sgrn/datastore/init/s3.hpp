#pragma once
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <sgrn/datastore/plugins/aws/S3Client.hpp>
#include <sgrn/debug.hpp>

#include <chrono>

namespace sgrn::datastore::plugins
{
inline void initS3() {
    drogon::app().registerBeginningAdvice([]() {
        auto* p_s3 = drogon::app().getPlugin<sgrn::datastore::plugins::aws::S3Client>();
        if (!p_s3) {
            SGRN_ERROR("SGRN-Datastore", "S3Client plugin not available at startup");
            return;
        }

        const Json::Value& cfg = drogon::app().getCustomConfig();
        std::string bucket = cfg.get("s3", Json::Value{}).get("default_bucket", "sgrn-uploads").asString();

        // Run on the event loop as a one-shot coroutine. Garage may still be
        // coming up (or restarting mid-life), so retry the check with backoff
        // instead of single-shotting it. The unit-level readiness gate
        // (ExecStartPre on SGRN-datastore.service) covers the boot race; this
        // covers everything after it.
        drogon::async_run([p_s3, bucket]() -> drogon::Task<void> {
            constexpr int kAttempts = 6; // worst case ~31s of backoff (1+2+4+8+16)
            for (int attempt = 1;; ++attempt) {
                auto exists_res = co_await p_s3->bucketExists(bucket);
                if (!exists_res.hasError()) {
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
                    co_return;
                }
                if (attempt >= kAttempts) {
                    SGRN_ERROR("SGRN-Datastore", "Could not check bucket '{}' after {} attempts: {}", bucket, kAttempts,
                        exists_res.error().message_);
                    co_return;
                }
                SGRN_WARN("SGRN-Datastore", "Could not check bucket '{}' (attempt {}/{}): {} — retrying", bucket, attempt, kAttempts,
                    exists_res.error().message_);
                co_await drogon::sleepCoro(drogon::app().getLoop(), std::chrono::seconds(1 << (attempt - 1)));
            }
        });
    });
}

} // namespace sgrn::datastore::plugins
