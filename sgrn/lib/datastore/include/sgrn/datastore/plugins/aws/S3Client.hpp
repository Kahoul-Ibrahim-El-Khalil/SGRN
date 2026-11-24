// src/sgrn/plugins/aws/S3Client.hpp
#pragma once

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpTypes.h>
#include <drogon/plugins/Plugin.h>
#include <drogon/utils/coroutine.h>
#include <sgrn/datastore/BackendError.hpp>
#include <sgrn/datastore/plugins/aws/NativeS3Client.hpp>
#include <sgrn/datastore/plugins/aws/S3Error.hpp>
#include <sgrn/datastore/plugins/aws/S3Types.hpp>
#include <sgrn/datastore/plugins/threadpool/Threadpool.hpp>
#include <sgrn/datastore/utils/concurrency.hpp>
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

/**
 * Drogon Plugin: S3Client
 *
 * Provides an async coroutine-friendly wrapper around AWS S3 / Garage.
 *
 * Thread-safety & concurrency model:
 *   - All public methods are coroutine-safe: they capture arguments by value,
 *     dispatch blocking S3 calls and file I/O to S3Client's dedicated worker
 *     pool (`s3_pool_`), and resume the coroutine on the calling event loop.
 *   - `s3_pool_` is intentionally separate from the CPU `Threadpool` plugin
 *     (`getWorkerPool()`) so slow S3 transfers never starve file hashing or
 *     compression, and vice versa.
 *   - `client_` owns `io_threads` parallel EventLoopThread + HttpClient
 *     keep-alive connections, dispatched round-robin.
 *   - `client_mutex_` guards shared_ptr copies of `client_` / `public_client_`
 *     and `s3_pool_` lifetime against concurrent `shutdown()`.
 *
 * JSON config keys:
 *   "region"          : str  (optional, default "us-east-1")
 *   "endpoint"        : str  (required)
 *   "public_endpoint" : str  (optional, defaults to "endpoint")
 *   "access_key"      : str  (required)
 *   "secret_key"      : str  (required)
 *   "worker_threads"  : int  (optional, default 4, range 1..64 — S3 I/O workers)
 *   "io_threads"      : int  (optional, defaults to worker_threads, range 1..32 — HTTP keep-alive loops)
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
    drogon::Task<Result<S3ListBucketsResult, BackendError>> listBuckets();
    drogon::Task<Result<void, BackendError>> createBucket(std::string t_bucket);
    drogon::Task<Result<void, BackendError>> deleteBucket(std::string t_bucket);
    drogon::Task<Result<bool, BackendError>> bucketExists(std::string t_bucket);

    // ========================================================================
    // Object Operations
    // ========================================================================
    drogon::Task<Result<void, BackendError>> uploadFile(std::string t_bucket, std::string t_key, std::string t_file_path);

    drogon::Task<Result<void, BackendError>> uploadFromMemory(
        std::string t_bucket, std::string t_key, std::string t_data, std::string t_content_type);

    drogon::Task<Result<void, BackendError>> uploadWithMetadata(
        std::string t_bucket, std::string t_key, std::string t_file_path, S3Metadata t_metadata);

    drogon::Task<Result<std::string, BackendError>> getObjectContent(std::string t_bucket, std::string t_key, int64_t t_max_bytes = 0);

    /// Zero-copy object fetch: returns S3Response keeping the underlying Drogon HttpResponsePtr
    /// alive so callers can stream/write from `resp.body` (std::string_view) without an extra copy.
    drogon::Task<Result<S3Response, BackendError>> getObjectResponse(std::string t_bucket, std::string t_key, int64_t t_max_bytes = 0);

    drogon::Task<Result<void, BackendError>> deleteFile(std::string t_bucket, std::string t_key);

    drogon::Task<Result<S3DeleteResult, BackendError>> deleteFiles(std::string t_bucket, std::vector<std::string> t_keys);

    drogon::Task<Result<void, BackendError>> copyFile(
        std::string t_src_bucket, std::string t_src_key, std::string t_dest_bucket, std::string t_dest_key);

    drogon::Task<Result<bool, BackendError>> exists(std::string t_bucket, std::string t_key);

    drogon::Task<Result<S3ObjectStat, BackendError>> statObject(std::string t_bucket, std::string t_key);

    drogon::Task<Result<S3ListObjectsResult, BackendError>> listObjects(
        std::string t_bucket, std::string t_prefix = "", std::string t_continuation_token = "", uint32_t t_max_keys = 1000);

    // ========================================================================
    // Multipart Upload Operations
    // ========================================================================
    drogon::Task<Result<std::string, BackendError>> createMultipartUpload(
        std::string t_bucket, std::string t_key, std::string t_content_type);

    drogon::Task<Result<std::string, BackendError>> uploadPart(
        std::string t_bucket, std::string t_key, std::string t_upload_id, int t_part_number, std::string t_body);

    drogon::Task<Result<void, BackendError>> completeMultipartUpload(
        std::string t_bucket, std::string t_key, std::string t_upload_id, std::vector<S3CompletedPart> t_parts);

    drogon::Task<Result<void, BackendError>> abortMultipartUpload(std::string t_bucket, std::string t_key, std::string t_upload_id);

    /// Streams a local file as a real multipart upload (bounded memory:
    /// ~one part in RAM). Resolves with the number of parts assembled,
    /// stored by the service layer as the upload's chunking trace.
    drogon::Task<Result<int, BackendError>> uploadFileMultipart(
        std::string t_bucket, std::string t_key, std::string t_content_type, std::filesystem::path t_file_path, size_t t_part_size_bytes);

    drogon::Task<Result<S3ListPartsResult, BackendError>> listParts(
        std::string t_bucket, std::string t_key, std::string t_upload_id, uint32_t t_max_parts = 1000);

    // ========================================================================
    // Tagging & Metadata Operations
    // ========================================================================
    drogon::Task<Result<void, BackendError>> putObjectTags(std::string t_bucket, std::string t_key, S3Tags t_tags);

    drogon::Task<Result<S3Tags, BackendError>> getObjectTags(std::string t_bucket, std::string t_key);

    drogon::Task<Result<void, BackendError>> deleteObjectTags(std::string t_bucket, std::string t_key);

    // ========================================================================
    // Presigned URL Operations
    // ========================================================================
    drogon::Task<Result<std::string, BackendError>> presignedGetUrl(std::string t_bucket, std::string t_key, uint32_t t_expiry_seconds);

    drogon::Task<Result<std::string, BackendError>> presignedPutUrl(std::string t_bucket, std::string t_key, uint32_t t_expiry_seconds);

    drogon::Task<Result<std::string, BackendError>> presignedDeleteUrl(std::string t_bucket, std::string t_key, uint32_t t_expiry_seconds);

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
    std::shared_ptr<NativeS3Client> client_;
    std::shared_ptr<NativeS3Client> public_client_;
    std::shared_ptr<sgrn::utils::ManagedThreadPool> s3_pool_;
    std::string endpoint_;
    std::string public_endpoint_;
    std::string region_;

    // ── Pool access ───────────────────────────────────────────────────────────
    // CPU hashing/compression pool owned by the Threadpool plugin.
    // S3Client uses its own dedicated `s3_pool_` for S3 network/disk I/O.
    static sgrn::datastore::plugins::Threadpool* threadpool() {
        return drogon::app().getPlugin<sgrn::datastore::plugins::Threadpool>();
    }

    drogon::Task<Result<std::string, BackendError>> generatePresignedUrl(
        std::string t_bucket, std::string t_key, uint32_t t_expiry_seconds, drogon::HttpMethod t_method);

    template <typename ReturnType, typename CallableType>
    drogon::Task<Result<ReturnType, BackendError>> execute(CallableType&& t_fun) {
        std::shared_ptr<NativeS3Client> sp_client_copy;
        std::shared_ptr<sgrn::utils::ManagedThreadPool> sp_pool_copy;
        {
            std::lock_guard lock(client_mutex_);
            sp_client_copy = client_;
            sp_pool_copy = s3_pool_;
        }

        if (sp_client_copy == nullptr) {
            co_return ::sgrn::datastore::plugins::aws::toBackendError(
                ::sgrn::datastore::plugins::aws::S3Error::NotInitialized, "S3 client not initialized");
        }
        if (sp_pool_copy == nullptr) {
            co_return BackendError(BackendErrorKind::Runtime, "S3 worker pool not initialized");
        }

        auto* p_pool = sp_pool_copy->get();
        co_return co_await sgrn::utils::runInPool(p_pool,
            [client_ = std::move(sp_client_copy), pool_guard_ = std::move(sp_pool_copy),
                t_fun = std::forward<CallableType>(t_fun)]() mutable -> Result<ReturnType, BackendError> {
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
