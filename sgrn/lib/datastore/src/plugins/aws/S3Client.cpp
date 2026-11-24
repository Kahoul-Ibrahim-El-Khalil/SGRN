#include <drogon/drogon.h>
#include <fmt/format.h>
#include <sgrn/datastore/plugins/aws/S3Client.hpp>
#include <sgrn/datastore/plugins/aws/S3Error.hpp>
#include <sgrn/datastore/utils/helpers.hpp>
#include <sgrn/debug.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <sgrn/utils/hashing.hpp>
#include <sgrn/utils/strings.hpp>
#include <sgrn/utils/xml.hpp>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef DEBUG_PLUGIN_AWS_S3
#define DEBUG_LOG(msg, ...) SGRN_DEBUG("S3Client", msg __VA_OPT__(, ) __VA_ARGS__)
#else
#define DEBUG_LOG(...) ((void)0)
#endif

#define INFO_LOG(msg, ...) SGRN_INFO("S3Client", msg __VA_OPT__(, ) __VA_ARGS__)
#define WARN_LOG(msg, ...) SGRN_WARN("S3Client", msg __VA_OPT__(, ) __VA_ARGS__)
#define ERROR_LOG(msg, ...) SGRN_ERROR("S3Client", msg __VA_OPT__(, ) __VA_ARGS__)

namespace sgrn::datastore::plugins::aws
{
using namespace drogon;

namespace
{

constexpr size_t kMaxDeleteBatchSize = 1000;
constexpr size_t kMaxMultipartParts = 10000;

BackendError makeS3Error(const S3Response& resp, std::string_view prefix) {
    auto [xml_code, _] = NativeS3Client::extractXmlError(resp.body);
    auto s3e = fromHttpStatus(resp.status, xml_code, resp.is_timeout);
    std::string msg = std::string(prefix) + resp.error_message;
    ERROR_LOG("{}", msg);
    return toBackendError(s3e, msg);
}

std::string md5Base64OrThrow(std::string_view data) {
    auto res = sgrn::utils::computeMd5Data(data, sgrn::utils::HashEncoding::base64);
    if (res.hasError())
        throw std::runtime_error(res.error());
    return std::move(*res);
}

bool isValidHeaderToken(std::string_view s) {
    if (s.empty())
        return false;
    for (unsigned char c : s) {
        if (c <= 0x20 || c >= 0x7F || c == ':')
            return false;
    }
    return true;
}

void abortMultipartQuietly(NativeS3Client& client, const std::string& bucket, const std::string& key, const std::string& upload_id) {
    auto resp =
        client.performRequest(drogon::Delete, bucket, key, {{"uploadId", upload_id}}, {}, "", NativeS3Client::kDefaultMetaTimeoutSeconds);
    if (!resp.isSuccess()) {
        WARN_LOG("Abort multipart upload failed for {}/{}: {}", bucket, key, resp.error_message);
    }
}

Result<void, BackendError> completeMultipartSync(NativeS3Client& client, const std::string& bucket, const std::string& key,
    const std::string& upload_id, const std::vector<S3CompletedPart>& parts) {
    std::string xml;
    xml.reserve(96 + parts.size() * 96);
    xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?><CompleteMultipartUpload>";
    for (const auto& part : parts) {
        xml += "<Part><PartNumber>";
        xml += std::to_string(part.part_number);
        xml += "</PartNumber><ETag>";
        xml += sgrn::utils::xml::xmlEscape(part.etag);
        xml += "</ETag></Part>";
    }
    xml += "</CompleteMultipartUpload>";

    std::map<std::string, std::string> headers;
    headers["content-type"] = "application/xml";

    auto resp = client.performRequest(
        drogon::Post, bucket, key, {{"uploadId", upload_id}}, headers, std::move(xml), NativeS3Client::kDefaultMetaTimeoutSeconds);
    if (!resp.isSuccess())
        return makeS3Error(resp, "Complete multipart upload failed: ");
    return {};
}

// Shared fetch helper used by both getObjectResponse (zero-copy view) and getObjectContent (owned string).
Result<S3Response, BackendError> fetchObjectSync(
    NativeS3Client& client, const std::string& bucket, const std::string& key, int64_t max_bytes) {
    std::map<std::string, std::string> headers;
    if (max_bytes > 0)
        headers["range"] = "bytes=0-" + std::to_string(max_bytes - 1);

    auto resp = client.performRequest(drogon::Get, bucket, key, {}, headers);
    if (!resp.isSuccess()) {
        // Range GET on a 0-byte object returns 416 Range Not Satisfiable; verify via HEAD.
        if (max_bytes > 0 && !resp.network_error && resp.status == 416) {
            auto head = client.performRequest(drogon::Head, bucket, key, {}, {}, "", NativeS3Client::kDefaultMetaTimeoutSeconds);
            if (head.isSuccess() && head.header("content-length") == "0")
                return head;
        }
        return makeS3Error(resp, "[S3Client] GetObject failed: ");
    }

    size_t reported_len = 0;
    if (auto cl = resp.header("content-length"); !cl.empty()) {
        try {
            reported_len = static_cast<size_t>(std::stoull(cl));
        } catch (...) {
        }
    }

    if (max_bytes > 0 && resp.body.size() > static_cast<size_t>(max_bytes))
        resp.body = resp.body.substr(0, static_cast<size_t>(max_bytes));

    if (max_bytes <= 0 && reported_len > 0 && resp.body.size() != reported_len) {
        std::string error = fmt::format("[S3Client] GetObject truncated read: expected {} bytes, got {}", reported_len, resp.body.size());
        ERROR_LOG("{}", error);
        return BackendError{BackendErrorKind::Garage, "Storage", error};
    }
    return resp;
}

} // namespace

void S3Client::initAndStart(const Json::Value& t_config) {
    region_ = t_config.get("region", "us-east-1").asString();
    endpoint_ = t_config.get("endpoint", "").asString();
    public_endpoint_ = t_config.get("public_endpoint", endpoint_).asString();

    const std::string access_key = t_config.get("access_key", "").asString();
    const std::string secret_key = t_config.get("secret_key", "").asString();

    if (endpoint_.empty() || access_key.empty() || secret_key.empty()) {
        ERROR_LOG("[S3Client] Missing required S3 configuration (endpoint/access_key/secret_key); client remains uninitialized");
        return;
    }

    const int worker_threads = std::clamp(t_config.get("worker_threads", 4).asInt(), 1, 64);
    const int io_threads = std::clamp(t_config.get("io_threads", worker_threads).asInt(), 1, 32);
    const bool unsigned_payload = t_config.get("unsigned_payload", false).asBool();

    s3_pool_ = std::make_shared<sgrn::utils::ManagedThreadPool>("s3-io-pool", worker_threads);
    client_ = std::make_shared<NativeS3Client>(
        endpoint_, region_, access_key, secret_key, /*sign_only=*/false, static_cast<size_t>(io_threads), unsigned_payload);

    if (!public_endpoint_.empty() && public_endpoint_ != endpoint_) {
        public_client_ = std::make_shared<NativeS3Client>(public_endpoint_, region_, access_key, secret_key, /*sign_only=*/true);
    }
    INFO_LOG("[S3Client] Initialized (worker_threads={}, io_threads={}, unsigned_payload={}, endpoint='{}')", worker_threads, io_threads,
        unsigned_payload, endpoint_);
}

void S3Client::shutdown() {
    std::lock_guard lock(client_mutex_);
    client_.reset();
    public_client_.reset();
    s3_pool_.reset();
}

Task<Result<void, BackendError>> S3Client::uploadFile(std::string t_bucket, std::string t_key, std::string t_file_path) {
    co_return co_await execute<void>([t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_file_path = std::move(t_file_path)](
                                         NativeS3Client& t_client) -> Result<void, BackendError> {
        std::string body;
        if (!sgrn::utils::filesystem::readBinaryFile(t_file_path, body)) {
            std::string error = "Cannot open file: " + t_file_path;
            ERROR_LOG("{}", error);
            return BackendError(BackendErrorKind::Filesystem, std::move(error));
        }

        auto resp = t_client.performRequest(drogon::Put, t_bucket, t_key, {}, {}, std::move(body));
        if (!resp.isSuccess())
            return makeS3Error(resp, "Upload failed: ");
        return {};
    });
}

Task<Result<void, BackendError>> S3Client::uploadFromMemory(
    std::string t_bucket, std::string t_key, std::string t_data, std::string t_content_type) {
    co_return co_await execute<void>(
        [t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_data = std::move(t_data), t_content_type = std::move(t_content_type)](
            NativeS3Client& t_client) -> Result<void, BackendError> {
            std::map<std::string, std::string> headers;
            if (!t_content_type.empty())
                headers["content-type"] = t_content_type;

            auto resp = t_client.performRequest(drogon::Put, t_bucket, t_key, {}, headers, std::move(t_data));
            if (!resp.isSuccess())
                return makeS3Error(resp, "Upload from memory failed: ");
            return {};
        });
}

Task<Result<void, BackendError>> S3Client::uploadWithMetadata(
    std::string t_bucket, std::string t_key, std::string t_file_path, S3Metadata t_metadata) {
    co_return co_await execute<void>([t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_file_path = std::move(t_file_path),
                                         t_metadata = std::move(t_metadata)](NativeS3Client& t_client) -> Result<void, BackendError> {
        std::string body;
        if (!sgrn::utils::filesystem::readBinaryFile(t_file_path, body)) {
            std::string error = "Cannot open file: " + t_file_path;
            ERROR_LOG("{}", error);
            return BackendError(BackendErrorKind::Filesystem, std::move(error));
        }

        std::map<std::string, std::string> headers;
        for (const auto& [key_name, val] : t_metadata) {
            std::string lower_key = sgrn::utils::strings::toLower(key_name);
            if (!isValidHeaderToken(lower_key))
                return BackendError(BackendErrorKind::Runtime, "Invalid S3 metadata key: " + key_name);
            headers["x-amz-meta-" + lower_key] = sgrn::utils::strings::collapseWhitespace(val);
        }

        auto resp = t_client.performRequest(drogon::Put, t_bucket, t_key, {}, headers, std::move(body));
        if (!resp.isSuccess())
            return makeS3Error(resp, "[S3Client] Upload with metadata failed: ");
        return {};
    });
}

Task<Result<S3Response, BackendError>> S3Client::getObjectResponse(std::string t_bucket, std::string t_key, int64_t t_max_bytes) {
    co_return co_await execute<S3Response>([t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_max_bytes](
                                               NativeS3Client& t_client) -> Result<S3Response, BackendError> {
        return fetchObjectSync(t_client, t_bucket, t_key, t_max_bytes);
    });
}

Task<Result<std::string, BackendError>> S3Client::getObjectContent(std::string t_bucket, std::string t_key, int64_t t_max_bytes) {
    co_return co_await execute<std::string>([t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_max_bytes](
                                                NativeS3Client& t_client) -> Result<std::string, BackendError> {
        auto res = fetchObjectSync(t_client, t_bucket, t_key, t_max_bytes);
        if (res.hasError())
            return res.error();
        std::string content(res->body);
        DEBUG_LOG("[S3Client] Content read - size: {}, ptr: {}", content.size(), (void*)content.data());
        return content;
    });
}

Task<Result<void, BackendError>> S3Client::deleteFile(std::string t_bucket, std::string t_key) {
    co_return co_await execute<void>(
        [t_bucket = std::move(t_bucket), t_key = std::move(t_key)](NativeS3Client& t_client) -> Result<void, BackendError> {
            auto resp = t_client.performRequest(drogon::Delete, t_bucket, t_key, {}, {}, "", NativeS3Client::kDefaultMetaTimeoutSeconds);
            if (!resp.isSuccess())
                return makeS3Error(resp, "[S3Client] Delete failed: ");
            return {};
        });
}

Task<Result<S3DeleteResult, BackendError>> S3Client::deleteFiles(std::string t_bucket, std::vector<std::string> t_keys) {
    co_return co_await execute<S3DeleteResult>(
        [t_bucket = std::move(t_bucket), t_keys = std::move(t_keys)](NativeS3Client& t_client) -> Result<S3DeleteResult, BackendError> {
            S3DeleteResult result;
            if (t_keys.empty())
                return result;
            result.deleted.reserve(t_keys.size());

            for (size_t offset = 0; offset < t_keys.size(); offset += kMaxDeleteBatchSize) {
                const size_t end = std::min(offset + kMaxDeleteBatchSize, t_keys.size());

                std::string xml;
                xml.reserve(64 + (end - offset) * 96);
                xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Delete>";
                for (size_t i = offset; i < end; ++i) {
                    xml += "<Object><Key>";
                    xml += sgrn::utils::xml::xmlEscape(t_keys[i]);
                    xml += "</Key></Object>";
                }
                xml += "</Delete>";

                std::map<std::string, std::string> headers;
                headers["content-type"] = "application/xml";
                headers["content-md5"] = md5Base64OrThrow(xml);

                auto resp = t_client.performRequest(
                    drogon::Post, t_bucket, "", {{"delete", ""}}, headers, std::move(xml), NativeS3Client::kDefaultMetaTimeoutSeconds);
                if (!resp.isSuccess()) {
                    if (offset == 0)
                        return makeS3Error(resp, "Bulk delete failed: ");
                    for (size_t i = offset; i < t_keys.size(); ++i) {
                        result.errors.push_back(S3DeleteError{t_keys[i], "BatchFailed", resp.error_message});
                    }
                    break;
                }

                sgrn::utils::xml::forEachXmlBlock(resp.body, "Deleted",
                    [&](std::string_view blk) { result.deleted.push_back(sgrn::utils::xml::extractXmlTag(blk, "Key")); });
                sgrn::utils::xml::forEachXmlBlock(resp.body, "Error", [&](std::string_view blk) {
                    result.errors.push_back(S3DeleteError{
                        sgrn::utils::xml::extractXmlTag(blk, "Key"),
                        sgrn::utils::xml::extractXmlTag(blk, "Code"),
                        sgrn::utils::xml::extractXmlTag(blk, "Message"),
                    });
                });
            }
            return result;
        });
}

Task<Result<void, BackendError>> S3Client::copyFile(
    std::string t_src_bucket, std::string t_src_key, std::string t_dest_bucket, std::string t_dest_key) {
    co_return co_await execute<void>([t_src_bucket = std::move(t_src_bucket), t_src_key = std::move(t_src_key),
                                         t_dest_bucket = std::move(t_dest_bucket),
                                         t_dest_key = std::move(t_dest_key)](NativeS3Client& t_client) -> Result<void, BackendError> {
        std::map<std::string, std::string> headers;
        std::string copy_src =
            "/" + sgrn::utils::encoding::uriEncode(t_src_bucket, true) + "/" + sgrn::utils::encoding::uriEncode(t_src_key, false);
        headers["x-amz-copy-source"] = copy_src;

        auto resp =
            t_client.performRequest(drogon::Put, t_dest_bucket, t_dest_key, {}, headers, "", NativeS3Client::kDefaultMetaTimeoutSeconds);
        if (!resp.isSuccess())
            return makeS3Error(resp, "Copy failed: ");
        return {};
    });
}

Task<Result<bool, BackendError>> S3Client::exists(std::string t_bucket, std::string t_key) {
    co_return co_await execute<bool>(
        [t_bucket = std::move(t_bucket), t_key = std::move(t_key)](NativeS3Client& t_client) -> Result<bool, BackendError> {
            auto resp = t_client.performRequest(drogon::Head, t_bucket, t_key, {}, {}, "", NativeS3Client::kDefaultMetaTimeoutSeconds);
            if (resp.isSuccess())
                return true;
            if (!resp.network_error && resp.status == 404)
                return false;
            return makeS3Error(resp, "HeadObject failed: ");
        });
}

Task<Result<S3ObjectStat, BackendError>> S3Client::statObject(std::string t_bucket, std::string t_key) {
    co_return co_await execute<S3ObjectStat>(
        [t_bucket = std::move(t_bucket), t_key = std::move(t_key)](NativeS3Client& t_client) -> Result<S3ObjectStat, BackendError> {
            auto resp = t_client.performRequest(drogon::Head, t_bucket, t_key, {}, {}, "", NativeS3Client::kDefaultMetaTimeoutSeconds);
            if (!resp.isSuccess())
                return makeS3Error(resp, "[S3Client] Head failed: ");

            S3ObjectStat stat;
            if (auto cl = resp.header("content-length"); !cl.empty()) {
                try {
                    stat.content_length = std::stoll(cl);
                } catch (...) {
                }
            }
            stat.content_type = resp.header("content-type");
            stat.etag = resp.header("etag");
            stat.last_modified = resp.header("last-modified");

            static constexpr std::string_view kMetaPrefix = "x-amz-meta-";
            for (const auto& [k, v] : resp.headers) {
                if (k.size() > kMetaPrefix.size() && k.rfind(kMetaPrefix, 0) == 0) {
                    stat.metadata[k.substr(kMetaPrefix.size())] = v;
                }
            }
            return stat;
        });
}

Task<Result<S3ListObjectsResult, BackendError>> S3Client::listObjects(
    std::string t_bucket, std::string t_prefix, std::string t_continuation_token, uint32_t t_max_keys) {
    co_return co_await execute<S3ListObjectsResult>(
        [t_bucket = std::move(t_bucket), t_prefix = std::move(t_prefix), t_continuation_token = std::move(t_continuation_token),
            t_max_keys = t_max_keys](NativeS3Client& t_client) -> Result<S3ListObjectsResult, BackendError> {
            std::vector<std::pair<std::string, std::string>> params = {{"list-type", "2"}};
            if (!t_prefix.empty())
                params.emplace_back("prefix", t_prefix);
            if (t_max_keys > 0)
                params.emplace_back("max-keys", std::to_string(t_max_keys));
            if (!t_continuation_token.empty())
                params.emplace_back("continuation-token", t_continuation_token);

            auto resp = t_client.performRequest(drogon::Get, t_bucket, "", params, {}, "", NativeS3Client::kDefaultMetaTimeoutSeconds);
            if (!resp.isSuccess())
                return makeS3Error(resp, "[S3Client] List objects failed: ");

            S3ListObjectsResult result;
            if (auto kc = sgrn::utils::xml::extractXmlTag(resp.body, "KeyCount"); !kc.empty()) {
                try {
                    result.key_count = std::stoll(kc);
                    if (result.key_count > 0)
                        result.objects.reserve(static_cast<size_t>(result.key_count));
                } catch (...) {
                }
            }
            sgrn::utils::xml::forEachXmlBlock(resp.body, "Contents", [&](std::string_view blk) {
                S3ObjectSummary obj;
                obj.key = sgrn::utils::xml::extractXmlTag(blk, "Key");
                if (auto s = sgrn::utils::xml::extractXmlTag(blk, "Size"); !s.empty()) {
                    try {
                        obj.size = std::stoll(s);
                    } catch (...) {
                    }
                }
                obj.etag = sgrn::utils::xml::extractXmlTag(blk, "ETag");
                obj.last_modified = sgrn::utils::xml::extractXmlTag(blk, "LastModified");
                result.objects.push_back(std::move(obj));
            });

            sgrn::utils::xml::forEachXmlBlock(resp.body, "CommonPrefixes", [&](std::string_view blk) {
                auto p = sgrn::utils::xml::extractXmlTag(blk, "Prefix");
                if (!p.empty())
                    result.common_prefixes.push_back(std::move(p));
            });

            result.is_truncated = (sgrn::utils::xml::extractXmlTag(resp.body, "IsTruncated") == "true");
            if (result.is_truncated)
                result.next_continuation_token = sgrn::utils::xml::extractXmlTag(resp.body, "NextContinuationToken");

            if (result.key_count == 0 && !result.objects.empty()) {
                result.key_count = static_cast<int64_t>(result.objects.size());
            }
            return result;
        });
}

Task<Result<void, BackendError>> S3Client::createBucket(std::string t_bucket) {
    co_return co_await execute<void>([t_bucket = std::move(t_bucket)](NativeS3Client& t_client) -> Result<void, BackendError> {
        auto resp = t_client.performRequest(drogon::Put, t_bucket, "", {}, {}, "", NativeS3Client::kDefaultMetaTimeoutSeconds);
        if (!resp.isSuccess())
            return makeS3Error(resp, "Create bucket failed: ");
        return {};
    });
}

Task<Result<void, BackendError>> S3Client::deleteBucket(std::string t_bucket) {
    co_return co_await execute<void>([t_bucket = std::move(t_bucket)](NativeS3Client& t_client) -> Result<void, BackendError> {
        auto resp = t_client.performRequest(drogon::Delete, t_bucket, "", {}, {}, "", NativeS3Client::kDefaultMetaTimeoutSeconds);
        if (!resp.isSuccess())
            return makeS3Error(resp, "Delete bucket failed: ");
        return {};
    });
}

Task<Result<bool, BackendError>> S3Client::bucketExists(std::string t_bucket) {
    co_return co_await execute<bool>([t_bucket = std::move(t_bucket)](NativeS3Client& t_client) -> Result<bool, BackendError> {
        auto resp = t_client.performRequest(drogon::Head, t_bucket, "", {}, {}, "", NativeS3Client::kDefaultMetaTimeoutSeconds);
        if (resp.isSuccess())
            return true;
        if (!resp.network_error && resp.status == 404)
            return false;
        return makeS3Error(resp, "HeadBucket failed: ");
    });
}

Task<Result<S3ListBucketsResult, BackendError>> S3Client::listBuckets() {
    co_return co_await execute<S3ListBucketsResult>([](NativeS3Client& t_client) -> Result<S3ListBucketsResult, BackendError> {
        auto resp = t_client.performRequest(drogon::Get, "", "", {}, {}, "", NativeS3Client::kDefaultMetaTimeoutSeconds);
        if (!resp.isSuccess())
            return makeS3Error(resp, "List buckets failed: ");

        S3ListBucketsResult result;
        sgrn::utils::xml::forEachXmlBlock(resp.body, "Bucket", [&](std::string_view blk) {
            result.buckets.push_back(S3BucketInfo{
                sgrn::utils::xml::extractXmlTag(blk, "Name"),
                sgrn::utils::xml::extractXmlTag(blk, "CreationDate"),
            });
        });
        return result;
    });
}

Task<Result<S3Tags, BackendError>> S3Client::getObjectTags(std::string t_bucket, std::string t_key) {
    co_return co_await execute<S3Tags>(
        [t_bucket = std::move(t_bucket), t_key = std::move(t_key)](NativeS3Client& t_client) -> Result<S3Tags, BackendError> {
            auto resp = t_client.performRequest(
                drogon::Get, t_bucket, t_key, {{"tagging", ""}}, {}, "", NativeS3Client::kDefaultMetaTimeoutSeconds);
            if (!resp.isSuccess())
                return makeS3Error(resp, "[S3Client] Get tags failed: ");

            S3Tags result;
            sgrn::utils::xml::forEachXmlBlock(resp.body, "Tag", [&](std::string_view blk) {
                result[sgrn::utils::xml::extractXmlTag(blk, "Key")] = sgrn::utils::xml::extractXmlTag(blk, "Value");
            });
            return result;
        });
}

Task<Result<void, BackendError>> S3Client::putObjectTags(std::string t_bucket, std::string t_key, S3Tags t_tags) {
    co_return co_await execute<void>([t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_tags = std::move(t_tags)](
                                         NativeS3Client& t_client) -> Result<void, BackendError> {
        std::string xml;
        xml.reserve(64 + t_tags.size() * 64);
        xml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Tagging><TagSet>";
        for (const auto& [key_name, val] : t_tags) {
            xml += "<Tag><Key>";
            xml += sgrn::utils::xml::xmlEscape(key_name);
            xml += "</Key><Value>";
            xml += sgrn::utils::xml::xmlEscape(val);
            xml += "</Value></Tag>";
        }
        xml += "</TagSet></Tagging>";

        std::map<std::string, std::string> headers;
        headers["content-type"] = "application/xml";
        headers["content-md5"] = md5Base64OrThrow(xml);

        auto resp = t_client.performRequest(
            drogon::Put, t_bucket, t_key, {{"tagging", ""}}, headers, std::move(xml), NativeS3Client::kDefaultMetaTimeoutSeconds);
        if (!resp.isSuccess())
            return makeS3Error(resp, "Put tags failed: ");
        return {};
    });
}

Task<Result<void, BackendError>> S3Client::deleteObjectTags(std::string t_bucket, std::string t_key) {
    co_return co_await execute<void>(
        [t_bucket = std::move(t_bucket), t_key = std::move(t_key)](NativeS3Client& t_client) -> Result<void, BackendError> {
            auto resp = t_client.performRequest(
                drogon::Delete, t_bucket, t_key, {{"tagging", ""}}, {}, "", NativeS3Client::kDefaultMetaTimeoutSeconds);
            if (!resp.isSuccess())
                return makeS3Error(resp, "Delete tags failed: ");
            return {};
        });
}

Task<Result<std::string, BackendError>> S3Client::createMultipartUpload(
    std::string t_bucket, std::string t_key, std::string t_content_type) {
    co_return co_await execute<std::string>(
        [t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_content_type = std::move(t_content_type)](
            NativeS3Client& t_client) -> Result<std::string, BackendError> {
            std::map<std::string, std::string> headers;
            if (!t_content_type.empty())
                headers["content-type"] = t_content_type;

            auto resp = t_client.performRequest(
                drogon::Post, t_bucket, t_key, {{"uploads", ""}}, headers, "", NativeS3Client::kDefaultMetaTimeoutSeconds);
            if (!resp.isSuccess())
                return makeS3Error(resp, "Create multipart upload failed: ");
            return sgrn::utils::xml::extractXmlTag(resp.body, "UploadId");
        });
}

Task<Result<std::string, BackendError>> S3Client::uploadPart(
    std::string t_bucket, std::string t_key, std::string t_upload_id, int t_part_number, std::string t_body) {
    co_return co_await execute<std::string>(
        [t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_upload_id = std::move(t_upload_id), t_part_number = t_part_number,
            t_data = std::move(t_body)](NativeS3Client& t_client) -> Result<std::string, BackendError> {
            std::vector<std::pair<std::string, std::string>> params = {
                {"partNumber", std::to_string(t_part_number)},
                {"uploadId", t_upload_id},
            };
            auto resp = t_client.performRequest(drogon::Put, t_bucket, t_key, params, {}, std::move(t_data));
            if (!resp.isSuccess())
                return makeS3Error(resp, "Upload part failed: ");
            return resp.header("etag");
        });
}

Task<Result<void, BackendError>> S3Client::completeMultipartUpload(
    std::string t_bucket, std::string t_key, std::string t_upload_id, std::vector<S3CompletedPart> t_parts) {
    co_return co_await execute<void>([t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_upload_id = std::move(t_upload_id),
                                         t_parts = std::move(t_parts)](NativeS3Client& t_client) -> Result<void, BackendError> {
        return completeMultipartSync(t_client, t_bucket, t_key, t_upload_id, t_parts);
    });
}

Task<Result<void, BackendError>> S3Client::abortMultipartUpload(std::string t_bucket, std::string t_key, std::string t_upload_id) {
    co_return co_await execute<void>([t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_upload_id = std::move(t_upload_id)](
                                         NativeS3Client& t_client) -> Result<void, BackendError> {
        auto resp = t_client.performRequest(
            drogon::Delete, t_bucket, t_key, {{"uploadId", t_upload_id}}, {}, "", NativeS3Client::kDefaultMetaTimeoutSeconds);
        if (!resp.isSuccess())
            return makeS3Error(resp, "Abort multipart upload failed: ");
        return {};
    });
}

Task<Result<int, BackendError>> S3Client::uploadFileMultipart(
    std::string t_bucket, std::string t_key, std::string t_content_type, std::filesystem::path t_file_path, size_t t_part_size_bytes) {
    co_return co_await execute<int>(
        [t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_content_type = std::move(t_content_type),
            t_file_path = std::move(t_file_path), t_part_size_bytes](NativeS3Client& t_client) -> Result<int, BackendError> {
            if (t_part_size_bytes == 0) {
                return BackendError(BackendErrorKind::Runtime, "Multipart part size must be greater than zero");
            }

            std::error_code ec;
            const auto file_size = std::filesystem::file_size(t_file_path, ec);
            if (ec) {
                return BackendError(BackendErrorKind::Filesystem, "Failed to stat file for multipart upload: " + ec.message());
            }
            if (file_size == 0) {
                return BackendError(BackendErrorKind::Filesystem, "Multipart upload produced zero parts (empty file)");
            }

            const uintmax_t expected_parts = (file_size + t_part_size_bytes - 1) / t_part_size_bytes;
            if (expected_parts > kMaxMultipartParts) {
                return BackendError(BackendErrorKind::Runtime,
                    fmt::format("Multipart upload requires {} parts, exceeding S3 limit of {}", expected_parts, kMaxMultipartParts));
            }

            std::ifstream in(t_file_path, std::ios::binary);
            if (!in) {
                return BackendError(BackendErrorKind::Filesystem, "Failed to open file for multipart upload");
            }

            std::map<std::string, std::string> init_headers;
            if (!t_content_type.empty())
                init_headers["content-type"] = t_content_type;

            auto create_resp = t_client.performRequest(
                drogon::Post, t_bucket, t_key, {{"uploads", ""}}, init_headers, "", NativeS3Client::kDefaultMetaTimeoutSeconds);
            if (!create_resp.isSuccess()) {
                return makeS3Error(create_resp, "Create multipart upload failed: ");
            }

            const std::string upload_id = sgrn::utils::xml::extractXmlTag(create_resp.body, "UploadId");
            if (upload_id.empty()) {
                return BackendError(BackendErrorKind::Garage, "Create multipart upload returned empty UploadId");
            }

            const size_t total_parts = static_cast<size_t>(expected_parts);
            const size_t concurrency = std::clamp<size_t>(std::min(t_client.ioThreadCount(), total_parts), 1, 4);

            std::vector<S3CompletedPart> parts(total_parts);
            std::atomic<bool> failed{false};
            std::mutex read_mutex;
            std::mutex err_mutex;
            std::optional<BackendError> first_error;
            int next_part_number = 1;
            std::atomic<size_t> uploaded_count{0};

            auto recordError = [&](BackendError err) {
                failed.store(true, std::memory_order_release);
                std::lock_guard lock(err_mutex);
                if (!first_error.has_value())
                    first_error = std::move(err);
            };

            auto uploadWorker = [&]() {
                try {
                    std::string buffer;
                    buffer.resize(t_part_size_bytes);
                    std::vector<std::pair<std::string, std::string>> params = {
                        {"partNumber", ""},
                        {"uploadId", upload_id},
                    };

                    while (!failed.load(std::memory_order_acquire)) {
                        int my_part = 0;
                        std::streamsize bytes_read = 0;
                        {
                            std::lock_guard lock(read_mutex);
                            if (failed.load(std::memory_order_relaxed) || !in)
                                break;
                            in.read(buffer.data(), static_cast<std::streamsize>(t_part_size_bytes));
                            bytes_read = in.gcount();
                            if (bytes_read <= 0)
                                break;
                            if (in.bad()) {
                                recordError(BackendError(BackendErrorKind::Filesystem, "I/O error reading file during multipart upload"));
                                break;
                            }
                            my_part = next_part_number++;
                        }

                        // Shrink buffer only on the final short part so performRequest sends exact length without extra copy.
                        buffer.resize(static_cast<size_t>(bytes_read));
                        params[0].second = std::to_string(my_part);
                        auto part_resp = t_client.performRequest(drogon::Put, t_bucket, t_key, params, {}, buffer);
                        buffer.resize(t_part_size_bytes);

                        if (!part_resp.isSuccess()) {
                            recordError(makeS3Error(part_resp, "Upload part failed: "));
                            break;
                        }

                        parts[static_cast<size_t>(my_part - 1)] = S3CompletedPart{my_part, part_resp.header("etag")};
                        uploaded_count.fetch_add(1, std::memory_order_relaxed);
                    }
                } catch (const std::exception& ex) {
                    recordError(BackendError(BackendErrorKind::Runtime, std::string("Exception in multipart upload worker: ") + ex.what()));
                } catch (...) {
                    recordError(BackendError(BackendErrorKind::Runtime, "Unknown exception in multipart upload worker"));
                }
            };

            try {
                if (concurrency <= 1) {
                    uploadWorker();
                } else {
                    std::vector<std::thread> workers;
                    workers.reserve(concurrency - 1);
                    for (size_t i = 0; i + 1 < concurrency; ++i) {
                        workers.emplace_back(uploadWorker);
                    }
                    uploadWorker();
                    for (auto& w : workers) {
                        if (w.joinable())
                            w.join();
                    }
                }

                if (first_error.has_value()) {
                    abortMultipartQuietly(t_client, t_bucket, t_key, upload_id);
                    return std::move(*first_error);
                }

                const size_t actual_parts = uploaded_count.load(std::memory_order_relaxed);
                if (actual_parts == 0) {
                    abortMultipartQuietly(t_client, t_bucket, t_key, upload_id);
                    return BackendError(BackendErrorKind::Filesystem, "Multipart upload produced zero parts (empty file?)");
                }
                parts.resize(actual_parts);

                auto fin_res = completeMultipartSync(t_client, t_bucket, t_key, upload_id, parts);
                if (fin_res.hasError()) {
                    abortMultipartQuietly(t_client, t_bucket, t_key, upload_id);
                    return fin_res.error();
                }
            } catch (...) {
                abortMultipartQuietly(t_client, t_bucket, t_key, upload_id);
                throw;
            }

            return static_cast<int>(parts.size());
        });
}

Task<Result<S3ListPartsResult, BackendError>> S3Client::listParts(
    std::string t_bucket, std::string t_key, std::string t_upload_id, uint32_t t_max_parts) {
    co_return co_await execute<S3ListPartsResult>(
        [t_bucket = std::move(t_bucket), t_key = std::move(t_key), t_upload_id = std::move(t_upload_id), t_max_parts = t_max_parts](
            NativeS3Client& t_client) -> Result<S3ListPartsResult, BackendError> {
            std::vector<std::pair<std::string, std::string>> params = {{"uploadId", t_upload_id}};
            if (t_max_parts > 0)
                params.emplace_back("max-parts", std::to_string(t_max_parts));

            auto resp = t_client.performRequest(drogon::Get, t_bucket, t_key, params, {}, "", NativeS3Client::kDefaultMetaTimeoutSeconds);
            if (!resp.isSuccess())
                return makeS3Error(resp, "List parts failed: ");

            S3ListPartsResult result;
            sgrn::utils::xml::forEachXmlBlock(resp.body, "Part", [&](std::string_view blk) {
                S3PartInfo p;
                try {
                    p.part_number = std::stoi(sgrn::utils::xml::extractXmlTag(blk, "PartNumber"));
                } catch (...) {
                }
                try {
                    p.size = std::stoll(sgrn::utils::xml::extractXmlTag(blk, "Size"));
                } catch (...) {
                }
                p.etag = sgrn::utils::xml::extractXmlTag(blk, "ETag");
                p.last_modified = sgrn::utils::xml::extractXmlTag(blk, "LastModified");
                result.parts.push_back(std::move(p));
            });
            result.is_truncated = (sgrn::utils::xml::extractXmlTag(resp.body, "IsTruncated") == "true");
            if (auto marker = sgrn::utils::xml::extractXmlTag(resp.body, "NextPartNumberMarker"); !marker.empty()) {
                try {
                    result.next_part_number_marker = std::stoi(marker);
                } catch (...) {
                }
            }
            return result;
        });
}

Task<Result<std::string, BackendError>> S3Client::generatePresignedUrl(
    std::string t_bucket, std::string t_key, uint32_t t_expiry_seconds, drogon::HttpMethod t_method) {
    std::shared_ptr<NativeS3Client> sp_pub_client;
    {
        std::lock_guard lock(client_mutex_);
        sp_pub_client = public_client_;
    }

    co_return co_await execute<std::string>(
        [sp_pub_client = std::move(sp_pub_client), t_bucket = std::move(t_bucket), t_key = std::move(t_key),
            t_expiry_seconds = t_expiry_seconds, t_method = t_method](NativeS3Client& t_client) -> Result<std::string, BackendError> {
            try {
                NativeS3Client* client_to_use = sp_pub_client ? sp_pub_client.get() : &t_client;
                return client_to_use->generatePresignedUrl(t_method, t_bucket, t_key, t_expiry_seconds);
            } catch (const std::exception& ex) {
                std::string error = "Generate presigned URL failed: " + std::string(ex.what());
                ERROR_LOG("{}", error);
                return toBackendError(S3Error::RequestFailed, error);
            }
        });
}

Task<Result<std::string, BackendError>> S3Client::presignedGetUrl(std::string t_bucket, std::string t_key, uint32_t t_expiry_seconds) {
    co_return co_await generatePresignedUrl(std::move(t_bucket), std::move(t_key), t_expiry_seconds, drogon::Get);
}

Task<Result<std::string, BackendError>> S3Client::presignedPutUrl(std::string t_bucket, std::string t_key, uint32_t t_expiry_seconds) {
    co_return co_await generatePresignedUrl(std::move(t_bucket), std::move(t_key), t_expiry_seconds, drogon::Put);
}

Task<Result<std::string, BackendError>> S3Client::presignedDeleteUrl(std::string t_bucket, std::string t_key, uint32_t t_expiry_seconds) {
    co_return co_await generatePresignedUrl(std::move(t_bucket), std::move(t_key), t_expiry_seconds, drogon::Delete);
}

} // namespace sgrn::datastore::plugins::aws

#undef DEBUG_LOG
#undef INFO_LOG
#undef WARN_LOG
#undef ERROR_LOG
