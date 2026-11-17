// cmd-admin-accounts.cpp — organisation, user and service administration.
#include "../shell-common.hpp"
namespace sgrn::datastore::shell
{

Result<void> DatastoreShell::cmdOrgs() {
    RETURN_ERROR_IF(!client_, "not connected");
    auto res = client_->tryListOrganisations();
    RETURN_IF(res.hasError(), res.error());
    std::vector<std::vector<std::string>> rows;
    for (const auto& o : res.value())
        rows.push_back({o.id, o.name});
    printTable({"ID", "NAME"}, std::move(rows), {0});
    return {};
}

Result<void> DatastoreShell::cmdDomains(const std::vector<std::string>& t_args) {
    RETURN_ERROR_IF(!t_args.empty(), "takes no arguments (scope comes from your session)");
    RETURN_ERROR_IF(!client_, "not connected");
    auto res = client_->tryListDomains();
    RETURN_IF(res.hasError(), res.error());
    std::vector<std::vector<std::string>> rows;
    for (const auto& d : res.value())
        rows.push_back({d.id, d.name});
    printTable({"ID", "NAME"}, std::move(rows), {0});
    return {};
}

Result<void> DatastoreShell::cmdStatuses(const std::vector<std::string>& t_args) {
    RETURN_ERROR_IF(t_args.empty(), "missing organisation (statuses <org>)");
    RETURN_ERROR_IF(!client_, "not connected");
    auto res = client_->tryListStatuses(t_args[0]);
    RETURN_IF(res.hasError(), res.error());
    std::vector<std::vector<std::string>> rows;
    for (const auto& s : res.value())
        rows.push_back({s.id, s.name});
    printTable({"ID", "NAME"}, std::move(rows), {0});
    return {};
}

Result<void> DatastoreShell::cmdUsers() {
    RETURN_ERROR_IF(!client_, "not connected");
    auto res = client_->tryListUsers();
    RETURN_IF(res.hasError(), res.error());
    std::vector<std::vector<std::string>> rows;
    for (const auto& u : res.value())
        rows.push_back({std::to_string(u.id), u.email, u.first_name + " " + u.family_name, u.status, u.domain});
    printTable({"ID", "EMAIL", "NAME", "STATUS", "DOMAIN"}, std::move(rows), {0});
    return {};
}

Result<void> DatastoreShell::cmdUseradd(const std::vector<std::string>& t_args) {
    sgrn::datastore::client::NewUser u;
    for (size_t i = 0; i < t_args.size(); ++i) {
        const std::string& a = t_args[i];
        if (a.rfind("--", 0) == 0) {
            bool known = false;
            for (const char* name :
                {"--first", "--family", "--email", "--password", "--phone", "--org", "--organisation", "--status", "--domain"}) {
                const std::string dash = name, eq = dash + "=";
                std::string v;
                if (a == dash && i + 1 < t_args.size()) {
                    v = t_args[++i];
                    known = true;
                } else if (a.rfind(eq, 0) == 0) {
                    v = a.substr(eq.size());
                    known = true;
                }
                if (!known)
                    continue;
                const std::string key = std::string(name + 2);
                if (key == "first")
                    u.first_name = v;
                else if (key == "family")
                    u.family_name = v;
                else if (key == "email")
                    u.email = v;
                else if (key == "password")
                    u.password = v;
                else if (key == "phone")
                    u.phone_number = v;
                else if (key == "org" || key == "organisation")
                    u.organisation = v;
                else if (key == "status")
                    u.status = v;
                else if (key == "domain")
                    u.domain = v;
                break;
            }
            RETURN_ERROR_IF(!known, fmt::format("unknown option '{}'", a));
            continue;
        }
        return ::sgrn::Error(fmt::format("unexpected argument '{}' (all fields are --flags)", a));
    }
    if (u.password.empty())
        u.password = promptSecretInput("password: ");
    RETURN_ERROR_IF(u.first_name.empty() || u.family_name.empty() || u.email.empty() || u.password.empty() || u.organisation.empty() ||
                        u.status.empty(),
        "--first/--family/--email/--password/--org/--status are required");
    RETURN_ERROR_IF(!client_, "not connected");
    auto r = client_->registerUser(u);
    RETURN_IF(r.hasError(), r.error());
    fmt::print("{}\n", r.value());
    return {};
}

Result<void> DatastoreShell::cmdServices() {
    RETURN_ERROR_IF(!client_, "not connected");
    auto res = client_->tryListServices();
    RETURN_IF(res.hasError(), res.error());
    std::vector<std::vector<std::string>> rows;
    for (const auto& s : res.value()) {
        std::string created = s.created_at.size() > 10 ? s.created_at.substr(0, 10) : s.created_at;
        rows.push_back({std::to_string(s.id), s.name, s.is_active ? "active" : "inactive", s.domain, created, maskSecret(s.token)});
    }
    printTable({"ID", "NAME", "STATUS", "DOMAIN", "CREATED", "TOKEN"}, std::move(rows), {0});
    return {};
}

Result<void> DatastoreShell::cmdServiceAdd(const std::vector<std::string>& t_args) {
    sgrn::datastore::client::NewService s;
    for (size_t i = 0; i < t_args.size(); ++i) {
        const std::string& a = t_args[i];
        bool consumed = false;
        for (const char* name : {"--name", "--org", "--organisation", "--kind", "--domain"}) {
            const std::string dash = name, eq = dash + "=";
            std::string v;
            if (a == dash && i + 1 < t_args.size()) {
                v = t_args[++i];
                consumed = true;
            } else if (a.rfind(eq, 0) == 0) {
                v = a.substr(eq.size());
                consumed = true;
            }
            if (!consumed)
                continue;
            const std::string key = std::string(name).substr(2);
            if (key == "name")
                s.name = v;
            else if (key == "org" || key == "organisation")
                s.organisation = v;
            else if (key == "kind")
                s.kind = v;
            else if (key == "domain")
                s.domain = v;
            break;
        }
        if (!consumed) {
            if (!a.empty() && a[0] != '-' && s.name.empty()) {
                s.name = a;
                consumed = true;
            }
        }
        RETURN_ERROR_IF(!consumed, fmt::format("unexpected argument '{}'", a));
    }
    RETURN_ERROR_IF(s.name.empty(), "--name is required");
    RETURN_ERROR_IF(!client_, "not connected");
    auto r = client_->registerService(s);
    RETURN_IF(r.hasError(), r.error());
    fmt::print("{}\n", r.value().message);
    if (!r.value().token.empty() || !r.value().token_secret.empty()) {
        printCredentialsOnce(r.value().token, r.value().token_secret);
    }
    return {};
}

Result<void> DatastoreShell::cmdServiceRotate(const std::vector<std::string>& t_args) {
    RETURN_ERROR_IF(t_args.empty(), "missing service (id or name)");
    auto id = resolveServiceId(t_args[0]);
    RETURN_IF(id.hasError(), id.error());
    RETURN_ERROR_IF(!client_, "not connected");
    auto r = client_->rotateServiceToken(id.value());
    RETURN_IF(r.hasError(), r.error());
    fmt::print("{}\n", r.value().message);
    if (!r.value().token.empty() || !r.value().token_secret.empty()) {
        printCredentialsOnce(r.value().token, r.value().token_secret);
    }
    return {};
}

} // namespace sgrn::datastore::shell
