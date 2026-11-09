// path.cpp — remote path normalisation and recursive tree/du helpers.
#include "shell-common.hpp"
namespace sgrn::datastore::shell
{

// ── Path resolution & tiny helpers ───────────────────────────────────────────

std::vector<std::string> DatastoreShell::splitWords(const std::string& t_line) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false;
    for (char c : t_line) {
        if (c == '"') {
            in_quotes = !in_quotes;
        } else if (c == ' ' || c == '\t') {
            if (in_quotes) {
                cur += c;
            } else if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

std::string DatastoreShell::normalizePath(const std::string& t_arg) {
    std::string joined = (!t_arg.empty() && t_arg[0] == '/') ? t_arg : cwd_ + "/" + t_arg;
    std::vector<std::string> parts;
    std::string cur;
    for (char c : joined + "/") {
        if (c == '/') {
            if (cur.empty() || cur == ".") {
                // skip
            } else if (cur == "..") {
                if (!parts.empty())
                    parts.pop_back();
            } else {
                parts.push_back(cur);
            }
            cur.clear();
        } else {
            cur += c;
        }
    }
    std::string out = "/";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0)
            out += "/";
        out += parts[i];
    }

    // Canonical shell authority form:
    //   /file/path/name@organisation-domain.subdomain
    //   /file/path/name@organisation-user@mail-domain
    // Dots identify domain levels; slashes identify storage paths.  The
    // authenticated server remains authoritative for organisation membership.
    if (out.rfind("/file/", 0) == 0) {
        const size_t last_slash = out.find_last_of('/');
        const std::string file_name = out.substr(last_slash + 1);
        const size_t authority_at = file_name.find('@');
        if (authority_at != std::string::npos && authority_at + 1 < file_name.size()) {
            const std::string authority = file_name.substr(authority_at + 1);
            const size_t org_separator = authority.find('-');
            const std::string relative = last_slash > 6 ? out.substr(6, last_slash - 6) : "";
            const std::string relative_prefix = relative.empty() ? "" : "/" + relative;

            if (org_separator != std::string::npos && org_separator + 1 < authority.size()) {
                const std::string target = authority.substr(org_separator + 1);
                const std::string name = file_name.substr(0, authority_at);
                if (target.find('@') != std::string::npos) {
                    // organisation-user@mail-domain -> authenticated user's
                    // namespace. The API resolves the email within the
                    // authenticated organisation.
                    scope_ = sgrn::datastore::client::StorageScope::Users;
                    return "/users/" + target + relative_prefix + "/" + name;
                }

                // organisation-domain.subdomain -> domain namespace. Domain
                // hierarchy is represented by dots in the authority and
                // slashes in the API path.
                std::string domain_path = target;
                std::replace(domain_path.begin(), domain_path.end(), '.', '/');
                scope_ = sgrn::datastore::client::StorageScope::Domain;
                return "/domains/" + domain_path + relative_prefix + "/" + name;
            }
        }
    }
    return out;
}

std::string DatastoreShell::baseName(const std::string& t_path) {
    const size_t p = t_path.find_last_of('/');
    return (p == std::string::npos) ? t_path : t_path.substr(p + 1);
}

std::string DatastoreShell::dirName(const std::string& t_path) {
    if (t_path == "/")
        return "/";
    const size_t p = t_path.find_last_of('/');
    return (p == 0) ? "/" : t_path.substr(0, p);
}

std::string DatastoreShell::humanBytes(int64_t t_bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(t_bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    return fmt::format("{:.1f}{}", v, units[u]);
}

sgrn::Result<std::optional<DatastoreShell::Resolved>> DatastoreShell::resolve(const std::string& t_abs) {
    if (t_abs == "/")
        return std::nullopt;
    const std::string parent = dirName(t_abs);
    const std::string name = baseName(t_abs);
    if (!storage_)
        return std::nullopt;
    auto r = storage_->tryListDrive(parent, scope_);
    RETURN_IF(r.hasError(), fmt::format("{}: {}", parent, r.error()));
    for (const auto& f : r.value().folders) {
        if (f.name_ == name)
            return std::optional<Resolved>(Resolved{f.id_, sgrn::datastore::client::DriveItemType::Folder, true});
    }
    for (const auto& f : r.value().files) {
        if (f.name_ == name)
            return std::optional<Resolved>(
                Resolved{f.id_ != 0 ? std::optional<int64_t>(f.id_) : std::nullopt, sgrn::datastore::client::DriveItemType::File, false});
    }
    return std::nullopt;
}

sgrn::Result<void> DatastoreShell::requireId(const std::string& t_path, const Resolved& t_hit) {
    RETURN_ERROR_IF(
        !t_hit.id, fmt::format("'{}': namespace entry has no id and cannot be addressed (move/delete inside it instead)", t_path));
    return {};
}

sgrn::Result<uint64_t> DatastoreShell::duRecursive(const std::string& t_abs) {
    RETURN_ERROR_IF(!storage_, "not connected");
    auto r = storage_->tryListDrive(t_abs, scope_);
    RETURN_IF(r.hasError(), fmt::format("{}: {}", t_abs, r.error()));
    uint64_t total = 0;
    for (const auto& f : r.value().files)
        total += static_cast<uint64_t>(f.size);
    for (const auto& d : r.value().folders) {
        const std::string child = t_abs == "/" ? "/" + d.name_ : t_abs + "/" + d.name_;
        auto child_total = duRecursive(child);
        RETURN_IF(child_total.hasError(), child_total.error());
        total += child_total.value();
    }
    return total;
}

sgrn::Result<void> DatastoreShell::treeRecursive(const std::string& t_abs, const std::string& t_prefix) {
    RETURN_ERROR_IF(!storage_, "not connected");
    auto r = storage_->tryListDrive(t_abs, scope_);
    RETURN_IF(r.hasError(), fmt::format("{}: {}", t_abs, r.error()));
    const auto& folders = r.value().folders;
    const auto& files = r.value().files;
    for (size_t i = 0; i < folders.size() + files.size(); ++i) {
        const bool last = (i + 1 == folders.size() + files.size());
        const bool is_dir = i < folders.size();
        const std::string& name = is_dir ? folders[i].name_ : files[i - folders.size()].name_;
        if (is_dir)
            fmt::print(styleDir(), "{}{}── {}/\n", t_prefix, last ? "└" : "├", name);
        else
            fmt::print("{}{}── {}\n", t_prefix, last ? "└" : "├", name);
        if (is_dir) {
            const std::string child = t_abs == "/" ? "/" + name : t_abs + "/" + name;
            PROPAGATE_ERROR_IF(treeRecursive(child, t_prefix + (last ? "    " : "│   ")));
        }
    }
    return {};
}

} // namespace sgrn::datastore::shell
