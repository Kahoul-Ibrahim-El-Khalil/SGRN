#include "shell-common.hpp"

namespace sgrn::datastore::shell
{

namespace
{

using CommandHandler = std::function<int(DatastoreShell*, const std::vector<std::string>&)>;

struct CommandDispatchEntry {
    std::string_view name;
    bool needs_session;
    CommandHandler handler;
};

// Commands accepting arguments.
#define DATASTORE_SHELL_COMMAND(name, session, method)                                                                                     \
    {name, session, [](DatastoreShell* shell, const auto& args) { return reportResult(name, shell->method(args)); }}

// Commands without arguments.
#define DATASTORE_SHELL_COMMAND_NO_ARGS(name, session, method)                                                                             \
    {name, session, [](DatastoreShell* shell, const auto&) { return reportResult(name, shell->method()); }}

// Aliases sharing an existing command handler.
#define DATASTORE_SHELL_ALIAS(name, session, method) DATASTORE_SHELL_COMMAND(name, session, method)

// Help commands.
#define DATASTORE_SHELL_HELP(name)                                                                                                         \
    {name, false, [](DatastoreShell* shell, const auto&) {                                                                                 \
         shell->printHelp();                                                                                                               \
         return 0;                                                                                                                         \
     }}

// Exit commands.
#define DATASTORE_SHELL_EXIT(name) {name, false, [](DatastoreShell*, const auto&) { return -1; }}

// Special commands.
#define DATASTORE_SHELL_PWD                                                                                                                \
    {"pwd", false, [](DatastoreShell* shell, const auto&) {                                                                                \
         fmt::print("{}\n", shell->cwd_);                                                                                                  \
         return 0;                                                                                                                         \
     }}

} // namespace

int DatastoreShell::dispatch(const std::vector<std::string>& t_words) {
    if (t_words.empty())
        return 0;

    static const std::array<CommandDispatchEntry, 48> kCommandTable = {{
        // Help and shell control
        DATASTORE_SHELL_HELP("help"),
        DATASTORE_SHELL_HELP("-h"),
        DATASTORE_SHELL_HELP("--help"),
        DATASTORE_SHELL_EXIT("quit"),
        DATASTORE_SHELL_EXIT("exit"),

        // Connection and session management
        DATASTORE_SHELL_COMMAND("connect", false, cmdConnect),
        DATASTORE_SHELL_ALIAS("open", false, cmdConnect),
        DATASTORE_SHELL_COMMAND("login", false, cmdLogin),
        DATASTORE_SHELL_PWD,
        {"session", false, [](DatastoreShell* shell, const auto&) { return reportResult("session", shell->cmdSession()); }},
        DATASTORE_SHELL_COMMAND("scope", false, cmdScope),
        DATASTORE_SHELL_COMMAND("switch", false, cmdSwitch),

        // Filesystem operations
        DATASTORE_SHELL_COMMAND("ls", true, cmdLs),
        DATASTORE_SHELL_COMMAND("cd", true, cmdCd),
        DATASTORE_SHELL_COMMAND("cat", true, cmdCat),
        DATASTORE_SHELL_COMMAND("get", true, cmdGet),
        DATASTORE_SHELL_COMMAND("put", true, cmdPut),
        DATASTORE_SHELL_COMMAND("mkdir", true, cmdMkdir),
        DATASTORE_SHELL_COMMAND("mv", true, cmdMv),
        DATASTORE_SHELL_COMMAND("rm", true, cmdRm),
        DATASTORE_SHELL_COMMAND("tree", true, cmdTree),
        DATASTORE_SHELL_COMMAND("du", true, cmdDu),

        // Authentication and identity
        DATASTORE_SHELL_COMMAND_NO_ARGS("logout", true, cmdLogout),
        DATASTORE_SHELL_COMMAND("passwd", true, cmdPasswd),
        DATASTORE_SHELL_COMMAND_NO_ARGS("whoami", true, cmdWhoami),

        // Organization and administration
        DATASTORE_SHELL_COMMAND_NO_ARGS("orgs", true, cmdOrgs),
        DATASTORE_SHELL_COMMAND("domains", true, cmdDomains),
        DATASTORE_SHELL_COMMAND("statuses", true, cmdStatuses),
        DATASTORE_SHELL_COMMAND_NO_ARGS("users", true, cmdUsers),
        DATASTORE_SHELL_COMMAND("useradd", true, cmdUseradd),

        // Services
        DATASTORE_SHELL_COMMAND_NO_ARGS("services", true, cmdServices),
        DATASTORE_SHELL_COMMAND("service-add", true, cmdServiceAdd),
        DATASTORE_SHELL_COMMAND("service-token-rotate", true, cmdServiceRotate),

        // Diagnostics
        DATASTORE_SHELL_COMMAND_NO_ARGS("stats", true, cmdStats),
        DATASTORE_SHELL_COMMAND_NO_ARGS("info", true, cmdInfo),
        DATASTORE_SHELL_COMMAND("storage-overview", true, cmdStorageOverview),
        DATASTORE_SHELL_COMMAND("storage-orphans", true, cmdStorageOrphans),
        DATASTORE_SHELL_COMMAND("storage-purge", true, cmdStoragePurge),
        DATASTORE_SHELL_COMMAND("storage-search", true, cmdStorageSearch),
        DATASTORE_SHELL_COMMAND_NO_ARGS("constraints", true, cmdInfo),

        // Archive and upload operations
        DATASTORE_SHELL_COMMAND("zip", true, cmdZip),
        DATASTORE_SHELL_COMMAND("rput", true, cmdRput),
        DATASTORE_SHELL_COMMAND("upload-status", true, cmdUploadStatus),
        DATASTORE_SHELL_COMMAND("upload-abort", true, cmdUploadAbort),

        // Sessions and webhooks
        DATASTORE_SHELL_COMMAND_NO_ARGS("sessions", true, cmdSessions),
        DATASTORE_SHELL_COMMAND_NO_ARGS("webhooks", true, cmdWebhooks),
        DATASTORE_SHELL_COMMAND("webhook-add", true, cmdWebhookAdd),
        DATASTORE_SHELL_COMMAND("webhook-del", true, cmdWebhookDel),
    }};

    const std::string& cmd = t_words[0];

    for (const auto& entry : kCommandTable) {
        if (entry.name != cmd)
            continue;

        if (entry.needs_session && !connected()) {
            fmt::print(stderr, "not connected (connect --url URL --token TOKEN, or login --email EMAIL)\n");
            return 1;
        }

        const std::vector<std::string> args(t_words.begin() + 1, t_words.end());
        return entry.handler(this, args);
    }

    // Not a built-in command: allow AngelScript fallback.
    return 127;
}

// Keep macros local to this implementation file.
#undef DATASTORE_SHELL_COMMAND
#undef DATASTORE_SHELL_COMMAND_NO_ARGS
#undef DATASTORE_SHELL_ALIAS
#undef DATASTORE_SHELL_HELP
#undef DATASTORE_SHELL_EXIT
#undef DATASTORE_SHELL_PWD

} // namespace sgrn::datastore::shell
