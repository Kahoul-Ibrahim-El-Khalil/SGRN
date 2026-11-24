#pragma once

#include <drogon/HttpClient.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <sgrn/datastore/plugins/aws/S3Error.hpp>
#include <sgrn/utils/strings.hpp>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <trantor/net/EventLoopThread.h>
#include <utility>
#include <vector>

namespace sgrn::datastore::plugins::aws
{

/**
 * Result of a single S3 REST call.
 *
 * Keeps the underlying Drogon HttpResponsePtr alive so `body` is a zero-copy
 * std::string_view directly into Drogon's receive buffer.
 */
struct S3Response {
    int status{0};
    drogon::HttpResponsePtr raw_response;
    std::string_view body;                      // Zero-copy view into raw_response->body()
    std::map<std::string, std::string> headers; // Lowercased header names
    bool network_error{false};
    bool is_timeout{false};
    std::string error_message;

    bool isSuccess() const noexcept {
        return !network_error && status >= 200 && status < 300;
    }

    /// True when S3/Garage returns HTTP 200 OK with an XML <Error> body
    /// (possible on CompleteMultipartUpload and CopyObject).
    bool hasXmlErrorBody() const noexcept {
        return body.find("<Error>") != std::string::npos || body.find("<Error ") != std::string::npos;
    }

    std::string header(std::string_view key) const {
        auto it = headers.find(sgrn::utils::strings::toLower(key));
        return it != headers.end() ? it->second : std::string{};
    }
};

/**
 * Lightweight native S3 REST client using Drogon HttpClient + OpenSSL SigV4.
 *
 * Replaces aws-sdk-cpp with zero shared-library dependencies.
 *
 * Concurrency model:
 *   - Allocates `io_threads` independent EventLoopThread + HttpClient pairs
 *     (one persistent keep-alive connection per worker) and dispatches requests
 *     round-robin so concurrent S3 workers do not serialize on a single loop.
 *   - `sign_only = true` skips creating any EventLoopThread/HttpClient (used
 *     for presign-only public endpoints).
 */
class NativeS3Client {
public:
    static constexpr double kDefaultDataTimeoutSeconds = 300.0;
    static constexpr double kDefaultMetaTimeoutSeconds = 15.0;
    static constexpr uint32_t kMaxPresignExpirySeconds = 604800; // 7 days (SigV4 limit)

    NativeS3Client(std::string endpoint, std::string region, std::string access_key, std::string secret_key, bool sign_only = false,
        size_t io_threads = 1, bool unsigned_payload = false);
    ~NativeS3Client();

    NativeS3Client(const NativeS3Client&) = delete;
    NativeS3Client& operator=(const NativeS3Client&) = delete;

    /// Execute an S3 request with automatic retries (up to 4 attempts with full-jitter backoff)
    /// on transient 5xx / network failures. Never throws for HTTP or OpenSSL signing errors.
    S3Response performRequest(drogon::HttpMethod method, std::string_view bucket, std::string_view key,
        const std::vector<std::pair<std::string, std::string>>& query_params = {},
        const std::map<std::string, std::string>& extra_headers = {}, std::string body = "",
        double timeout_seconds = kDefaultDataTimeoutSeconds);

    /// Generate a SigV4 presigned URL (expiry clamped to [1, 604800] seconds).
    std::string generatePresignedUrl(
        drogon::HttpMethod method, std::string_view bucket, std::string_view key, uint32_t expiry_seconds) const;

    size_t ioThreadCount() const noexcept {
        return http_workers_.size();
    }

    // ── Helpers ───────────────────────────────────────────────────────────────
    static std::pair<std::string, std::string> extractXmlError(std::string_view body);

private:
    struct HttpWorker {
        std::unique_ptr<trantor::EventLoopThread> loop_thread;
        drogon::HttpClientPtr client;
    };

    std::string scheme_host_; // e.g. "http://127.0.0.1:3900" (no path)
    std::string path_prefix_; // e.g. "" or "/s3"
    std::string host_port_;   // Host header value (default ports :80/:443 stripped)
    std::string region_;
    std::string access_key_;
    std::string secret_key_;
    bool unsigned_payload_{false};

    std::vector<HttpWorker> http_workers_;
    mutable std::atomic<size_t> next_worker_{0};

    // Per-day SigV4 signing key cache (avoids 4 HMAC-SHA256 derivations per request).
    mutable std::mutex signing_key_mutex_;
    mutable std::string cached_date_stamp_;
    mutable std::string cached_signing_key_;

    S3Response performRequestOnce(drogon::HttpMethod method, std::string_view bucket, std::string_view key,
        const std::vector<std::pair<std::string, std::string>>& query_params, const std::map<std::string, std::string>& extra_headers,
        std::string& body, bool preserve_body_for_retry, double timeout_seconds);

    std::string getSigningKey(std::string_view date_stamp) const;
    std::string buildCanonicalUri(std::string_view bucket, std::string_view key) const;
    static std::string buildCanonicalQuery(const std::vector<std::pair<std::string, std::string>>& query_params);
};

} // namespace sgrn::datastore::plugins::aws
