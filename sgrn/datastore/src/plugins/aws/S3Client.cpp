#include <drogon/drogon.h>
#include <fmt/format.h>
#include <sgrn/datastore/plugins/aws/S3Client.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/debug.hpp>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/http/HttpTypes.h>
#include <aws/s3/model/AbortMultipartUploadRequest.h>
#include <aws/s3/model/CompleteMultipartUploadRequest.h>
#include <aws/s3/model/CopyObjectRequest.h>
#include <aws/s3/model/CreateBucketRequest.h>
#include <aws/s3/model/CreateMultipartUploadRequest.h>
#include <aws/s3/model/DeleteBucketRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/DeleteObjectTaggingRequest.h>
#include <aws/s3/model/DeleteObjectsRequest.h>
#include <aws/s3/model/GetObjectAclRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/GetObjectTaggingRequest.h>
#include <aws/s3/model/HeadBucketRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListBucketsRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/ListPartsRequest.h>
#include <aws/s3/model/PutObjectAclRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/PutObjectTaggingRequest.h>
#include <aws/s3/model/UploadPartRequest.h>

#ifdef DEBUG_PLUGIN_AWS_S3
#define DEBUG_LOG(msg, ...) SGRN_DEBUG("S3Client", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define DEBUG_LOG(...) ((void)0)
#endif

#define INFO_LOG(msg, ...) SGRN_INFO("S3Client", msg __VA_OPT__(, ) __VA_ARGS__)
#define WARN_LOG(msg, ...) SGRN_WARN("S3Client", msg __VA_OPT__(, ) __VA_ARGS__)
#define ERROR_LOG(msg, ...) SGRN_ERROR("S3Client", msg __VA_OPT__(, ) __VA_ARGS__)

// Undefine Windows macros that conflict with AWS SDK
#ifdef GetObject
#undef GetObject
#endif
#ifdef GetMessage
#undef GetMessage
#endif

namespace sgrn::datastore::plugins::aws
{
using namespace drogon;
using ::sgrn::Result;
using sgrn::datastore::BackendError;
using sgrn::datastore::BackendResult;

void S3Client::initAndStart(const Json::Value& t_config) {
    region_ = t_config.get("region", "us-east-1").asString();
    endpoint_ = t_config.get("endpoint", "").asString();
    public_endpoint_ = t_config.get("public_endpoint", endpoint_).asString();

    Aws::InitAPI(sdk_options_); // now synchronous

    const std::string access_key = t_config.get("access_key", "").asString();
    const std::string secret_key = t_config.get("secret_key", "").asString();
    Aws::Auth::AWSCredentials credentials(access_key, secret_key);

    Aws::S3::S3ClientConfiguration cfg;
    cfg.region = region_;
    cfg.useVirtualAddressing = false;
    if (!endpoint_.empty())
        cfg.endpointOverride = endpoint_;

    client_ = std::make_shared<Aws::S3::S3Client>(credentials, nullptr, cfg);

    if (!public_endpoint_.empty() && public_endpoint_ != endpoint_) {
        Aws::S3::S3ClientConfiguration pub_cfg;
        pub_cfg.region = region_;
        pub_cfg.useVirtualAddressing = false;
        pub_cfg.endpointOverride = public_endpoint_;
        public_client_ = std::make_shared<Aws::S3::S3Client>(credentials, nullptr, pub_cfg);
    }
}
void S3Client::shutdown() {
    {
        std::lock_guard lock(client_mutex_);
        client_.reset();
        public_client_.reset();
    }
    Aws::ShutdownAPI(sdk_options_);
}

Task<BackendResult<void>> S3Client::uploadFile(std::string t_bucket, std::string t_key, std::string t_file_path) {
    co_return co_await execute<void>([t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_file_path = std::move(t_file_path)](
                                         Aws::S3::S3Client& t_client) -> ::sgrn::datastore::BackendResult<void> {
        Aws::S3::Model::PutObjectRequest req;
        req.SetBucket(t_bucket.c_str());
        req.SetKey(t_key.c_str());

        auto input_stream = Aws::MakeShared<Aws::FStream>("UploadStream", t_file_path.c_str(), std::ios::in | std::ios::binary);

        if (!*input_stream) {
            std::string error = "Cannot open file: " + t_file_path;
            ERROR_LOG(error);
            return ::sgrn::datastore::BackendError{::sgrn::datastore::scope_file_system, std::move(error)};
        }

        req.SetBody(input_stream);

        auto outcome = t_client.PutObject(req);

        if (!outcome.IsSuccess()) {
            std::string error = "Upload failed: " + outcome.GetError().GetMessage();
            ERROR_LOG(error);
            return ::sgrn::datastore::BackendError{::sgrn::datastore::scope_minio, std::move(error)};
        }
        return {};
    });
}

Task<BackendResult<void>> S3Client::uploadFromMemory(
    std::string t_bucket, std::string t_key, std::string t_data, std::string t_content_type) {
    co_return co_await execute<void>([t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_data = std::move(t_data),
                                         t_content_type = std::move(t_content_type)](Aws::S3::S3Client& t_client) -> BackendResult<void> {
        Aws::S3::Model::PutObjectRequest req;
        req.SetBucket(t_bucket.c_str());
        req.SetKey(t_key.c_str());

        auto stream = Aws::MakeShared<Aws::StringStream>("UploadStream", std::move(t_data));
        req.SetBody(stream);

        if (!t_content_type.empty()) {
            req.SetContentType(t_content_type.c_str());
        }
        auto outcome = t_client.PutObject(req);

        if (!outcome.IsSuccess()) {
            std::string error = "Upload from memory failed: " + outcome.GetError().GetMessage();
            ERROR_LOG(error);
            return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
        }
        return {};
    });
}

Task<BackendResult<void>> S3Client::uploadWithMetadata(
    std::string t_bucket, std::string t_key, std::string t_file_path, Json::Value t_metadata) {
    co_return co_await execute<void>([t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_file_path = std::move(t_file_path),
                                         t_metadata = std::move(t_metadata)](Aws::S3::S3Client& t_client) -> BackendResult<void> {
        Aws::S3::Model::PutObjectRequest req;
        req.SetBucket(t_bucket.c_str());
        req.SetKey(t_key.c_str());

        auto input_stream = Aws::MakeShared<Aws::FStream>("UploadStream", t_file_path.c_str(), std::ios::in | std::ios::binary);

        if (!*input_stream) {
            std::string error = "Cannot open file: " + t_file_path;
            ERROR_LOG(error);
            return BackendError{::sgrn::datastore::scope_file_system, std::move(error)};
        }

        req.SetBody(input_stream);

        for (const auto& key_name : t_metadata.getMemberNames()) {
            req.AddMetadata(key_name.c_str(), t_metadata[key_name].asString().c_str());
        }

        auto outcome = t_client.PutObject(req);

        if (!outcome.IsSuccess()) {
            std::string error = "[S3Client] Upload with metadata failed: " + outcome.GetError().GetMessage();
            ERROR_LOG(error);
            return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
        }

        return {};
    });
}

Task<BackendResult<void>> S3Client::downloadFile(std::string t_bucket, std::string t_key, std::string t_download_path) {
    co_return co_await execute<void>([t_bucket = std::move(t_bucket), t_key = std::move(t_key),
                                         t_download_path = std::move(t_download_path)](Aws::S3::S3Client& t_client) -> BackendResult<void> {
        Aws::S3::Model::GetObjectRequest req;
        req.SetBucket(t_bucket.c_str());
        req.SetKey(t_key.c_str());

        auto outcome = t_client.GetObject(req);

        if (!outcome.IsSuccess()) {
            std::string error = "Download failed: " + outcome.GetError().GetMessage();
            ERROR_LOG(error);
            return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
        }

        std::ofstream out(t_download_path, std::ios::binary);
        if (!out) {
            return BackendError{::sgrn::datastore::scope_file_system, "Failed to open local file for writing: " + t_download_path};
        }
        out << outcome.GetResult().GetBody().rdbuf();
        return {};
    });
}

Task<BackendResult<void>> S3Client::downloadFileRange(
    std::string t_bucket, std::string t_key, std::string t_download_path, int64_t t_start, int64_t t_end) {
    co_return co_await execute<void>(
        [t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_download_path = std::move(t_download_path), t_start = t_start,
            t_end = t_end](Aws::S3::S3Client& t_client) -> BackendResult<void> {
            Aws::S3::Model::GetObjectRequest req;
            req.SetBucket(t_bucket.c_str());
            req.SetKey(t_key.c_str());

            std::stringstream range;
            range << "bytes=" << t_start << "-" << t_end;
            req.SetRange(range.str().c_str());

            auto outcome = t_client.GetObject(req);

            if (!outcome.IsSuccess()) {
                std::string error = "[S3Client] Download range failed: " + outcome.GetError().GetMessage();
                ERROR_LOG(error);
                return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
            }

            std::ofstream out(t_download_path, std::ios::binary);
            if (!out) {
                return BackendError{::sgrn::datastore::scope_file_system, "Failed to open local file for writing: " + t_download_path};
            }
            out << outcome.GetResult().GetBody().rdbuf();
            return {};
        });
}

Task<BackendResult<std::string>> S3Client::getObjectContent(std::string t_bucket, std::string t_key) {
    co_return co_await execute<std::string>(
        [t_bucket = std::move(t_bucket), t_key = std::move(t_key)](Aws::S3::S3Client& t_client) -> BackendResult<std::string> {
            Aws::S3::Model::GetObjectRequest req;
            req.SetBucket(t_bucket.c_str());
            req.SetKey(t_key.c_str());

            auto outcome = t_client.GetObject(req);

            if (!outcome.IsSuccess()) {
                std::string error = "[S3Client] GetObject failed: " + outcome.GetError().GetMessage();
                ERROR_LOG(error);
                return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
            }

            auto& result = outcome.GetResult();
            size_t reported_len = static_cast<size_t>(result.GetContentLength());
            INFO_LOG("[S3Client] getObjectContent: bucket='{}' key='{}' reported_content_length={}", t_bucket, t_key, reported_len);

            Aws::IOStream& body_stream = result.GetBody();
            std::string content((std::istreambuf_iterator<char>(body_stream)), std::istreambuf_iterator<char>());
            INFO_LOG("[S3Client] getObjectContent: read {} bytes from stream", content.size());
            DEBUG_LOG("[S3Client] Content read - size: {}, ptr: {}", content.size(), (void*)content.data());
            return content;
        });
}

Task<BackendResult<void>> S3Client::deleteFile(std::string t_bucket, std::string t_key) {
    co_return co_await execute<void>(
        [t_bucket = std::move(t_bucket), t_key = std::move(t_key)](Aws::S3::S3Client& t_client) -> BackendResult<void> {
            Aws::S3::Model::DeleteObjectRequest req;
            req.SetBucket(t_bucket.c_str());
            req.SetKey(t_key.c_str());

            auto outcome = t_client.DeleteObject(req);

            if (!outcome.IsSuccess()) {
                std::string error = "[S3Client] Delete failed: " + outcome.GetError().GetMessage();
                ERROR_LOG(error);
                return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
            }

            return {};
        });
}

Task<BackendResult<Json::Value>> S3Client::deleteFiles(std::string t_bucket, std::vector<std::string> t_keys) {
    co_return co_await execute<Json::Value>(
        [t_bucket = std::move(t_bucket), t_keys = std::move(t_keys)](Aws::S3::S3Client& t_client) -> BackendResult<Json::Value> {
            Json::Value result;
            result["deleted"] = Json::Value(Json::arrayValue);
            result["errors"] = Json::Value(Json::arrayValue);

            Aws::S3::Model::DeleteObjectsRequest req;
            req.SetBucket(t_bucket.c_str());

            Aws::S3::Model::Delete deleteContainer;
            for (const auto& t_key : t_keys) {
                Aws::S3::Model::ObjectIdentifier obj;
                obj.SetKey(t_key.c_str());
                deleteContainer.AddObjects(obj);
            }
            req.SetDelete(deleteContainer);

            auto outcome = t_client.DeleteObjects(req);

            if (!outcome.IsSuccess()) {
                std::string error = "Bulk delete failed: " + outcome.GetError().GetMessage();
                ERROR_LOG(error);
                return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
            }

            for (const auto& deleted : outcome.GetResult().GetDeleted()) {
                result["deleted"].append(deleted.GetKey().c_str());
            }

            for (const auto& error_obj : outcome.GetResult().GetErrors()) {
                Json::Value err;
                err["key"] = error_obj.GetKey().c_str();
                err["code"] = error_obj.GetCode().c_str();
                err["message"] = error_obj.GetMessage().c_str();
                result["errors"].append(err);
            }
            return result;
        });
}

Task<BackendResult<void>> S3Client::copyFile(
    std::string t_src_bucket, std::string t_src_key, std::string t_dest_bucket, std::string t_dest_key) {
    co_return co_await execute<void>(
        [t_src_bucket = std::move(t_src_bucket), t_src_key = std::move(t_src_key), t_dest_bucket = std::move(t_dest_bucket),
            t_dest_key = std::move(t_dest_key)](Aws::S3::S3Client& t_client) -> BackendResult<void> {
            Aws::S3::Model::CopyObjectRequest req;

            std::string copySource = t_src_bucket + "/" + t_src_key;
            req.SetCopySource(copySource.c_str());
            req.SetBucket(t_dest_bucket.c_str());
            req.SetKey(t_dest_key.c_str());

            auto outcome = t_client.CopyObject(req);

            if (!outcome.IsSuccess()) {
                std::string error = "Copy failed: " + outcome.GetError().GetMessage();
                ERROR_LOG(error);
                return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
            }

            return {};
        });
}

Task<BackendResult<bool>> S3Client::exists(std::string t_bucket, std::string t_key) {
    co_return co_await execute<bool>(
        [t_bucket = std::move(t_bucket), t_key = std::move(t_key)](Aws::S3::S3Client& t_client) -> BackendResult<bool> {
            Aws::S3::Model::HeadObjectRequest req;
            req.SetBucket(t_bucket.c_str());
            req.SetKey(t_key.c_str());

            auto outcome = t_client.HeadObject(req);

            if (outcome.IsSuccess()) {
                return true;
            }

            auto error = outcome.GetError();
            if (error.GetErrorType() == Aws::S3::S3Errors::NO_SUCH_KEY || error.GetErrorType() == Aws::S3::S3Errors::RESOURCE_NOT_FOUND) {
                return false;
            }

            std::string error_msg = "HeadObject failed: " + error.GetMessage();
            ERROR_LOG(error_msg);
            return BackendError{::sgrn::datastore::scope_minio, std::move(error_msg)};
        });
}

Task<BackendResult<Json::Value>> S3Client::statObject(std::string t_bucket, std::string t_key) {
    co_return co_await execute<Json::Value>([t_bucket = std::move(t_bucket), t_key = std::move(t_key)](
                                                Aws::S3::S3Client& t_client) -> ::sgrn::datastore::BackendResult<Json::Value> {
        Aws::S3::Model::HeadObjectRequest req;
        req.SetBucket(t_bucket.c_str());
        req.SetKey(t_key.c_str());

        auto outcome = t_client.HeadObject(req);

        if (!outcome.IsSuccess()) {
            std::string error = "[S3Client] Head failed: " + outcome.GetError().GetMessage();
            ERROR_LOG(error);
            return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
        }

        auto& result = outcome.GetResult();
        Json::Value meta;
        meta["content_length"] = static_cast<Json::Int64>(result.GetContentLength());
        meta["content_type"] = result.GetContentType().c_str();
        meta["etag"] = result.GetETag().c_str();
        meta["last_modified"] = result.GetLastModified().ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str();

        Json::Value user_meta;
        for (const auto& kv : result.GetMetadata()) {
            user_meta[kv.first.c_str()] = kv.second.c_str();
        }
        meta["metadata"] = user_meta;

        return meta;
    });
}

Task<BackendResult<Json::Value>> S3Client::listObjects(
    std::string t_bucket, std::string t_prefix, std::string t_continuation_token, uint32_t t_max_keys) {
    co_return co_await execute<Json::Value>([t_bucket = std::move(t_bucket), t_prefix = std::move(t_prefix),
                                                t_continuation_token = std::move(t_continuation_token),
                                                t_max_keys = t_max_keys](Aws::S3::S3Client& t_client) -> BackendResult<Json::Value> {
        Aws::S3::Model::ListObjectsV2Request req;
        req.SetBucket(t_bucket.c_str());

        if (!t_prefix.empty()) {
            req.SetPrefix(t_prefix.c_str());
        }

        if (t_max_keys > 0) {
            req.SetMaxKeys(t_max_keys);
        }

        if (!t_continuation_token.empty()) {
            req.SetContinuationToken(t_continuation_token.c_str());
        }

        auto outcome = t_client.ListObjectsV2(req);

        if (!outcome.IsSuccess()) {
            std::string error = "[S3Client] List objects failed: " + outcome.GetError().GetMessage();
            ERROR_LOG(error);
            return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
        }

        auto& list_result = outcome.GetResult();
        Json::Value result;
        result["objects"] = Json::Value(Json::arrayValue);

        for (const auto& object : list_result.GetContents()) {
            Json::Value obj;
            obj["key"] = object.GetKey().c_str();
            obj["size"] = static_cast<Json::Int64>(object.GetSize());
            obj["etag"] = object.GetETag().c_str();
            obj["last_modified"] = object.GetLastModified().ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str();
            obj["storage_class"] = Aws::S3::Model::ObjectStorageClassMapper::GetNameForObjectStorageClass(object.GetStorageClass()).c_str();
            result["objects"].append(obj);
        }

        result["is_truncated"] = list_result.GetIsTruncated();
        if (list_result.GetIsTruncated()) {
            result["next_continuation_token"] = list_result.GetNextContinuationToken().c_str();
        }
        return result;
    });
}

Task<BackendResult<void>> S3Client::createBucket(std::string t_bucket) {
    co_return co_await execute<void>([t_bucket = std::move(t_bucket)](Aws::S3::S3Client& t_client) -> BackendResult<void> {
        Aws::S3::Model::CreateBucketRequest req;
        req.SetBucket(t_bucket.c_str());

        auto outcome = t_client.CreateBucket(req);

        if (!outcome.IsSuccess()) {
            std::string error = "Create bucket failed: " + outcome.GetError().GetMessage();
            ERROR_LOG(error);
            return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
        }

        return {};
    });
}

Task<BackendResult<void>> S3Client::deleteBucket(std::string t_bucket) {
    co_return co_await execute<void>([t_bucket = std::move(t_bucket)](Aws::S3::S3Client& t_client) -> BackendResult<void> {
        Aws::S3::Model::DeleteBucketRequest req;
        req.SetBucket(t_bucket.c_str());

        auto outcome = t_client.DeleteBucket(req);

        if (!outcome.IsSuccess()) {
            std::string error = "Delete bucket failed: " + outcome.GetError().GetMessage();
            ERROR_LOG(error);
            return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
        }

        return {};
    });
}

Task<BackendResult<bool>> S3Client::bucketExists(std::string t_bucket) {
    co_return co_await execute<bool>([t_bucket = std::move(t_bucket)](Aws::S3::S3Client& t_client) -> BackendResult<bool> {
        Aws::S3::Model::HeadBucketRequest req;
        req.SetBucket(t_bucket.c_str());

        auto outcome = t_client.HeadBucket(req);

        if (outcome.IsSuccess()) {
            return true;
        }

        auto error = outcome.GetError();
        if (error.GetErrorType() == Aws::S3::S3Errors::NO_SUCH_BUCKET || error.GetErrorType() == Aws::S3::S3Errors::RESOURCE_NOT_FOUND) {
            return false;
        }

        std::string error_msg = "HeadBucket failed: " + error.GetMessage();
        ERROR_LOG(error_msg);
        return BackendError{::sgrn::datastore::scope_minio, std::move(error_msg)};
    });
}

Task<BackendResult<Json::Value>> S3Client::listBuckets() {
    co_return co_await execute<Json::Value>([](Aws::S3::S3Client& t_client) -> BackendResult<Json::Value> {
        auto outcome = t_client.ListBuckets();

        if (!outcome.IsSuccess()) {
            std::string error = "List buckets failed: " + outcome.GetError().GetMessage();
            ERROR_LOG(error);
            return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
        }

        Json::Value result;
        result["buckets"] = Json::Value(Json::arrayValue);

        for (const auto& t_bucket : outcome.GetResult().GetBuckets()) {
            Json::Value b;
            b["name"] = t_bucket.GetName().c_str();
            b["creation_date"] = t_bucket.GetCreationDate().ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str();
            result["buckets"].append(b);
        }
        return result;
    });
}

Task<BackendResult<Json::Value>> S3Client::getObjectTags(std::string t_bucket, std::string t_key) {
    co_return co_await execute<Json::Value>(
        [t_bucket = std::move(t_bucket), t_key = std::move(t_key)](Aws::S3::S3Client& t_client) -> BackendResult<Json::Value> {
            Json::Value result(Json::objectValue);

            Aws::S3::Model::GetObjectTaggingRequest req;
            req.SetBucket(t_bucket.c_str());
            req.SetKey(t_key.c_str());

            auto outcome = t_client.GetObjectTagging(req);

            if (!outcome.IsSuccess()) {
                std::string error = "[S3Client] Get tags failed: " + outcome.GetError().GetMessage();
                ERROR_LOG(error);
                return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
            }

            for (const auto& tag : outcome.GetResult().GetTagSet()) {
                result[tag.GetKey().c_str()] = tag.GetValue().c_str();
            }
            return result;
        });
}

Task<BackendResult<void>> S3Client::putObjectTags(std::string t_bucket, std::string t_key, Json::Value t_tags) {
    co_return co_await execute<void>([t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_tags = std::move(t_tags)](
                                         Aws::S3::S3Client& t_client) -> BackendResult<void> {
        Aws::S3::Model::PutObjectTaggingRequest req;
        req.SetBucket(t_bucket.c_str());
        req.SetKey(t_key.c_str());

        Aws::S3::Model::Tagging tagging;
        for (const auto& key_name : t_tags.getMemberNames()) {
            Aws::S3::Model::Tag tag;
            tag.SetKey(key_name.c_str());
            tag.SetValue(t_tags[key_name].asString().c_str());
            tagging.AddTagSet(tag);
        }
        req.SetTagging(tagging);

        auto outcome = t_client.PutObjectTagging(req);

        if (!outcome.IsSuccess()) {
            std::string error = "Put tags failed: " + outcome.GetError().GetMessage();
            ERROR_LOG(error);
            return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
        }

        return {};
    });
}

Task<BackendResult<void>> S3Client::deleteObjectTags(std::string t_bucket, std::string t_key) {
    co_return co_await execute<void>(
        [t_bucket = std::move(t_bucket), t_key = std::move(t_key)](Aws::S3::S3Client& t_client) -> BackendResult<void> {
            Aws::S3::Model::DeleteObjectTaggingRequest req;
            req.SetBucket(t_bucket.c_str());
            req.SetKey(t_key.c_str());

            auto outcome = t_client.DeleteObjectTagging(req);

            if (!outcome.IsSuccess()) {
                std::string error = "Delete tags failed: " + outcome.GetError().GetMessage();
                ERROR_LOG(error);
                return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
            }

            return {};
        });
}

Task<BackendResult<std::string>> S3Client::createMultipartUpload(std::string t_bucket, std::string t_key, std::string t_content_type) {
    co_return co_await execute<std::string>(
        [t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_content_type = std::move(t_content_type)](
            Aws::S3::S3Client& t_client) -> BackendResult<std::string> {
            Aws::S3::Model::CreateMultipartUploadRequest req;
            req.SetBucket(t_bucket.c_str());
            req.SetKey(t_key.c_str());

            if (!t_content_type.empty()) {
                req.SetContentType(t_content_type.c_str());
            }

            auto outcome = t_client.CreateMultipartUpload(req);

            if (!outcome.IsSuccess()) {
                std::string error = "Create multipart upload failed: " + outcome.GetError().GetMessage();
                ERROR_LOG(error);
                return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
            }

            return outcome.GetResult().GetUploadId();
        });
}

Task<BackendResult<std::string>> S3Client::uploadPart(
    std::string t_bucket, std::string t_key, std::string t_upload_id, int t_part_number, std::string t_body) {
    co_return co_await execute<std::string>(
        [t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_upload_id = std::move(t_upload_id), t_part_number = t_part_number,
            t_data = std::move(t_body)](Aws::S3::S3Client& t_client) -> BackendResult<std::string> {
            Aws::S3::Model::UploadPartRequest req;
            req.SetBucket(t_bucket.c_str());
            req.SetKey(t_key.c_str());
            req.SetUploadId(t_upload_id.c_str());
            req.SetPartNumber(t_part_number);

            auto stream = Aws::MakeShared<Aws::StringStream>("UploadPartStream");
            *stream << t_data;
            req.SetBody(stream);

            auto outcome = t_client.UploadPart(req);

            if (!outcome.IsSuccess()) {
                std::string error = "Upload part failed: " + outcome.GetError().GetMessage();
                ERROR_LOG(error);
                return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
            }

            return outcome.GetResult().GetETag();
        });
}

Task<BackendResult<void>> S3Client::completeMultipartUpload(
    std::string t_bucket, std::string t_key, std::string t_upload_id, Json::Value t_parts) {
    co_return co_await execute<void>([t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_upload_id = std::move(t_upload_id),
                                         t_parts = std::move(t_parts)](Aws::S3::S3Client& t_client) -> BackendResult<void> {
        Aws::S3::Model::CompleteMultipartUploadRequest req;
        req.SetBucket(t_bucket.c_str());
        req.SetKey(t_key.c_str());
        req.SetUploadId(t_upload_id.c_str());

        Aws::S3::Model::CompletedMultipartUpload completed;
        for (const auto& part : t_parts) {
            Aws::S3::Model::CompletedPart p;
            p.SetPartNumber(part["part_number"].asInt());
            p.SetETag(part["etag"].asString().c_str());
            completed.AddParts(p);
        }
        req.SetMultipartUpload(completed);

        auto outcome = t_client.CompleteMultipartUpload(req);

        if (!outcome.IsSuccess()) {
            std::string error = "Complete multipart upload failed: " + outcome.GetError().GetMessage();
            ERROR_LOG(error);
            return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
        }

        return {};
    });
}

Task<BackendResult<void>> S3Client::abortMultipartUpload(std::string t_bucket, std::string t_key, std::string t_upload_id) {
    co_return co_await execute<void>([t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_upload_id = std::move(t_upload_id)](
                                         Aws::S3::S3Client& t_client) -> BackendResult<void> {
        Aws::S3::Model::AbortMultipartUploadRequest req;
        req.SetBucket(t_bucket.c_str());
        req.SetKey(t_key.c_str());
        req.SetUploadId(t_upload_id.c_str());

        auto outcome = t_client.AbortMultipartUpload(req);

        if (!outcome.IsSuccess()) {
            std::string error = "Abort multipart upload failed: " + outcome.GetError().GetMessage();
            ERROR_LOG(error);
            return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
        }

        return {};
    });
}

Task<BackendResult<Json::Value>> S3Client::listParts(
    std::string t_bucket, std::string t_key, std::string t_upload_id, uint32_t t_max_parts) {
    co_return co_await execute<Json::Value>([t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_upload_id = std::move(t_upload_id),
                                                t_max_parts = t_max_parts](Aws::S3::S3Client& t_client) -> BackendResult<Json::Value> {
        Json::Value result;
        result["parts"] = Json::Value(Json::arrayValue);

        Aws::S3::Model::ListPartsRequest req;
        req.SetBucket(t_bucket.c_str());
        req.SetKey(t_key.c_str());
        req.SetUploadId(t_upload_id.c_str());

        if (t_max_parts > 0) {
            req.SetMaxParts(t_max_parts);
        }

        auto outcome = t_client.ListParts(req);

        if (!outcome.IsSuccess()) {
            std::string error = "List parts failed: " + outcome.GetError().GetMessage();
            ERROR_LOG(error);
            return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
        }

        for (const auto& part : outcome.GetResult().GetParts()) {
            Json::Value p;
            p["part_number"] = part.GetPartNumber();
            p["etag"] = part.GetETag().c_str();
            p["size"] = static_cast<Json::Int64>(part.GetSize());
            p["last_modified"] = part.GetLastModified().ToGmtString(Aws::Utils::DateFormat::ISO_8601).c_str();
            result["parts"].append(p);
        }
        return result;
    });
}

Task<BackendResult<std::string>> S3Client::generatePresignedUrl(
    std::string t_bucket, std::string t_key, uint32_t t_expiry_seconds, Aws::Http::HttpMethod t_method) {
    std::shared_ptr<Aws::S3::S3Client> sp_pub_client;
    {
        std::lock_guard lock(client_mutex_);
        sp_pub_client = public_client_;
    }

    co_return co_await execute<std::string>([sp_pub_client = std::move(sp_pub_client), t_bucket = std::move(t_bucket),
                                                t_key = std::move(t_key), t_expiry_seconds = t_expiry_seconds, t_method = t_method](
                                                Aws::S3::S3Client& t_client) -> ::sgrn::datastore::BackendResult<std::string> {
        try {
            Aws::S3::S3Client* client_to_use = sp_pub_client ? sp_pub_client.get() : &t_client;

            std::string url = client_to_use->GeneratePresignedUrl(t_bucket.c_str(), t_key.c_str(), t_method, t_expiry_seconds);

            return url;
        } catch (const std::exception& ex) {
            std::string error = "Generate presigned URL failed: " + std::string(ex.what());
            ERROR_LOG(error);
            return BackendError{::sgrn::datastore::scope_minio, std::move(error)};
        }
    });
}

Task<BackendResult<std::string>> S3Client::presignedGetUrl(std::string t_bucket, std::string t_key, uint32_t t_expiry_seconds) {
    co_return co_await generatePresignedUrl(std::move(t_bucket), std::move(t_key), t_expiry_seconds, Aws::Http::HttpMethod::HTTP_GET);
}

Task<BackendResult<std::string>> S3Client::presignedPutUrl(std::string t_bucket, std::string t_key, uint32_t t_expiry_seconds) {
    co_return co_await generatePresignedUrl(std::move(t_bucket), std::move(t_key), t_expiry_seconds, Aws::Http::HttpMethod::HTTP_PUT);
}

Task<BackendResult<std::string>> S3Client::presignedDeleteUrl(std::string t_bucket, std::string t_key, uint32_t t_expiry_seconds) {
    co_return co_await generatePresignedUrl(std::move(t_bucket), std::move(t_key), t_expiry_seconds, Aws::Http::HttpMethod::HTTP_DELETE);
}

} // namespace sgrn::datastore::plugins::aws

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
