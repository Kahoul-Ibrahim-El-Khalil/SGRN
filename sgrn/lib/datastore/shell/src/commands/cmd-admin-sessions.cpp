// cmd-admin-sessions.cpp — live session and webhook administration.
#include "../shell-common.hpp"
namespace sgrn::datastore::shell
{

// ── Live sessions & webhooks ─────────────────────────────────────────────────

sgrn::Result<void> DatastoreShell::cmdSessions() {
    RETURN_ERROR_IF(!client_, "not connected");
    auto r = client_->tryListActiveSessions();
    RETURN_IF(r.hasError(), r.error());
    std::vector<std::vector<std::string>> rows;
    rows.reserve(r.value().size());
    for (const auto& s : r.value()) {
        // A session belongs to either a user or an automated service.
        std::string id;
        if (s.user_id)
            id = std::to_string(*s.user_id);
        else if (s.automated_service_id)
            id = std::to_string(*s.automated_service_id);
        rows.push_back({id, s.actor_type, s.actor_name, s.ip, s.created_at, s.expires_at});
    }
    if (rows.empty()) {
        fmt::print("(no active sessions)\n");
    } else {
        printTable({"ID", "ACTOR", "NAME", "IP", "SINCE", "EXPIRES"}, std::move(rows), {0});
    }
    return {};
}

sgrn::Result<void> DatastoreShell::cmdWebhooks() {
    RETURN_ERROR_IF(!client_, "not connected");
    auto r = client_->tryListWebhooks();
    RETURN_IF(r.hasError(), r.error());
    std::vector<std::vector<std::string>> rows;
    rows.reserve(r.value().size());
    for (const auto& w : r.value()) {
        rows.push_back({std::to_string(w.id), w.url, w.organisation, w.is_active ? "active" : "inactive", w.created_at});
    }
    if (rows.empty()) {
        fmt::print("(no webhooks registered)\n");
    } else {
        printTable({"ID", "URL", "ORG", "STATUS", "CREATED"}, std::move(rows), {0});
    }
    return {};
}

sgrn::Result<void> DatastoreShell::cmdWebhookAdd(const std::vector<std::string>& t_args) {
    std::string url;
    std::string secret;
    for (size_t i = 0; i < t_args.size(); ++i) {
        const auto& a = t_args[i];
        if ((a == "--secret" || a == "-s") && i + 1 < t_args.size())
            secret = t_args[++i];
        else if (url.empty() && !a.empty() && a[0] != '-')
            url = a;
    }
    RETURN_ERROR_IF(url.empty(), "missing URL (webhook-add <URL> [--secret S])");
    RETURN_ERROR_IF(!client_, "not connected");
    auto r = client_->registerWebhookJson(url, secret);
    RETURN_IF(r.hasError(), r.error());
    printJsonPretty(r.value());
    return {};
}

sgrn::Result<void> DatastoreShell::cmdWebhookDel(const std::vector<std::string>& t_args) {
    RETURN_ERROR_IF(t_args.empty(), "missing webhook ID");
    int32_t id = 0;
    try {
        id = std::stoi(t_args[0]);
    } catch (...) {
        return ::sgrn::Error("ID must be an integer");
    }
    RETURN_ERROR_IF(!client_, "not connected");
    auto r = client_->deleteWebhookJson(id);
    RETURN_IF(r.hasError(), r.error());
    printJsonPretty(r.value());
    return {};
}

} // namespace sgrn::datastore::shell
