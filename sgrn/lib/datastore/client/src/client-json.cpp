// client-json.cpp — converters between the wire/mirror structs declared in
// client-json.hpp and the public domain structs of the datastore client.

#include "client-json.hpp"

namespace sgrn::datastore::client::detail
{

StoragePurgeResult purgeResultFromJson(const PurgeDryRunJson& t_w) {
    StoragePurgeResult r;
    r.dry_run = true;
    r.affected.reserve(t_w.would_delete.size());
    for (const auto& k : t_w.would_delete) {
        r.affected.push_back(k.key);
    }
    r.skipped.reserve(t_w.skipped.size());
    for (const auto& s : t_w.skipped) {
        r.skipped.push_back(s.reason.empty() ? s.key : s.key + ": " + s.reason);
    }
    return r;
}

StoragePurgeResult purgeResultFromJson(const PurgeResponseJson& t_w) {
    StoragePurgeResult r;
    r.dry_run = false;
    r.affected = t_w.deleted;
    if (t_w.errors && t_w.errors->is_array()) {
        for (const auto& e : t_w.errors->get_array()) {
            if (e.is_string()) {
                r.errors.push_back(e.get_string());
            } else if (auto text = sgrn::json::write(e); !text.hasError()) {
                r.errors.push_back(std::move(text).value());
            }
        }
    }
    r.skipped.reserve(t_w.skipped.size());
    for (const auto& s : t_w.skipped) {
        r.skipped.push_back(s.reason.empty() ? s.key : s.key + ": " + s.reason);
    }
    return r;
}

DatastoreClient::ResumableUploadSession sessionFromJson(const ResumableUploadSessionJson& t_w) {
    DatastoreClient::ResumableUploadSession s;
    s.upload_id = t_w.upload_id;
    if (t_w.chunk_size) {
        s.chunk_size = *t_w.chunk_size;
    }
    s.total_chunks = t_w.total_chunks;
    s.status = t_w.status;
    s.is_encrypted = t_w.is_encrypted;
    return s;
}

DatastoreClient::ResumableUploadStatus uploadStatusFromJson(const ResumableUploadStatusJson& t_w) {
    DatastoreClient::ResumableUploadStatus s;
    s.upload_id = t_w.upload_id;
    s.filename = t_w.filename;
    s.target_path = t_w.target_path;
    s.total_size = t_w.total_size;
    s.chunk_size = t_w.chunk_size;
    s.total_chunks = t_w.total_chunks;
    s.uploaded_chunks_count = t_w.uploaded_chunks_count;
    s.status = t_w.status;
    s.uploaded_chunk_indices = t_w.uploaded_chunk_indices;
    return s;
}

} // namespace sgrn::datastore::client::detail
