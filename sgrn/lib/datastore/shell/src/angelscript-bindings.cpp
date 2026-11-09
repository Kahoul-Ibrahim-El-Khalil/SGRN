// angelscript-bindings.cpp — AngelScript registration and global wrappers.
#include "shell-common.hpp"

namespace sgrn::datastore::shell
{

// ── AngelScript Bindings ─────────────────────────────────────────────────────

void DatastoreShell::registerBindings() {
    auto* engine = p_script_engine_;
    if (!engine)
        return;

    engine->RegisterGlobalFunction("void connectService(const string &in, const string &in, const string &in)",
        asMETHOD(DatastoreShell, as_connectService), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction("void loginUser(const string &in, const string &in, const string &in)",
        asMETHOD(DatastoreShell, as_loginUser), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction("void logout()", asMETHOD(DatastoreShell, as_logout), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction(
        "string createUser(const string &in, const string &in, const string &in, const string &in, const string &in, const string &in)",
        asMETHOD(DatastoreShell, as_createUser), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction("void listUsers()", asMETHOD(DatastoreShell, as_listUsers), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction("string createService(const string &in, const string &in)", asMETHOD(DatastoreShell, as_createService),
        asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction("void listServices()", asMETHOD(DatastoreShell, as_listServices), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction(
        "string rotateServiceToken(const string &in)", asMETHOD(DatastoreShell, as_rotateServiceToken), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction("void listOrgs()", asMETHOD(DatastoreShell, as_listOrgs), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction("void listDomains()", asMETHOD(DatastoreShell, as_listDomains), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction(
        "void listStatuses(const string &in)", asMETHOD(DatastoreShell, as_listStatuses), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction("string whoami()", asMETHOD(DatastoreShell, as_whoami), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction("string storageStats()", asMETHOD(DatastoreShell, as_storageStats), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction("string storageInfo()", asMETHOD(DatastoreShell, as_storageInfo), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction("void changePassword(const string &in, const string &in)", asMETHOD(DatastoreShell, as_changePassword),
        asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction(
        "void setScope(const string &in)", asMETHOD(DatastoreShell, as_setScope), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction("string getScope()", asMETHOD(DatastoreShell, as_getScope), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction("string currentPath()", asMETHOD(DatastoreShell, as_currentPath), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction("void sessionInfo()", asMETHOD(DatastoreShell, as_sessionInfo), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction("bool isConnected()", asMETHOD(DatastoreShell, as_isConnected), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction("string cat(const string &in)", asMETHOD(DatastoreShell, as_cat), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction(
        "string pipe(const string &in, const string &in)", asMETHOD(DatastoreShell, as_pipe), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction(
        "string exec(const string &in, const string &in)", asMETHOD(DatastoreShell, as_exec), asCALL_THISCALL_ASGLOBAL, this);
    // Resumable upload
    engine->RegisterGlobalFunction("string rput(const string &in, const string &in, int64, bool = false)",
        asMETHODPR(DatastoreShell, as_rput, (const std::string&, const std::string&, int64_t, bool), std::string), asCALL_THISCALL_ASGLOBAL,
        this);
    // Admin: live sessions & webhooks
    engine->RegisterGlobalFunction("string sessions()", asMETHOD(DatastoreShell, as_sessions), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction("string webhooks()", asMETHOD(DatastoreShell, as_webhooks), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction(
        "string webhookAdd(const string &in, const string &in)", asMETHOD(DatastoreShell, as_webhookAdd), asCALL_THISCALL_ASGLOBAL, this);
    engine->RegisterGlobalFunction("string webhookDel(int)", asMETHOD(DatastoreShell, as_webhookDel), asCALL_THISCALL_ASGLOBAL, this);
}

void DatastoreShell::as_connectService(const std::string& t_url, const std::string& t_token, const std::string& t_secret) {
    reportResult("connectService", cmdConnect({"--url", t_url, "--token", t_token, "--secret", t_secret}));
}

void DatastoreShell::as_loginUser(const std::string& t_url, const std::string& t_email, const std::string& t_password) {
    reportResult("loginUser", cmdLogin({"--url", t_url, "--email", t_email, "--password", t_password}));
}

void DatastoreShell::as_logout() {
    reportResult("logout", cmdLogout());
}

std::string DatastoreShell::as_createUser(const std::string& t_first, const std::string& t_family, const std::string& t_email,
    const std::string& t_password, const std::string& t_org, const std::string& t_status) {
    if (!client_)
        return "error: not connected";
    sgrn::datastore::client::NewUser u;
    u.first_name_ = t_first;
    u.family_name_ = t_family;
    u.email_ = t_email;
    u.password_ = t_password;
    u.organisation_ = t_org;
    u.status_ = t_status;
    auto r = client_->registerUser(u);
    if (r.hasError()) {
        return "error: " + r.error();
    }
    return r.value();
}

void DatastoreShell::as_listUsers() {
    reportResult("listUsers", cmdUsers());
}

std::string DatastoreShell::as_createService(const std::string& t_name, const std::string& t_org) {
    if (!client_)
        return "error: not connected";
    sgrn::datastore::client::NewService s;
    s.name_ = t_name;
    s.organisation_ = t_org;
    auto r = client_->registerService(s);
    if (r.hasError()) {
        return "error: " + r.error();
    }
    return fmt::format("{} | token: {} secret: {}", r.value().message_, r.value().token_, r.value().token_secret_);
}

void DatastoreShell::as_listServices() {
    reportResult("listServices", cmdServices());
}

std::string DatastoreShell::as_rotateServiceToken(const std::string& t_name_or_id) {
    if (!client_)
        return "error: not connected";
    auto id = resolveServiceId(t_name_or_id);
    if (id.hasError())
        return "error: " + id.error();
    auto r = client_->rotateServiceToken(id.value());
    if (r.hasError())
        return "error: " + r.error();
    return fmt::format("{} | token: {} secret: {}", r.value().message_, r.value().token_, r.value().token_secret_);
}

void DatastoreShell::as_listOrgs() {
    reportResult("listOrgs", cmdOrgs());
}

void DatastoreShell::as_listDomains() {
    reportResult("listDomains", cmdDomains({}));
}

void DatastoreShell::as_listStatuses(const std::string& t_org) {
    reportResult("listStatuses", cmdStatuses({t_org}));
}

std::string DatastoreShell::as_whoami() {
    if (!client_)
        return "error: not connected";
    auto r = client_->userInfoJson();
    return r.hasError() ? "error: " + r.error() : r.value();
}

std::string DatastoreShell::as_storageStats() {
    if (!client_)
        return "error: not connected";
    auto r = client_->storageStatsJson();
    return r.hasError() ? "error: " + r.error() : r.value();
}

std::string DatastoreShell::as_storageInfo() {
    if (!client_)
        return "error: not connected";
    auto r = client_->storageConstraintsJson();
    return r.hasError() ? "error: " + r.error() : r.value();
}

void DatastoreShell::as_changePassword(const std::string& t_old_pw, const std::string& t_new_pw) {
    reportResult("changePassword", cmdPasswd({"--old", t_old_pw, "--new", t_new_pw}));
}

void DatastoreShell::as_setScope(const std::string& t_name) {
    reportResult("setScope", cmdScope({t_name}));
}

std::string DatastoreShell::as_getScope() {
    return std::string(sgrn::datastore::client::detail::storageScopeToString(scope_));
}

std::string DatastoreShell::as_currentPath() {
    return cwd_;
}

void DatastoreShell::as_sessionInfo() {
    reportResult("sessionInfo", cmdSession());
}

bool DatastoreShell::as_isConnected() {
    return connected();
}

std::string DatastoreShell::as_cat(const std::string& t_remote_path) {
    if (!client_) {
        fmt::print(stderr, "cat: not connected\n");
        return "";
    }
    const std::string path = normalizePath(t_remote_path);
    auto r = client_->doDownload(path, scope_);
    if (!r.ok) {
        fmt::print(stderr, "cat: {}: {}\n", path, r.message.empty() ? "download failed" : r.message);
        return "";
    }
    return r.bytes;
}

std::string DatastoreShell::as_pipe(const std::string& t_input, const std::string& t_shell_cmd) {
    return pipeToProcess(t_input, t_shell_cmd);
}

std::string DatastoreShell::as_exec(const std::string& t_shell_cmd, const std::string& t_input) {
    return pipeToProcess(t_input, t_shell_cmd);
}

// ── Resumable upload AS binding ──────────────────────────────────────────────

std::string DatastoreShell::as_rput(const std::string& t_local, const std::string& t_remote, int64_t t_chunk_size, bool t_encrypt) {
    std::vector<std::string> args = {t_local};
    if (!t_remote.empty())
        args.push_back(t_remote);
    if (t_chunk_size > 0) {
        args.push_back("--chunk-size");
        args.push_back(std::to_string(t_chunk_size));
    }
    if (t_encrypt) {
        args.push_back("--encrypt");
    }
    auto result = cmdRput(args);
    return result.hasError() ? "error: " + result.error() : "ok";
}

// ── Admin AS bindings ────────────────────────────────────────────────────────

std::string DatastoreShell::as_sessions() {
    if (!client_)
        return "error: not connected";
    auto r = client_->listActiveSessionsJson();
    return r.hasError() ? "error: " + r.error() : r.value();
}

std::string DatastoreShell::as_webhooks() {
    if (!client_)
        return "error: not connected";
    auto r = client_->listWebhooksJson();
    return r.hasError() ? "error: " + r.error() : r.value();
}

std::string DatastoreShell::as_webhookAdd(const std::string& t_url, const std::string& t_secret) {
    if (!client_)
        return "error: not connected";
    auto r = client_->registerWebhookJson(t_url, t_secret);
    return r.hasError() ? "error: " + r.error() : r.value();
}

std::string DatastoreShell::as_webhookDel(int32_t t_id) {
    RETURN_IF(!client_, "error: not connected");

    auto r = client_->deleteWebhookJson(t_id);

    return r.hasError() ? "error: " + r.error() : r.value();
}

} // namespace sgrn::datastore::shell
