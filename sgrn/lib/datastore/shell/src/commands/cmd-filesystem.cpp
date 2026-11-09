// cmd-filesystem.cpp — remote filesystem navigation and transfer commands.
#include "../shell-common.hpp"
namespace sgrn::datastore::shell
{

sgrn::Result<void> DatastoreShell::cmdLs(const std::vector<std::string>& t_args) {
    const std::string path = t_args.empty() ? cwd_ : normalizePath(t_args[0]);
    RETURN_ERROR_IF(!storage_, "not connected");
    auto r = storage_->tryListDrive(path, scope_);
    RETURN_IF(r.hasError(), fmt::format("{}: {}", path, r.error()));
    // Align the size column; directories render cyan with a trailing slash.
    size_t name_width = 0;
    for (const auto& d : r.value().folders)
        name_width = std::max(name_width, displayWidth(d.name_));
    for (const auto& f : r.value().files)
        name_width = std::max(name_width, displayWidth(f.name_));
    for (const auto& d : r.value().folders)
        fmt::print(styleDir(), "{}/\n", padCell(d.name_, name_width, false));
    for (const auto& f : r.value().files) {
        if (f.is_encrypted) {
            fmt::print("{}  {}  [enc]\n", padCell(f.name_, name_width, false), humanBytes(f.size));
        } else {
            fmt::print("{}  {}\n", padCell(f.name_, name_width, false), humanBytes(f.size));
        }
    }
    return {};
}

sgrn::Result<void> DatastoreShell::cmdCd(const std::vector<std::string>& t_args) {
    if (t_args.empty()) {
        cwd_ = "/";
        return {};
    }
    const std::string path = normalizePath(t_args[0]);
    if (path == "/") {
        cwd_ = "/";
        return {};
    }
    auto hit = resolve(path);
    RETURN_IF(hit.hasError(), hit.error());
    RETURN_ERROR_IF(!hit.value() || !hit.value()->is_dir, fmt::format("{}: No such directory", path));
    cwd_ = path;
    return {};
}

sgrn::Result<void> DatastoreShell::cmdCat(const std::vector<std::string>& t_args) {
    RETURN_ERROR_IF(t_args.empty(), "missing file operand");
    const std::string path = normalizePath(t_args[0]);
    RETURN_ERROR_IF(!client_, "not connected");
    auto r = client_->doDownload(path, scope_);
    RETURN_ERROR_IF(!r.ok, fmt::format("{}: {}", path, r.message.empty() ? "download failed" : r.message));
    fmt::print("{}", r.bytes);
    return {};
}

sgrn::Result<void> DatastoreShell::cmdGet(const std::vector<std::string>& t_args) {
    RETURN_ERROR_IF(t_args.empty(), "missing remote path (get <remote> [local])");
    const std::string remote = normalizePath(t_args[0]);
    std::string local = (t_args.size() > 1) ? expandLocalPath(t_args[1]) : baseName(remote);
    if (std::filesystem::is_directory(local)) {
        local = (std::filesystem::path(local) / baseName(remote)).string();
    }
    RETURN_ERROR_IF(!storage_, "not connected");
    const auto outcome = storage_->download(remote, local, scope_);
    RETURN_ERROR_IF(!outcome.ok, fmt::format("{}: {}", remote, outcome.detail));
    return {};
}

sgrn::Result<void> DatastoreShell::cmdPut(const std::vector<std::string>& t_args) {
    RETURN_ERROR_IF(t_args.empty(), "missing local path (put <local> [remote] [--encrypt])");
    for (const auto& a : t_args) {
        if (a == "--encrypt" || a == "-e")
            return cmdRput(t_args);
    }
    const std::string local = expandLocalPath(t_args[0]);
    std::string remote = (t_args.size() > 1) ? normalizePath(t_args[1]) : normalizePath(baseName(local));
    if (remote == "/" || (t_args.size() > 1 && (t_args[1] == "." || t_args[1] == ".." || t_args[1].back() == '/'))) {
        remote = normalizePath(remote + "/" + baseName(local));
    } else {
        auto hit = resolve(remote);
        RETURN_IF(hit.hasError(), hit.error());
        if (hit.value() && hit.value()->is_dir)
            remote = normalizePath(remote + "/" + baseName(local));
    }
    RETURN_ERROR_IF(!storage_, "not connected");
    const auto outcome = storage_->upload(remote, local, scope_);
    RETURN_ERROR_IF(!outcome.ok, fmt::format("{} -> {}: {}", local, remote, outcome.detail));
    return {};
}

sgrn::Result<void> DatastoreShell::cmdMkdir(const std::vector<std::string>& t_args) {
    RETURN_ERROR_IF(t_args.empty(), "missing operand");
    const std::string path = normalizePath(t_args[0]);
    RETURN_ERROR_IF(!storage_ || !storage_->createDirectory(path, scope_), fmt::format("cannot create directory '{}'", path));
    return {};
}

sgrn::Result<void> DatastoreShell::cmdMv(const std::vector<std::string>& t_args) {
    RETURN_ERROR_IF(t_args.size() < 2, "missing operand (mv <src> <dst>)");
    const std::string src = normalizePath(t_args[0]);
    const std::string dst = normalizePath(t_args[1]);
    auto hit = resolve(src);
    RETURN_IF(hit.hasError(), hit.error());
    RETURN_ERROR_IF(!hit.value(), fmt::format("{}: No such file or directory", src));
    const Resolved& src_hit = *hit.value();
    PROPAGATE_ERROR_IF(requireId(src, src_hit));
    std::string parent;
    std::string new_name;
    auto dst_hit = resolve(dst);
    RETURN_IF(dst_hit.hasError(), dst_hit.error());
    if (dst_hit.value() && dst_hit.value()->is_dir) {
        parent = dst;
        new_name = baseName(src);
    } else {
        parent = dirName(dst);
        new_name = baseName(dst);
    }
    std::optional<int64_t> parent_id;
    if (parent != "/") {
        auto parent_hit = resolve(parent);
        RETURN_IF(parent_hit.hasError(), parent_hit.error());
        RETURN_ERROR_IF(!parent_hit.value() || !parent_hit.value()->is_dir, fmt::format("{}: No such directory", parent));
        parent_id = parent_hit.value()->id;
    }
    RETURN_ERROR_IF(!storage_ || !storage_->moveItem(*src_hit.id, src_hit.type, new_name, parent_id, scope_),
        fmt::format("{} -> {}: failed", src, dst));
    return {};
}

sgrn::Result<void> DatastoreShell::cmdRm(const std::vector<std::string>& t_args) {
    bool recursive = false;
    std::vector<std::string> paths;
    for (const auto& a : t_args) {
        if (a == "-r" || a == "-R" || a == "--recursive")
            recursive = true;
        else
            paths.push_back(a);
    }
    RETURN_ERROR_IF(paths.empty(), "missing operand (rm [-r] <path>)");
    std::vector<std::string> errors;
    for (const auto& p : paths) {
        const std::string path = normalizePath(p);
        if (path == "/") {
            errors.push_back("refusing to remove root");
            continue;
        }
        auto hit = resolve(path);
        if (hit.hasError()) {
            errors.push_back(hit.error());
            continue;
        }
        if (!hit.value()) {
            errors.push_back(fmt::format("{}: No such file or directory", path));
            continue;
        }
        const Resolved& entry = *hit.value();
        if (entry.is_dir && !recursive) {
            errors.push_back(fmt::format("{}: is a directory (use -r)", path));
            continue;
        }
        auto id_check = requireId(path, entry);
        if (id_check.hasError()) {
            errors.push_back(id_check.error());
            continue;
        }
        if (!storage_ || !storage_->deleteItem(*entry.id, entry.type, scope_))
            errors.push_back(fmt::format("{}: failed", path));
    }
    if (!errors.empty()) {
        std::string joined;
        for (size_t i = 0; i < errors.size(); ++i) {
            if (i > 0)
                joined += "\n";
            joined += errors[i];
        }
        return ::sgrn::Error(std::move(joined));
    }
    return {};
}

sgrn::Result<void> DatastoreShell::cmdTree(const std::vector<std::string>& t_args) {
    const std::string path = t_args.empty() ? cwd_ : normalizePath(t_args[0]);
    fmt::print("{}\n", path);
    return treeRecursive(path, "");
}

sgrn::Result<void> DatastoreShell::cmdDu(const std::vector<std::string>& t_args) {
    const std::string path = t_args.empty() ? cwd_ : normalizePath(t_args[0]);
    auto total = duRecursive(path);
    RETURN_IF(total.hasError(), total.error());
    fmt::print("{} {}\n", humanBytes(static_cast<int64_t>(total.value())), path);
    return {};
}

sgrn::Result<void> DatastoreShell::cmdZip(const std::vector<std::string>& t_args) {
    RETURN_ERROR_IF(t_args.empty(), "missing remote path (zip <remote-dir> [local.zip])");
    const std::string remote = normalizePath(t_args[0]);
    const std::string local = (t_args.size() > 1) ? expandLocalPath(t_args[1]) : baseName(remote) + ".zip";
    RETURN_ERROR_IF(!storage_, "not connected");
    const auto outcome = storage_->downloadZip(remote, local, scope_);
    RETURN_ERROR_IF(!outcome.ok, fmt::format("{}: {}", remote, outcome.detail));
    return {};
}

} // namespace sgrn::datastore::shell
