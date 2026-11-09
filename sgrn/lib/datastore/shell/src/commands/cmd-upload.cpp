// cmd-upload.cpp — resumable upload commands.
#include "../shell-common.hpp"
namespace sgrn::datastore::shell
{

// ── Resumable upload ─────────────────────────────────────────────────────────

sgrn::Result<void> DatastoreShell::cmdRput(const std::vector<std::string>& t_args) {
    // Usage: rput <local> [remote] [--chunk-size BYTES] [--encrypt]
    RETURN_ERROR_IF(t_args.empty(), "missing local path (rput <local> [remote] [--chunk-size BYTES] [--encrypt])");
    RETURN_ERROR_IF(!client_, "not connected");

    std::string local;
    std::string remote;
    int64_t chunk_size = 5 * 1024 * 1024; // 5 MB default
    bool encrypt = false;
    for (size_t i = 0; i < t_args.size(); ++i) {
        const auto& a = t_args[i];
        if (a == "--encrypt" || a == "-e") {
            encrypt = true;
        } else if ((a == "--chunk-size" || a == "-c") && i + 1 < t_args.size()) {
            try {
                chunk_size = std::stoll(t_args[++i]);
            } catch (...) {
                return ::sgrn::Error("invalid chunk-size");
            }
        } else if (local.empty()) {
            local = expandLocalPath(a);
        } else if (remote.empty()) {
            remote = normalizePath(a);
        }
    }
    RETURN_ERROR_IF(local.empty(), "missing local file");
    if (remote.empty())
        remote = normalizePath(baseName(local));

    // Resolve destination: if remote is an existing directory append filename
    auto hit = resolve(remote);
    RETURN_IF(hit.hasError(), hit.error());
    if (hit.value() && hit.value()->is_dir)
        remote = normalizePath(remote + "/" + baseName(local));

    std::ifstream ifs(local, std::ios::binary | std::ios::ate);
    RETURN_ERROR_IF(!ifs, fmt::format("cannot open '{}'", local));
    const int64_t total_size = static_cast<int64_t>(ifs.tellg());
    ifs.seekg(0);

    // Determine MIME type from extension (best-effort)
    std::string mime = "application/octet-stream";
    const std::string ext = std::filesystem::path(local).extension().string();
    if (ext == ".json")
        mime = "application/json";
    else if (ext == ".txt")
        mime = "text/plain";
    else if (ext == ".html" || ext == ".htm")
        mime = "text/html";
    else if (ext == ".png")
        mime = "image/png";
    else if (ext == ".jpg" || ext == ".jpeg")
        mime = "image/jpeg";
    else if (ext == ".pdf")
        mime = "application/pdf";

    auto sess_res = client_->initUploadSession(baseName(local), remote, total_size, chunk_size, mime, encrypt);
    RETURN_ERROR_IF(sess_res.hasError(), fmt::format("init failed: {}", sess_res.error()));
    const auto& sess = sess_res.value();
    fmt::print("rput: session {} ({} chunks of {} bytes, encrypted: {})\n", sess.upload_id, sess.total_chunks, humanBytes(sess.chunk_size),
        sess.is_encrypted ? "yes" : "no");

    std::vector<char> buf(static_cast<size_t>(chunk_size));
    for (int32_t idx = 0; idx < sess.total_chunks; ++idx) {
        ifs.read(buf.data(), static_cast<std::streamsize>(chunk_size));
        const std::streamsize bytes_read = ifs.gcount();
        RETURN_ERROR_IF(bytes_read <= 0, fmt::format("short read at chunk {} — session {} left open, use 'upload-abort {}' to cancel", idx,
                                             sess.upload_id, sess.upload_id));
        auto chunk_res = client_->uploadChunk(sess.upload_id, idx, buf.data(), static_cast<size_t>(bytes_read));
        RETURN_ERROR_IF(chunk_res.hasError(), fmt::format("chunk {} failed: {} — session {} left open, use 'upload-abort {}' to cancel",
                                                  idx, chunk_res.error(), sess.upload_id, sess.upload_id));
        // Inline progress bar (only when stdout is a tty)
#ifndef _WIN32
        if (isatty(STDOUT_FILENO)) {
            const int pct = static_cast<int>((idx + 1) * 100 / sess.total_chunks);
            const int filled = pct / 5;
            std::string bar = std::string(static_cast<size_t>(filled), '#') + std::string(static_cast<size_t>(20 - filled), '-');
            fmt::print("\r[{}] {}% ({}/{})  ", bar, pct, idx + 1, sess.total_chunks);
            fflush(stdout);
        }
#endif
    }
#ifndef _WIN32
    if (isatty(STDOUT_FILENO))
        fmt::print("\n");
#endif

    auto done_res = client_->completeUploadSession(sess.upload_id);
    RETURN_ERROR_IF(done_res.hasError(), fmt::format("complete failed: {}", done_res.error()));
    fmt::print(styleOk(), "rput: {} -> {} ({}): {}\n", local, remote, humanBytes(total_size), done_res.value());
    return {};
}

sgrn::Result<void> DatastoreShell::cmdUploadStatus(const std::vector<std::string>& t_args) {
    RETURN_ERROR_IF(t_args.empty(), "missing upload_id");
    RETURN_ERROR_IF(!client_, "not connected");
    auto r = client_->getUploadStatus(t_args[0]);
    RETURN_IF(r.hasError(), r.error());
    const auto& s = r.value();
    fmt::print("id:       {}\n", s.upload_id);
    fmt::print("file:     {}\n", s.filename);
    fmt::print("target:   {}\n", s.target_path);
    fmt::print("size:     {}\n", humanBytes(s.total_size));
    fmt::print("chunks:   {}/{}\n", s.uploaded_chunks_count, s.total_chunks);
    fmt::print("status:   {}\n", s.status);
    if (!s.uploaded_chunk_indices.empty()) {
        fmt::print("received: ");
        for (size_t i = 0; i < s.uploaded_chunk_indices.size(); ++i) {
            if (i)
                fmt::print(",");
            fmt::print("{}", s.uploaded_chunk_indices[i]);
        }
        fmt::print("\n");
    }
    return {};
}

sgrn::Result<void> DatastoreShell::cmdUploadAbort(const std::vector<std::string>& t_args) {
    RETURN_ERROR_IF(t_args.empty(), "missing upload_id");
    RETURN_ERROR_IF(!client_, "not connected");
    auto r = client_->abortUploadSession(t_args[0]);
    RETURN_IF(r.hasError(), r.error());
    fmt::print("{}\n", r.value());
    return {};
}

} // namespace sgrn::datastore::shell
