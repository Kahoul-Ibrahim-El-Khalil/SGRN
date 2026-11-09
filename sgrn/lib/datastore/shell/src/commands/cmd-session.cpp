// cmd-session.cpp — session summary and namespace scope commands.
#include "../shell-common.hpp"
namespace sgrn::datastore::shell
{

sgrn::Result<void> DatastoreShell::cmdSession() {
    fmt::print("{:<8} {}\n", "url:", url_.empty() ? "(not connected)" : url_);
    fmt::print("{:<8} {}\n", "auth:", connected() ? auth_desc_ : "-");
    fmt::print("{:<8} {}\n", "session:", connected() && client_->hasSessionToken() ? "active" : "none");
    fmt::print("{:<8} {}\n", "scope:", sgrn::datastore::client::detail::storageScopeToString(scope_));
    fmt::print("{:<8} {}\n", "cwd:", cwd_);
    return {};
}

sgrn::Result<void> DatastoreShell::cmdScope(const std::vector<std::string>& t_args) {
    if (t_args.empty()) {
        fmt::print("scope: {}\n", sgrn::datastore::client::detail::storageScopeToString(scope_));
        return {};
    }
    auto parsed = parseScope(t_args[0]);
    RETURN_ERROR_IF(!parsed, fmt::format("unknown scope '{}' (personal|users|automated-services|domain|auto)", t_args[0]));
    scope_ = *parsed;
    return {};
}

sgrn::Result<void> DatastoreShell::cmdSwitch(const std::vector<std::string>& t_args) {
    // `switch scope [NAME]` — switch the namespace scope; a missing or
    // empty NAME resets to the default (auto). Only `scope` is switchable
    // today; the subcommand slot keeps room for future targets.
    if (t_args.empty() || (t_args.size() == 1 && t_args[0] == "scope")) {
        scope_ = sgrn::datastore::client::StorageScope::Auto;
        fmt::print("scope: {}\n", sgrn::datastore::client::detail::storageScopeToString(scope_));
        return {};
    }
    RETURN_ERROR_IF(t_args.size() != 2 || t_args[0] != "scope", "usage: switch scope [personal|users|automated-services|domain|auto]");
    return cmdScope({t_args[1]});
}

} // namespace sgrn::datastore::shell
