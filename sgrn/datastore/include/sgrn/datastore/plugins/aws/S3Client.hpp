// src/sgrn/plugins/aws/S3Client.hpp
#pragma once

#include <drogon/HttpAppFramework.h>
#include <drogon/plugins/Plugin.h>
#include <drogon/utils/coroutine.h>
#include <sgrn/datastore/BackendError.hpp>
#include <sgrn/datastore/plugins/aws/S3Error.hpp>
#include <sgrn/datastore/plugins/threadpool/Threadpool.hpp>
#include <aws/core/Aws.h>
#include <aws/core/http/HttpTypes.h>
#include <aws/s3/S3Client.h>
#include <filesystem>
#include <json/json.h>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

namespace sgrn::datastore::plugins::aws
{

using ::sgrn::Result;

/**
 * Drogon Plugin: S3Client
 *
 * Provides an async coroutine-friendly wrapper around AWS S3 / MinIO.
 *
 * Thread-safety contract:
 *   - All public methods are coroutine-safe: they capture arguments by value,
 *     dispatch blocking SDK calls to a dedicated EventLoopThreadPool, and
 *     resume the coroutine on the calling event loop when done.
 *   - client_mutex_ serialises access to the underlying Aws::S3::S3Client.
 *   - No method ever blocks a Drogon event-loop thread.
 *
 * JSON config keys (all optional):
 *   "worker_threads"  : int  — thread pool size (default 4)
 *   "region"          : str
 *   "endpoint"        : str  — MinIO / custom S3 endpoint
 *   "public_endpoint" : str
 *   "access_key"      : str
 *   "secret_key"      : str
 */
class S3Client : public drogon::Plugin<S3Client> {
public:
    S3Client() = default;
    S3Client(const S3Client&) = delete;
    S3Client(S3Client&&) = delete;
    S3Client& operator=(const S3Client&) = delete;
    S3Client& operator=(S3Client&&) = delete;
    ~S3Client() override = default;

    // ========================================================================
    // Plugin Lifecycle
    // ========================================================================
    void initAndStart(const Json::Value& t_config) override;
    void shutdown() override;

    // ========================================================================
    // Bucket Operations
    // ========================================================================
    drogon::Task<BackendResult<Json::Value>> listBuckets();
    drogon::Task<BackendResult<void>> createBucket(std::string t_bucket);
    drogon::Task<BackendResult<void>> deleteBucket(std::string t_bucket);
    drogon::Task<BackendResult<bool>> bucketExists(std::string t_bucket);

    // ========================================================================
    // Object Operations
    // ========================================================================
    drogon::Task<BackendResult<void>> uploadFile(std::string t_bucket, std::string t_key, std::string t_file_path);

    drogon::Task<BackendResult<void>> uploadFromMemory(
        std::string t_bucket, std::string t_key, std::string t_data, std::string t_content_type);

    drogon::Task<BackendResult<void>> uploadWithMetadata(
        std::string t_bucket, std::string t_key, std::string t_file_path, Json::Value t_metadata);

    drogon::Task<BackendResult<void>> downloadFile(std::string t_bucket, std::string t_key, std::string t_download_path);

    drogon::Task<BackendResult<void>> downloadFileRange(
        std::string t_bucket, std::string t_key, std::string t_download_path, int64_t t_start, int64_t t_end);

    drogon::Task<BackendResult<std::string>> getObjectContent(std::string t_bucket, std::string t_key);

    drogon::Task<BackendResult<void>> deleteFile(std::string t_bucket, std::string t_key);

    drogon::Task<BackendResult<Json::Value>> deleteFiles(std::string t_bucket, std::vector<std::string> t_keys);

    drogon::Task<BackendResult<void>> copyFile(
        std::string t_src_bucket, std::string t_src_key, std::string t_dest_bucket, std::string t_dest_key);

    drogon::Task<BackendResult<bool>> exists(std::string t_bucket, std::string t_key);

    drogon::Task<BackendResult<Json::Value>> statObject(std::string t_bucket, std::string t_key);

    drogon::Task<BackendResult<Json::Value>> listObjects(
        std::string t_bucket, std::string t_prefix = "", std::string t_continuation_token = "", uint32_t t_max_keys = 1000);

    // ========================================================================
    // Multipart Upload Operations
    // ========================================================================
    drogon::Task<BackendResult<std::string>> createMultipartUpload(std::string t_bucket, std::string t_key, std::string t_content_type);

    drogon::Task<BackendResult<std::string>> uploadPart(
        std::string t_bucket, std::string t_key, std::string t_upload_id, int t_part_number, std::string t_body);

    drogon::Task<BackendResult<void>> completeMultipartUpload(
        std::string t_bucket, std::string t_key, std::string t_upload_id, Json::Value t_parts);

    drogon::Task<BackendResult<void>> abortMultipartUpload(std::string t_bucket, std::string t_key, std::string t_upload_id);

    drogon::Task<BackendResult<Json::Value>> listParts(
        std::string t_bucket, std::string t_key, std::string t_upload_id, uint32_t t_max_parts = 1000);

    // ========================================================================
    // Tagging & Metadata Operations
    // ========================================================================
    drogon::Task<BackendResult<void>> putObjectTags(std::string t_bucket, std::string t_key, Json::Value t_tags);

    drogon::Task<BackendResult<Json::Value>> getObjectTags(std::string t_bucket, std::string t_key);

    drogon::Task<BackendResult<void>> deleteObjectTags(std::string t_bucket, std::string t_key);

    // ========================================================================
    // Presigned URL Operations
    // ========================================================================
    drogon::Task<BackendResult<std::string>> presignedGetUrl(std::string t_bucket, std::string t_key, uint32_t t_expiry_seconds);

    drogon::Task<BackendResult<std::string>> presignedPutUrl(std::string t_bucket, std::string t_key, uint32_t t_expiry_seconds);

    drogon::Task<BackendResult<std::string>> presignedDeleteUrl(std::string t_bucket, std::string t_key, uint32_t t_expiry_seconds);

    /// Returns the raw pool from the shared Threadpool plugin.
    /// Callers in service.cpp use this via sgrn::utils::runInPool().
    static sgrn::utils::DynamicThreadPool& getWorkerPool() {
        return *threadpool()->getPool();
    }

private:
    // ========================================================================
    // Members
    // ========================================================================
    std::mutex client_mutex_;
    Aws::SDKOptions sdk_options_;
    std::shared_ptr<Aws::S3::S3Client> client_;
    std::shared_ptr<Aws::S3::S3Client> public_client_;
    std::string endpoint_;
    std::string public_endpoint_;
    std::string region_;

    // ── Pool access ───────────────────────────────────────────────────────────
    // The thread pool is owned by the Threadpool plugin; S3Client just borrows
    // a pointer to it.  The plugin is guaranteed to outlive S3Client because
    // Drogon shuts down plugins in reverse registration order.
    static sgrn::datastore::plugins::Threadpool* threadpool() {
        return drogon::app().getPlugin<sgrn::datastore::plugins::Threadpool>();
    }

    drogon::Task<BackendResult<std::string>> generatePresignedUrl(
        std::string t_bucket, std::string t_key, uint32_t t_expiry_seconds, Aws::Http::HttpMethod t_method);

    template <typename ReturnType, typename CallableType>
    drogon::Task<BackendResult<ReturnType>> execute(CallableType&& t_fun) {
        std::shared_ptr<Aws::S3::S3Client> sp_client_copy;
        {
            std::lock_guard lock(client_mutex_);
            sp_client_copy = client_;
        }

        if (sp_client_copy == nullptr) {
            co_return ::sgrn::datastore::plugins::aws::toBackendError(
                ::sgrn::datastore::plugins::aws::S3Error::NotInitialized, "S3 client not initialized");
        }

        auto* p_tp = threadpool();
        if (p_tp == nullptr) {
            co_return BackendError{"Runtime", "Threadpool plugin not initialized"};
        }

        co_return co_await p_tp->run(
            [client_ = std::move(sp_client_copy), t_fun = std::forward<CallableType>(t_fun)]() mutable -> BackendResult<ReturnType> {
                try {
                    if constexpr (std::is_void_v<ReturnType>) {
                        auto res = t_fun(*client_);
                        if (res.hasError())
                            return res.error();
                        return {};
                    } else {
                        return t_fun(*client_);
                    }
                } catch (const std::bad_alloc&) {
                    return ::sgrn::datastore::plugins::aws::toBackendError(
                        ::sgrn::datastore::plugins::aws::S3Error::Unknown, "Out of memory in S3 worker");
                } catch (const std::filesystem::filesystem_error& e) {
                    return ::sgrn::datastore::plugins::aws::toBackendError(::sgrn::datastore::plugins::aws::S3Error::FilesystemError,
                        std::string("Filesystem error in S3 worker: ") + e.what());
                } catch (const std::exception& e) {
                    return ::sgrn::datastore::plugins::aws::toBackendError(
                        ::sgrn::datastore::plugins::aws::S3Error::Unknown, std::string("Exception in S3 worker: ") + e.what());
                } catch (...) {
                    return ::sgrn::datastore::plugins::aws::toBackendError(
                        ::sgrn::datastore::plugins::aws::S3Error::Unknown, "Unknown exception in S3 worker");
                }
            });
    }
};

} // namespace sgrn::datastore::plugins::aws
