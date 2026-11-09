// connection.cpp — sign-in, prompts and credential/scope helpers.
#include "shell-common.hpp"
namespace sgrn::datastore::shell
{

// ── Connection Helpers ───────────────────────────────────────────────────────

sgrn::Result<void> DatastoreShell::establish(sgrn::datastore::client::DatastoreClientConfig t_cfg, const std::string& t_desc) {
    auto client = std::make_unique<sgrn::datastore::client::DatastoreClient>(std::move(t_cfg));
    RETURN_ERROR_IF(!client->signIn(), "sign-in failed");
    url_ = client->config().backend_url_;
    storage_ = std::make_unique<sgrn::datastore::client::StorageClient>(*client);
    client_ = std::move(client);
    cwd_ = "/";
    auth_desc_ = t_desc;
    fmt::print(styleOk(), "Connected to {} as {}.\n", url_, t_desc);
    return {};
}

std::string DatastoreShell::promptVisibleInput(const std::string& t_prompt) {
    fmt::print("{}", t_prompt);
    std::string out;
    std::getline(std::cin, out);
    return out;
}

std::string DatastoreShell::promptSecretInput(const std::string& t_prompt) {
#ifndef _WIN32
    if (isatty(STDIN_FILENO)) {
        fmt::print("{}", t_prompt);
        fflush(stdout);
        struct termios old_tio{};
        if (tcgetattr(STDIN_FILENO, &old_tio) == 0) {
            struct termios new_tio = old_tio;
            new_tio.c_lflag &= ~static_cast<unsigned>(ECHO);
            if (tcsetattr(STDIN_FILENO, TCSANOW, &new_tio) == 0) {
                std::string out;
                std::getline(std::cin, out);
                tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
                fmt::print("\n");
                return out;
            }
        }
    }
#endif
    return promptVisibleInput(t_prompt);
}

size_t DatastoreShell::editDistance(const std::string& t_a, const std::string& t_b) {
    std::vector<size_t> prev(t_b.size() + 1), cur(t_b.size() + 1);
    for (size_t j = 0; j <= t_b.size(); ++j)
        prev[j] = j;
    for (size_t i = 1; i <= t_a.size(); ++i) {
        cur[0] = i;
        for (size_t j = 1; j <= t_b.size(); ++j)
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (t_a[i - 1] == t_b[j - 1] ? 0 : 1)});
        prev.swap(cur);
    }
    return prev[t_b.size()];
}

sgrn::Result<int64_t> DatastoreShell::resolveServiceId(const std::string& t_ref) {
    try {
        size_t pos = 0;
        const long long id = std::stoll(t_ref, &pos);
        if (pos == t_ref.size() && id > 0)
            return static_cast<int64_t>(id);
    } catch (...) {
    }
    RETURN_ERROR_IF(!client_, "not connected");
    auto res = client_->tryListServices();
    RETURN_IF(res.hasError(), res.error());
    for (const auto& s : res.value()) {
        if (s.name_ == t_ref)
            return s.id_;
    }
    return ::sgrn::Error(fmt::format("service '{}' not found", t_ref));
}

std::string DatastoreShell::maskSecret(const std::string& t_secret) {
    if (t_secret.size() <= 6)
        return "…";
    return t_secret.substr(0, 6) + "…";
}

std::optional<sgrn::datastore::client::StorageScope> DatastoreShell::parseScope(const std::string& t_name) {
    using sgrn::datastore::client::StorageScope;
    struct ScopeMapping {
        std::string_view name;
        StorageScope scope;
    };
    static constexpr std::array<ScopeMapping, 7> kScopeMap = {{{"auto", StorageScope::Auto}, {"personal", StorageScope::Personal},
        {"users", StorageScope::Users}, {"automated-services", StorageScope::AutomatedServices},
        {"automatedservices", StorageScope::AutomatedServices}, {"domain", StorageScope::Domain}, {"domains", StorageScope::Domain}}};
    for (const auto& item : kScopeMap) {
        if (item.name == t_name)
            return item.scope;
    }
    return std::nullopt;
}

} // namespace sgrn::datastore::shell
