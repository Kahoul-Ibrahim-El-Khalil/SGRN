// cmd-admin-sessions.cpp — live session and webhook administration.
#include "../shell-common.hpp"
namespace sgrn::datastore::shell
{

// ── Live sessions & webhooks ─────────────────────────────────────────────────

sgrn::Result<void> DatastoreShell::cmdSessions() {
    RETURN_ERROR_IF(!client_, "not connected");
    auto r = client_->listActiveSessionsJson();
    RETURN_IF(r.hasError(), r.error());
    // Try to render as a table when response is a JSON array
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errs;
    const std::string& raw = r.value();
    if (reader->parse(raw.data(), raw.data() + raw.size(), &root, &errs) && root.isArray()) {
        std::vector<std::vector<std::string>> rows;
        for (const auto& s : root) {
            rows.push_back({
                s.get("user_id", "").asString(),
                s.get("email", "").asString(),
                s.get("domain", "").asString(),
                s.get("auth_mode", "").asString(),
                s.get("ip", s.get("remote_ip", "")).asString(),
                s.get("created_at", s.get("logged_in_at", "")).asString(),
            });
        }
        if (rows.empty()) {
            fmt::print("(no active sessions)\n");
        } else {
            printTable({"USER ID", "EMAIL", "DOMAIN", "AUTH", "IP", "SINCE"}, std::move(rows), {0});
        }
    } else {
        printJsonPretty(raw);
    }
    return {};
}

sgrn::Result<void> DatastoreShell::cmdWebhooks() {
    RETURN_ERROR_IF(!client_, "not connected");
    auto r = client_->listWebhooksJson();
    RETURN_IF(r.hasError(), r.error());
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errs;
    const std::string& raw = r.value();
    if (reader->parse(raw.data(), raw.data() + raw.size(), &root, &errs) && root.isArray()) {
        std::vector<std::vector<std::string>> rows;
        for (const auto& w : root) {
            rows.push_back({
                w.get("id", "").asString(),
                w.get("url", "").asString(),
                w.get("organisation", "").asString(),
                w.get("is_active", false).asBool() ? "active" : "inactive",
                w.get("created_at", "").asString(),
            });
        }
        if (rows.empty()) {
            fmt::print("(no webhooks registered)\n");
        } else {
            printTable({"ID", "URL", "ORG", "STATUS", "CREATED"}, std::move(rows), {0});
        }
    } else {
        printJsonPretty(raw);
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
