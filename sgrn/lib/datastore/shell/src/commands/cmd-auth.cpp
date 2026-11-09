// cmd-auth.cpp — connect/login/logout, password changes and identity.
#include "../shell-common.hpp"
namespace sgrn::datastore::shell
{

// ── connection flag parsing ─────────────────────────────────────────────────
namespace
{

static constexpr std::array<std::string_view, 6> kConnFlags = {"url", "token", "secret", "email", "password", "session-token"};

void takeConnFlag(const std::vector<std::string>& t_args, size_t& t_i, std::string& t_url, std::string& t_token, std::string& t_secret,
    std::string& t_email, std::string& t_password, std::string& t_session_token, std::vector<std::string>& t_positional,
    std::string& t_error) {
    const std::string& a = t_args[t_i];
    for (const auto& name : kConnFlags) {
        const std::string dash = "--" + std::string(name), eq = dash + "=";
        std::string value;
        bool hit = false;
        if (a == dash && t_i + 1 < t_args.size()) {
            value = t_args[++t_i];
            hit = true;
        } else if (a.rfind(eq, 0) == 0) {
            value = a.substr(eq.size());
            hit = true;
        }
        if (!hit)
            continue;
        if (name == "url")
            t_url = value;
        else if (name == "token")
            t_token = value;
        else if (name == "secret")
            t_secret = value;
        else if (name == "email")
            t_email = value;
        else if (name == "password")
            t_password = value;
        else if (name == "session-token")
            t_session_token = value;
        return;
    }
    if (a.rfind("--", 0) == 0) {
        std::string best;
        size_t best_d = 3;
        const std::string bare = a.substr(0, a.find('='));
        for (const auto& name : kConnFlags) {
            const std::string sname = "--" + std::string(name);
            const size_t d = DatastoreShell::editDistance(bare, sname);
            if (d < best_d) {
                best_d = d;
                best = sname;
            }
        }
        t_error = !best.empty() ? "unknown option '" + a + "' (did you mean '" + best + "'?)" : "unknown option '" + a + "'";
        return;
    }
    t_positional.push_back(a);
}

} // namespace

sgrn::Result<void> DatastoreShell::cmdConnect(const std::vector<std::string>& t_args) {
    std::string url = defaults_.url, token = defaults_.token, secret = defaults_.secret, email = defaults_.email,
                password = defaults_.password, session_token = defaults_.session_token;
    std::vector<std::string> positional;
    for (size_t i = 0; i < t_args.size(); ++i) {
        std::string err;
        takeConnFlag(t_args, i, url, token, secret, email, password, session_token, positional, err);
        RETURN_IF(!err.empty(), err);
    }
    if (url.empty() && !positional.empty())
        url = positional.front();
    RETURN_ERROR_IF(positional.size() > 1, fmt::format("unexpected argument '{}'", positional[1]));
    RETURN_ERROR_IF(url.empty(), "need a URL (arg, --url, or SGRN_DATASTORE_URL)");
    if (!email.empty()) {
        RETURN_ERROR_IF(password.empty(), "email needs --password (or SGRN_DATASTORE_PASSWORD)");
        sgrn::datastore::client::DatastoreClientConfig cfg;
        cfg.backend_url_ = url;
        cfg.auth_mode_ = sgrn::datastore::client::AuthMode::UserPassword;
        cfg.email_ = email;
        cfg.password_ = password;
        RETURN_IF(establish(std::move(cfg), "user").hasError(), fmt::format("user sign-in failed for {}", url));
        return {};
    }
    if (!session_token.empty()) {
        sgrn::datastore::client::DatastoreClientConfig cfg;
        cfg.backend_url_ = url;
        cfg.auth_mode_ = sgrn::datastore::client::AuthMode::SessionToken;
        cfg.session_token_ = session_token;
        RETURN_IF(establish(std::move(cfg), "session-token").hasError(), fmt::format("session token rejected by {}", url));
        return {};
    }
    RETURN_ERROR_IF(token.empty(), "need credentials (--token/--email/--session-token, see 'help')");
    sgrn::datastore::client::DatastoreClientConfig cfg;
    cfg.backend_url_ = url;
    cfg.token_ = token;
    cfg.secret_ = secret;
    RETURN_IF(establish(std::move(cfg), "service").hasError(), fmt::format("service sign-in failed for {}", url));
    return {};
}

sgrn::Result<void> DatastoreShell::cmdLogin(const std::vector<std::string>& t_args) {
    std::string email = defaults_.email, password = defaults_.password, url = defaults_.url;
    std::string ignored_token, ignored_secret, ignored_session;
    std::vector<std::string> positional;
    for (size_t i = 0; i < t_args.size(); ++i) {
        std::string err;
        takeConnFlag(t_args, i, url, ignored_token, ignored_secret, email, password, ignored_session, positional, err);
        RETURN_IF(!err.empty(), err);
    }
    if (email.empty() && !positional.empty()) {
        email = positional.front();
        positional.erase(positional.begin());
    }
    RETURN_ERROR_IF(!positional.empty(), fmt::format("unexpected argument '{}'", positional.front()));
    if (email.empty())
        email = promptVisibleInput("email: ");
    if (password.empty())
        password = promptSecretInput("password: ");
    RETURN_ERROR_IF(url.empty() || email.empty() || password.empty(), "need --url URL, an email and a password");
    sgrn::datastore::client::DatastoreClientConfig cfg;
    cfg.backend_url_ = url;
    cfg.auth_mode_ = sgrn::datastore::client::AuthMode::UserPassword;
    cfg.email_ = email;
    cfg.password_ = password;
    RETURN_IF(establish(std::move(cfg), "user").hasError(), fmt::format("sign-in failed for {} as {}", url, email));
    return {};
}

sgrn::Result<void> DatastoreShell::cmdLogout() {
    if (client_) {
        if (!client_->signOut())
            fmt::print(stderr, "logout: server sign-out failed (local session dropped anyway)\n");
    }
    client_.reset();
    storage_.reset();
    url_.clear();
    auth_desc_ = "-";
    cwd_ = "/";
    fmt::print("Logged out.\n");
    return {};
}

sgrn::Result<void> DatastoreShell::cmdPasswd(const std::vector<std::string>& t_args) {
    std::string old_pw, new_pw;
    for (size_t i = 0; i < t_args.size(); ++i) {
        const std::string& a = t_args[i];
        if ((a == "--old" || a == "--old-password") && i + 1 < t_args.size())
            old_pw = t_args[++i];
        else if ((a == "--new" || a == "--new-password") && i + 1 < t_args.size())
            new_pw = t_args[++i];
        else
            return ::sgrn::Error(fmt::format("unexpected argument '{}'", a));
    }
    if (old_pw.empty())
        old_pw = promptSecretInput("current password: ");
    if (new_pw.empty())
        new_pw = promptSecretInput("new password: ");
    RETURN_ERROR_IF(old_pw.empty() || new_pw.empty(), "both current and new passwords are required");
    std::string confirm = promptSecretInput("confirm new password: ");
    RETURN_ERROR_IF(confirm != new_pw, "new passwords do not match");
    RETURN_ERROR_IF(new_pw.size() < 8, "new password must be at least 8 characters");
    RETURN_ERROR_IF(!client_, "not connected");
    auto r = client_->updatePassword(old_pw, new_pw);
    RETURN_IF(r.hasError(), r.error());
    fmt::print("{}\nLog in again with the new password.\n", r.value());
    cmdLogout();
    return {};
}

sgrn::Result<void> DatastoreShell::cmdWhoami() {
    RETURN_ERROR_IF(!client_, "not connected");
    auto r = client_->userInfoJson();
    RETURN_IF(r.hasError(), r.error());
    printJsonPretty(r.value());
    return {};
}

} // namespace sgrn::datastore::shell
