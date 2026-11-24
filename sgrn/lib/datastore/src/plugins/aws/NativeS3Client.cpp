#include <sgrn/datastore/plugins/aws/NativeS3Client.hpp>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/hashing.hpp>
#include <sgrn/utils/strings.hpp>
#include <sgrn/utils/xml.hpp>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <random>
#include <stdexcept>
#include <thread>

namespace sgrn::datastore::plugins::aws
{

namespace
{

// Derive the daily SigV4 signing key: HMAC(HMAC(HMAC(HMAC("AWS4"+secret, date), region), "s3"), "aws4_request").
Result<std::string> deriveSigningKey(std::string_view secret_key, std::string_view date_stamp, std::string_view region) {
    std::string k_secret = "AWS4" + std::string(secret_key);
    auto k_date = sgrn::utils::computeHmacSha256(k_secret, date_stamp, sgrn::utils::HashEncoding::raw);
    RETURN_ERROR_IF(k_date.hasError(), k_date.error());
    auto k_region = sgrn::utils::computeHmacSha256(*k_date, region, sgrn::utils::HashEncoding::raw);
    RETURN_ERROR_IF(k_region.hasError(), k_region.error());
    auto k_service = sgrn::utils::computeHmacSha256(*k_region, "s3", sgrn::utils::HashEncoding::raw);
    RETURN_ERROR_IF(k_service.hasError(), k_service.error());
    return sgrn::utils::computeHmacSha256(*k_service, "aws4_request", sgrn::utils::HashEncoding::raw);
}

std::pair<std::string, std::string> currentUtcTimestamps() {
    std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char amz_buf[32]{};
    char date_buf[16]{};
    std::strftime(amz_buf, sizeof(amz_buf), "%Y%m%dT%H%M%SZ", &utc);
    std::strftime(date_buf, sizeof(date_buf), "%Y%m%d", &utc);
    return {std::string(amz_buf), std::string(date_buf)};
}

std::string httpMethodName(drogon::HttpMethod method) {
    switch (method) {
        case drogon::Get:
            return "GET";
        case drogon::Post:
            return "POST";
        case drogon::Put:
            return "PUT";
        case drogon::Delete:
            return "DELETE";
        case drogon::Head:
            return "HEAD";
        default:
            return "GET";
    }
}

// POST is non-idempotent on timeout (e.g. CreateMultipartUpload/CompleteMultipartUpload may have
// already executed server-side), so POST timeouts are not retried automatically.
bool isRetryable(drogon::HttpMethod method, const S3Response& r) {
    if (r.network_error)
        return !(r.is_timeout && method == drogon::Post);
    return r.status == 500 || r.status == 502 || r.status == 503 || r.status == 504;
}

// Strip default ports (:80 for http, :443 for https) so the signed Host header
// matches what Drogon sends on the wire.
std::string stripDefaultPort(std::string_view scheme, std::string_view authority) {
    if (scheme == "http" && authority.ends_with(":80"))
        return std::string(authority.substr(0, authority.size() - 3));
    if (scheme == "https" && authority.ends_with(":443"))
        return std::string(authority.substr(0, authority.size() - 4));
    return std::string(authority);
}

S3Response makeInternalErrorResponse(std::string message) {
    S3Response out;
    out.network_error = true;
    out.error_message = std::move(message);
    return out;
}

} // namespace

NativeS3Client::NativeS3Client(std::string endpoint, std::string region, std::string access_key, std::string secret_key, bool sign_only,
    size_t io_threads, bool unsigned_payload)
    : region_(std::move(region))
    , access_key_(std::move(access_key))
    , secret_key_(std::move(secret_key))
    , unsigned_payload_(unsigned_payload) {
    if (endpoint.empty())
        endpoint = "http://127.0.0.1:3900";
    if (endpoint.find("://") == std::string::npos)
        endpoint = "http://" + endpoint;
    while (endpoint.size() > 1 && endpoint.back() == '/')
        endpoint.pop_back();

    const size_t scheme_pos = endpoint.find("://");
    const std::string scheme = sgrn::utils::strings::toLower(std::string_view(endpoint).substr(0, scheme_pos));
    const std::string_view rest = std::string_view(endpoint).substr(scheme_pos + 3);
    const size_t slash_pos = rest.find('/');
    const std::string_view authority = (slash_pos != std::string_view::npos) ? rest.substr(0, slash_pos) : rest;

    path_prefix_ = (slash_pos != std::string_view::npos) ? std::string(rest.substr(slash_pos)) : std::string{};
    scheme_host_ = scheme + "://" + std::string(authority);
    host_port_ = stripDefaultPort(scheme, authority);

    if (!sign_only) {
        const size_t count = std::clamp<size_t>(io_threads, 1, 32);
        http_workers_.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            HttpWorker w;
            w.loop_thread = std::make_unique<trantor::EventLoopThread>("S3HttpClientLoop-" + std::to_string(i));
            w.loop_thread->run();
            w.client = drogon::HttpClient::newHttpClient(scheme_host_, w.loop_thread->getLoop(), false, true);
            http_workers_.push_back(std::move(w));
        }
    }
}

NativeS3Client::~NativeS3Client() {
    // Release HttpClients before stopping their event loops so pending callbacks drain safely.
    for (auto& w : http_workers_)
        w.client.reset();
    for (auto& w : http_workers_) {
        if (w.loop_thread) {
            w.loop_thread->getLoop()->quit();
            w.loop_thread->wait();
        }
    }
}

std::string NativeS3Client::getSigningKey(std::string_view date_stamp) const {
    std::lock_guard lock(signing_key_mutex_);
    if (cached_date_stamp_ == date_stamp && !cached_signing_key_.empty())
        return cached_signing_key_;
    auto derived = deriveSigningKey(secret_key_, date_stamp, region_);
    if (derived.hasError())
        return {};
    cached_signing_key_ = std::move(*derived);
    cached_date_stamp_.assign(date_stamp);
    return cached_signing_key_;
}

std::string NativeS3Client::buildCanonicalUri(std::string_view bucket, std::string_view key) const {
    if (bucket.empty())
        return path_prefix_.empty() ? std::string("/") : path_prefix_;
    std::string uri;
    uri.reserve(path_prefix_.size() + bucket.size() + key.size() + 16);
    uri += path_prefix_;
    uri.push_back('/');
    uri += sgrn::utils::encoding::uriEncode(bucket, true);
    if (!key.empty()) {
        uri.push_back('/');
        uri += sgrn::utils::encoding::uriEncode(key, false);
    }
    return uri;
}

std::string NativeS3Client::buildCanonicalQuery(const std::vector<std::pair<std::string, std::string>>& query_params) {
    if (query_params.empty())
        return {};
    std::vector<std::pair<std::string, std::string>> encoded;
    encoded.reserve(query_params.size());
    size_t total_len = query_params.size() * 2;
    for (const auto& [k, v] : query_params) {
        auto ek = sgrn::utils::encoding::uriEncode(k, true);
        auto ev = sgrn::utils::encoding::uriEncode(v, true);
        total_len += ek.size() + ev.size();
        encoded.emplace_back(std::move(ek), std::move(ev));
    }
    std::sort(encoded.begin(), encoded.end());
    std::string out;
    out.reserve(total_len);
    for (size_t i = 0; i < encoded.size(); ++i) {
        if (i > 0)
            out.push_back('&');
        out += encoded[i].first;
        out.push_back('=');
        out += encoded[i].second;
    }
    return out;
}

S3Response NativeS3Client::performRequest(drogon::HttpMethod method, std::string_view bucket, std::string_view key,
    const std::vector<std::pair<std::string, std::string>>& query_params, const std::map<std::string, std::string>& extra_headers,
    std::string body, double timeout_seconds) {

    static constexpr int kMaxAttempts = 4;
    static constexpr int kBaseDelayMs = 100;
    static constexpr int kMaxDelayMs = 2000;

    S3Response resp;
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        const bool may_retry = (attempt < kMaxAttempts);
        resp = performRequestOnce(method, bucket, key, query_params, extra_headers, body, may_retry, timeout_seconds);
        if (resp.isSuccess() || !may_retry || !isRetryable(method, resp))
            break;

        // Full-jitter exponential backoff avoids thundering herds on 503/SlowDown.
        const int exp_cap = std::min(kMaxDelayMs, kBaseDelayMs * (1 << (attempt - 1)));
        thread_local std::mt19937_64 rng{std::random_device{}()};
        std::uniform_int_distribution<int> dist(0, exp_cap);
        const int sleep_ms = dist(rng);
        if (sleep_ms > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    }
    return resp;
}

S3Response NativeS3Client::performRequestOnce(drogon::HttpMethod method, std::string_view bucket, std::string_view key,
    const std::vector<std::pair<std::string, std::string>>& query_params, const std::map<std::string, std::string>& extra_headers,
    std::string& body, bool preserve_body_for_retry, double timeout_seconds) {

    if (http_workers_.empty()) {
        return makeInternalErrorResponse("NativeS3Client initialized in sign-only mode");
    }

    const auto [amz_date, date_stamp] = currentUtcTimestamps();
    const std::string method_str = httpMethodName(method);
    const std::string canonical_uri = buildCanonicalUri(bucket, key);
    const std::string canonical_query = buildCanonicalQuery(query_params);

    std::string payload_hash;
    if (unsigned_payload_) {
        payload_hash = "UNSIGNED-PAYLOAD";
    } else {
        auto payload_hash_res = sgrn::utils::computeSha256Data(body, sgrn::utils::HashEncoding::hex);
        if (payload_hash_res.hasError())
            return makeInternalErrorResponse("SigV4 payload hash failed: " + payload_hash_res.error());
        payload_hash = std::move(*payload_hash_res);
    }

    std::map<std::string, std::string> signed_map;
    signed_map["host"] = host_port_;
    signed_map["x-amz-content-sha256"] = payload_hash;
    signed_map["x-amz-date"] = amz_date;
    for (const auto& [k, v] : extra_headers) {
        std::string lower_k = sgrn::utils::strings::toLower(k);
        if (lower_k.empty() || lower_k.find_first_of("\r\n:") != std::string::npos)
            continue;
        signed_map[std::move(lower_k)] = sgrn::utils::strings::collapseWhitespace(v);
    }

    size_t hdr_bytes = 0;
    size_t shdr_bytes = 0;
    for (const auto& [k, v] : signed_map) {
        hdr_bytes += k.size() + v.size() + 2;
        shdr_bytes += k.size() + 1;
    }

    std::string canonical_headers;
    canonical_headers.reserve(hdr_bytes);
    std::string signed_headers;
    signed_headers.reserve(shdr_bytes);
    for (const auto& [k, v] : signed_map) {
        canonical_headers += k;
        canonical_headers.push_back(':');
        canonical_headers += v;
        canonical_headers.push_back('\n');
        if (!signed_headers.empty())
            signed_headers.push_back(';');
        signed_headers += k;
    }

    std::string canonical_request;
    canonical_request.reserve(method_str.size() + canonical_uri.size() + canonical_query.size() + canonical_headers.size() +
                              signed_headers.size() + payload_hash.size() + 6);
    canonical_request += method_str;
    canonical_request.push_back('\n');
    canonical_request += canonical_uri;
    canonical_request.push_back('\n');
    canonical_request += canonical_query;
    canonical_request.push_back('\n');
    canonical_request += canonical_headers;
    canonical_request.push_back('\n');
    canonical_request += signed_headers;
    canonical_request.push_back('\n');
    canonical_request += payload_hash;

    std::string scope;
    scope.reserve(date_stamp.size() + region_.size() + 17);
    scope += date_stamp;
    scope.push_back('/');
    scope += region_;
    scope += "/s3/aws4_request";

    auto canonical_hash_res = sgrn::utils::computeSha256Data(canonical_request, sgrn::utils::HashEncoding::hex);
    if (canonical_hash_res.hasError())
        return makeInternalErrorResponse("SigV4 canonical hash failed: " + canonical_hash_res.error());
    const std::string& canonical_hash = *canonical_hash_res;

    std::string string_to_sign;
    string_to_sign.reserve(18 + amz_date.size() + scope.size() + canonical_hash.size() + 3);
    string_to_sign += "AWS4-HMAC-SHA256\n";
    string_to_sign += amz_date;
    string_to_sign.push_back('\n');
    string_to_sign += scope;
    string_to_sign.push_back('\n');
    string_to_sign += canonical_hash;

    const std::string signing_key = getSigningKey(date_stamp);
    if (signing_key.empty())
        return makeInternalErrorResponse("SigV4 signing key derivation failed");

    auto signature_res = sgrn::utils::computeHmacSha256(signing_key, string_to_sign, sgrn::utils::HashEncoding::hex);
    if (signature_res.hasError())
        return makeInternalErrorResponse("SigV4 HMAC signature failed: " + signature_res.error());
    const std::string& signature = *signature_res;

    std::string authorization;
    authorization.reserve(64 + access_key_.size() + scope.size() + signed_headers.size() + signature.size());
    authorization += "AWS4-HMAC-SHA256 Credential=";
    authorization += access_key_;
    authorization.push_back('/');
    authorization += scope;
    authorization += ", SignedHeaders=";
    authorization += signed_headers;
    authorization += ", Signature=";
    authorization += signature;

    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(method);
    req->setPathEncode(false);
    if (canonical_query.empty()) {
        req->setPath(canonical_uri);
    } else {
        std::string full_path;
        full_path.reserve(canonical_uri.size() + 1 + canonical_query.size());
        full_path += canonical_uri;
        full_path.push_back('?');
        full_path += canonical_query;
        req->setPath(std::move(full_path));
    }

    for (const auto& [k, v] : signed_map) {
        if (k == "host")
            continue; // Drogon populates Host from scheme_host_
        if (k == "content-type") {
            req->setContentTypeString(v);
        } else {
            req->addHeader(k, v);
        }
    }
    req->addHeader("authorization", authorization);

    // Move payload into Drogon to avoid duplicating multi-MB buffers on the common success path;
    // only restore it from req->body() if a retryable error occurs.
    const bool had_body = !body.empty();
    if (had_body) {
        req->setBody(std::move(body));
    }

    const size_t idx = next_worker_.fetch_add(1, std::memory_order_relaxed) % http_workers_.size();
    auto [res_code, resp] = http_workers_[idx].client->sendRequest(req, timeout_seconds);

    S3Response out;
    if (res_code != drogon::ReqResult::Ok || !resp) {
        out.network_error = true;
        out.is_timeout = (res_code == drogon::ReqResult::Timeout);
        out.error_message = std::string(drogon::to_string_view(res_code));
        if (had_body && preserve_body_for_retry && isRetryable(method, out)) {
            body.assign(req->body().data(), req->body().size());
        }
        return out;
    }

    out.raw_response = std::move(resp);
    out.status = static_cast<int>(out.raw_response->statusCode());
    out.body = out.raw_response->body();
    for (const auto& [k, v] : out.raw_response->headers()) {
        out.headers[sgrn::utils::strings::toLower(k)] = v;
    }

    // S3 can return HTTP 200 OK with an XML <Error> body on CompleteMultipartUpload / CopyObject.
    if (out.isSuccess() && (method == drogon::Post || method == drogon::Put) && out.hasXmlErrorBody()) {
        auto [err_code, err_msg] = extractXmlError(out.body);
        out.status = 500;
        if (!err_msg.empty())
            out.error_message = err_code.empty() ? err_msg : (err_code + ": " + err_msg);
        else
            out.error_message = "S3 returned 200 OK with XML Error body";
        if (had_body && preserve_body_for_retry && isRetryable(method, out)) {
            body.assign(req->body().data(), req->body().size());
        }
        return out;
    }

    if (!out.isSuccess()) {
        auto [err_code, err_msg] = extractXmlError(out.body);
        if (!err_msg.empty())
            out.error_message = err_code.empty() ? err_msg : (err_code + ": " + err_msg);
        else
            out.error_message = "HTTP " + std::to_string(out.status);
        if (had_body && preserve_body_for_retry && isRetryable(method, out)) {
            body.assign(req->body().data(), req->body().size());
        }
    }
    return out;
}

std::string NativeS3Client::generatePresignedUrl(
    drogon::HttpMethod method, std::string_view bucket, std::string_view key, uint32_t expiry_seconds) const {

    const uint32_t clamped_expiry = std::clamp<uint32_t>(expiry_seconds, 1U, kMaxPresignExpirySeconds);
    const auto [amz_date, date_stamp] = currentUtcTimestamps();
    const std::string method_str = httpMethodName(method);
    const std::string canonical_uri = buildCanonicalUri(bucket, key);

    std::string scope;
    scope.reserve(date_stamp.size() + region_.size() + 17);
    scope += date_stamp;
    scope.push_back('/');
    scope += region_;
    scope += "/s3/aws4_request";

    std::vector<std::pair<std::string, std::string>> params = {
        {"X-Amz-Algorithm", "AWS4-HMAC-SHA256"},
        {"X-Amz-Credential", access_key_ + "/" + scope},
        {"X-Amz-Date", amz_date},
        {"X-Amz-Expires", std::to_string(clamped_expiry)},
        {"X-Amz-SignedHeaders", "host"},
    };
    const std::string canonical_query = buildCanonicalQuery(params);

    std::string canonical_request;
    canonical_request.reserve(method_str.size() + canonical_uri.size() + canonical_query.size() + host_port_.size() + 32);
    canonical_request += method_str;
    canonical_request.push_back('\n');
    canonical_request += canonical_uri;
    canonical_request.push_back('\n');
    canonical_request += canonical_query;
    canonical_request += "\nhost:";
    canonical_request += host_port_;
    canonical_request += "\n\nhost\nUNSIGNED-PAYLOAD";

    auto canonical_hash_res = sgrn::utils::computeSha256Data(canonical_request, sgrn::utils::HashEncoding::hex);
    if (canonical_hash_res.hasError())
        throw std::runtime_error("SigV4 presign hash failed: " + canonical_hash_res.error());

    std::string string_to_sign;
    string_to_sign.reserve(18 + amz_date.size() + scope.size() + canonical_hash_res->size() + 3);
    string_to_sign += "AWS4-HMAC-SHA256\n";
    string_to_sign += amz_date;
    string_to_sign.push_back('\n');
    string_to_sign += scope;
    string_to_sign.push_back('\n');
    string_to_sign += *canonical_hash_res;

    const std::string signing_key = getSigningKey(date_stamp);
    if (signing_key.empty())
        throw std::runtime_error("SigV4 presign signing key derivation failed");

    auto signature_res = sgrn::utils::computeHmacSha256(signing_key, string_to_sign, sgrn::utils::HashEncoding::hex);
    if (signature_res.hasError())
        throw std::runtime_error("SigV4 presign HMAC failed: " + signature_res.error());

    std::string url;
    url.reserve(scheme_host_.size() + canonical_uri.size() + 1 + canonical_query.size() + 17 + signature_res->size());
    url += scheme_host_;
    url += canonical_uri;
    url.push_back('?');
    url += canonical_query;
    url += "&X-Amz-Signature=";
    url += *signature_res;
    return url;
}

std::pair<std::string, std::string> NativeS3Client::extractXmlError(std::string_view body) {
    return {sgrn::utils::xml::extractXmlTag(body, "Code"), sgrn::utils::xml::extractXmlTag(body, "Message")};
}

} // namespace sgrn::datastore::plugins::aws
