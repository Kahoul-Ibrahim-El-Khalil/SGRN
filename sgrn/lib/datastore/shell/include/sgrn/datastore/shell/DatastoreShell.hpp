#pragma once
// DatastoreShell.hpp — AngelScript-powered administration shell for the SGRN datastore API.

#include <sgrn/AngelScriptEngine.hpp>
#include <sgrn/datastore/client/Client.hpp>
#include <sgrn/datastore/client/StorageClient.hpp>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sgrn::datastore::shell
{

/// Connection defaults from CLI flags / environment variables.
struct ConnDefaults {
    std::string url;
    std::string token;
    std::string secret;
    std::string email;
    std::string password;
    std::string session_token;
};

class DatastoreShell : public sgrn::scripting::AngelScriptEngine {
public:
    DatastoreShell();

    /// Set connection defaults (from CLI flags / env vars).
    void setDefaults(ConnDefaults t_def);

    /// Auto-connect using current defaults (for one-shot mode).
    bool autoConnect();

    /// Dispatch a single line of input: expansion, shell escapes, pipelines,
    /// then builtins. Returns exit status; -1 means quit, 127 means "not a
    /// shell line" (caller falls through to AngelScript evaluation).
    int dispatchLine(const std::string& t_text);

    /// Run a one-shot command from pre-parsed words.  Returns exit status.
    int runOneShot(const std::vector<std::string>& t_words);

    /// True when a backend session is active.
    bool connected() const;

    /// Build the prompt string for the REPL.
    std::string promptString() const;

    /// Execute an AngelScript batch script file.
    void runScript(const std::string& t_filename);

    /// Display help text.
    void printHelp();
    void showHelp() const override;

    static size_t editDistance(const std::string& t_a, const std::string& t_b);

    /// Tab completion generator for REPL.
    std::vector<std::string> generateCompletions(const std::string& t_line_buffer, const std::string& t_text, int t_start);
#ifndef _WIN32
    static char** completionDispatch(const char* tp_text, int t_start, int t_end);
#endif

protected:
    void onStart() override;
    std::string getPrompt() const override;
    bool handleMetaCommand(const std::string& t_line) override;

private:
    // ── shell state ─────────────────────────────────────────────────────────
    std::unique_ptr<sgrn::datastore::client::DatastoreClient> client_;
    std::unique_ptr<sgrn::datastore::client::StorageClient> storage_;
    std::string url_;
    std::string cwd_{"/"};
    sgrn::datastore::client::StorageScope scope_{sgrn::datastore::client::StorageScope::Auto};
    std::string auth_desc_{"-"};
    ConnDefaults defaults_;
    int last_exit_code_{0};

    // ── path resolution ─────────────────────────────────────────────────────
    struct Resolved {
        std::optional<int64_t> id;
        sgrn::datastore::client::DriveItemType type{sgrn::datastore::client::DriveItemType::File};
        bool is_dir{false};
    };

    static std::vector<std::string> splitWords(const std::string& t_line);
    std::string normalizePath(const std::string& t_arg) const;
    static std::string baseName(const std::string& t_path);
    static std::string dirName(const std::string& t_path);
    static std::string humanBytes(int64_t t_bytes);
    std::optional<Resolved> resolve(const std::string& t_abs);
    static bool requireId(const std::string& t_cmd, const std::string& t_path, const Resolved& t_hit);
    uint64_t duRecursive(const std::string& t_abs, bool& t_ok);
    void treeRecursive(const std::string& t_abs, const std::string& t_prefix, bool& t_ok);

    // ── connection helpers ───────────────────────────────────────────────────
    int establish(sgrn::datastore::client::DatastoreClientConfig t_cfg, const std::string& t_desc);
    static std::string promptVisibleInput(const std::string& t_prompt);
    static std::string promptSecretInput(const std::string& t_prompt);
    int64_t resolveServiceId(const std::string& t_ref, bool& t_ok);
    static std::string maskSecret(const std::string& t_secret);
    static std::optional<sgrn::datastore::client::StorageScope> parseScope(const std::string& t_name);

    // ── command implementations ─────────────────────────────────────────────
    int cmdConnect(const std::vector<std::string>& t_args);
    int cmdLogin(const std::vector<std::string>& t_args);
    int cmdLogout();
    int cmdOrgs();
    int cmdDomains(const std::vector<std::string>& t_args);
    int cmdStatuses(const std::vector<std::string>& t_args);
    int cmdUsers();
    int cmdUseradd(const std::vector<std::string>& t_args);
    int cmdServices();
    int cmdServiceAdd(const std::vector<std::string>& t_args);
    int cmdServiceRotate(const std::vector<std::string>& t_args);
    int cmdPasswd(const std::vector<std::string>& t_args);
    int cmdWhoami();
    int cmdStats();
    int cmdInfo();
    int cmdStorageOverview(const std::vector<std::string>& t_args);
    int cmdStorageOrphans(const std::vector<std::string>& t_args);
    int cmdStoragePurge(const std::vector<std::string>& t_args);
    int cmdStorageSearch(const std::vector<std::string>& t_args);
    int cmdLs(const std::vector<std::string>& t_args);
    int cmdCd(const std::vector<std::string>& t_args);
    int cmdCat(const std::vector<std::string>& t_args);
    int cmdGet(const std::vector<std::string>& t_args);
    int cmdPut(const std::vector<std::string>& t_args);
    int cmdMkdir(const std::vector<std::string>& t_args);
    int cmdMv(const std::vector<std::string>& t_args);
    int cmdRm(const std::vector<std::string>& t_args);
    int cmdTree(const std::vector<std::string>& t_args);
    int cmdDu(const std::vector<std::string>& t_args);
    int cmdSession();
    int cmdScope(const std::vector<std::string>& t_args);
    int cmdSwitch(const std::vector<std::string>& t_args);
    int cmdZip(const std::vector<std::string>& t_args);
    // Resumable upload
    int cmdRput(const std::vector<std::string>& t_args);
    int cmdUploadStatus(const std::vector<std::string>& t_args);
    int cmdUploadAbort(const std::vector<std::string>& t_args);
    // Live sessions & webhooks (admin)
    int cmdSessions();
    int cmdWebhooks();
    int cmdWebhookAdd(const std::vector<std::string>& t_args);
    int cmdWebhookDel(const std::vector<std::string>& t_args);
    int dispatch(const std::vector<std::string>& t_words);

    // ── shell escape & process piping ─────────────────────────────────────
    void runShellCommand(const std::string& t_cmd);
    static std::string pipeToProcess(const std::string& t_input, const std::string& t_cmd);
    int executePipeline(const std::string& t_line);
    void setReplVariable(const std::string& t_var_name, const std::string& t_val);

    // ── AngelScript binding registration ────────────────────────────────────
    void registerBindings();

    // AS wrapper methods (registered as global functions via asCALL_THISCALL_ASGLOBAL)
    void as_connectService(const std::string& t_url, const std::string& t_token, const std::string& t_secret);
    void as_loginUser(const std::string& t_url, const std::string& t_email, const std::string& t_password);
    void as_logout();
    std::string as_createUser(const std::string& t_first, const std::string& t_family, const std::string& t_email,
        const std::string& t_password, const std::string& t_org, const std::string& t_status);
    void as_listUsers();
    std::string as_createService(const std::string& t_name, const std::string& t_org);
    void as_listServices();
    std::string as_rotateServiceToken(const std::string& t_name_or_id);
    void as_listOrgs();
    void as_listDomains();
    void as_listStatuses(const std::string& t_org);
    std::string as_whoami();
    std::string as_storageStats();
    std::string as_storageInfo();
    void as_changePassword(const std::string& t_old_pw, const std::string& t_new_pw);
    void as_setScope(const std::string& t_name);
    std::string as_getScope();
    std::string as_currentPath();
    void as_sessionInfo();
    bool as_isConnected();
    std::string as_cat(const std::string& t_remote_path);
    std::string as_pipe(const std::string& t_input, const std::string& t_shell_cmd);
    std::string as_exec(const std::string& t_shell_cmd, const std::string& t_input);
    // Resumable upload AS bindings
    std::string as_rput(const std::string& t_local, const std::string& t_remote, int64_t t_chunk_size);
    // Admin AS bindings
    std::string as_sessions();
    std::string as_webhooks();
    std::string as_webhookAdd(const std::string& t_url, const std::string& t_secret);
    std::string as_webhookDel(int32_t t_id);
};

} // namespace sgrn::datastore::shell

namespace sgrn::datastore_shell
{
using namespace sgrn::datastore::shell;
}
