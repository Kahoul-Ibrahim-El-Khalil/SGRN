#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sgrn::datastore::plugins::aws
{

/// Glaze-reflectable metadata map (user-defined x-amz-meta-* headers).
using S3Metadata = std::map<std::string, std::string>;

/// Glaze-reflectable object tag map.
using S3Tags = std::map<std::string, std::string>;

struct S3BucketInfo {
    std::string name;
    std::string creation_date;
};

struct S3ListBucketsResult {
    std::vector<S3BucketInfo> buckets;
};

struct S3ObjectSummary {
    std::string key;
    int64_t size{0};
    std::string last_modified;
    std::string etag;
};

struct S3ListObjectsResult {
    std::vector<S3ObjectSummary> objects;
    std::vector<std::string> common_prefixes;
    bool is_truncated{false};
    std::string next_continuation_token;
    int64_t key_count{0};
};

struct S3ObjectStat {
    int64_t content_length{0};
    std::string content_type;
    std::string etag;
    std::string last_modified;
    S3Metadata metadata;
};

struct S3DeleteError {
    std::string key;
    std::string code;
    std::string message;
};

struct S3DeleteResult {
    std::vector<std::string> deleted;
    std::vector<S3DeleteError> errors;
};

struct S3CompletedPart {
    int part_number{0};
    std::string etag;
};

struct S3PartInfo {
    int part_number{0};
    std::string etag;
    int64_t size{0};
    std::string last_modified;
};

struct S3ListPartsResult {
    std::vector<S3PartInfo> parts;
    bool is_truncated{false};
    int next_part_number_marker{0};
};

} // namespace sgrn::datastore::plugins::aws
