// DatastoreShell.cpp — core lifecycle, REPL plumbing, completion and dispatch.
#include "shell-common.hpp"

#include <scriptbuilder/scriptbuilder.h>

#ifndef _WIN32
#include <readline/history.h>
#include <readline/readline.h>
#endif

namespace sgrn::datastore::shell
{

DatastoreShell::DatastoreShell() {
}

void DatastoreShell::setDefaults(ConnDefaults t_def) {
    defaults_ = std::move(t_def);
}

bool DatastoreShell::autoConnect() {
    const bool want_session = !defaults_.email.empty() || !defaults_.session_token.empty() || !defaults_.token.empty();
    if (want_session && !defaults_.url.empty()) {
        std::vector<std::string> no_args;
        return reportResult("connect", cmdConnect(no_args)) == 0;
    }
    return false;
}

bool DatastoreShell::connected() const {
    return client_ != nullptr;
}

std::string DatastoreShell::promptString() const {
    if (connected()) {
        return fmt::format("dss [{}] {}$ ", sgrn::datastore::client::detail::storageScopeToString(scope_), cwd_);
    }
    return "dss (offline)$ ";
}

#ifndef _WIN32
static DatastoreShell* g_active_shell_for_completion = nullptr;
static std::vector<std::string> g_completion_candidates;

char** DatastoreShell::completionDispatch(const char* tp_text, int t_start, int t_end) {
    rl_attempted_completion_over = 1;
    if (!g_active_shell_for_completion)
        return nullptr;

    const std::string line_buf(rl_line_buffer, t_end);
    const std::string text(tp_text);

    auto candidates = g_active_shell_for_completion->generateCompletions(line_buf, text, t_start);
    if (candidates.empty())
        return nullptr;

    g_completion_candidates = std::move(candidates);

    return rl_completion_matches(tp_text, [](const char*, int t_state) -> char* {
        static size_t idx;
        if (t_state == 0)
            idx = 0;
        if (idx < g_completion_candidates.size())
            return strdup(g_completion_candidates[idx++].c_str());
        return nullptr;
    });
}
#endif

void DatastoreShell::onStart() {
    // Register custom AngelScript bindings for DatastoreShell
    registerBindings();
#ifndef _WIN32
    g_active_shell_for_completion = this;
    rl_attempted_completion_function = &DatastoreShell::completionDispatch;
    // Welcome banner on interactive terminals only — piped sessions keep
    // machine-readable output.
    if (isatty(STDIN_FILENO)) {
        fmt::print(styleHeader(), "SGRN datastore shell\n");
        fmt::print("Type 'help' for commands, 'quit' to leave.\n");
        if (!connected()) {
            fmt::print("Not connected — 'connect' or 'login' to begin.\n");
        }
    }
#endif
}

std::vector<std::string> DatastoreShell::generateCompletions(const std::string& t_line_buffer, const std::string& t_text, int t_start) {
    std::set<std::string> matches;

    const std::string before = t_line_buffer.substr(0, t_start);
    auto words = splitWords(before);

    if (words.empty()) {
        static constexpr std::array<std::string_view, 46> builtins = {"connect", "login", "logout", "orgs", "domains", "statuses", "users",
            "useradd", "services", "service-add", "service-token-rotate", "passwd", "whoami", "stats", "info", "constraints", "ls", "cd",
            "cat", "get", "put", "rput", "upload-status", "upload-abort", "mkdir", "mv", "rm", "tree", "du", "session", "sessions", "scope",
            "switch", "zip", "storage-overview", "storage-orphans", "storage-purge", "storage-search", "webhooks", "webhook-add",
            "webhook-del", "help", "quit", "exit"};
        for (const auto& cmd : builtins) {
            if (cmd.rfind(t_text, 0) == 0)
                matches.insert(std::string(cmd));
        }

        if (p_script_engine_) {
            for (asUINT i = 0; i < p_script_engine_->GetGlobalFunctionCount(); ++i) {
                asIScriptFunction* f = p_script_engine_->GetGlobalFunctionByIndex(i);
                if (f) {
                    std::string name = f->GetName();
                    if (name.rfind(t_text, 0) == 0)
                        matches.insert(name);
                }
            }
        }
        return {matches.begin(), matches.end()};
    }

    const std::string& cmd = words[0];

    if (cmd == "switch") {
        if (words.size() == 1) {
            matches.insert("scope");
            return {matches.begin(), matches.end()};
        }
        if (words.size() == 2 && words[1] != "scope") {
            if (std::string("scope").rfind(t_text, 0) == 0)
                matches.insert("scope");
            return {matches.begin(), matches.end()};
        }
        if (words.size() < 2 || words[1] != "scope")
            return {};
        // fall through to scope-name completion below
    }

    if (cmd == "scope" || cmd == "switch") {
        static constexpr std::array<std::string_view, 5> scopes = {"auto", "personal", "users", "automated-services", "domain"};
        for (const auto& sc : scopes) {
            if (sc.rfind(t_text, 0) == 0)
                matches.insert(std::string(sc));
        }
        return {matches.begin(), matches.end()};
    }

    static constexpr std::array<std::string_view, 8> path_cmds = {"cd", "ls", "cat", "mkdir", "rm", "tree", "du", "zip"};
    const bool is_path_cmd = std::find(path_cmds.begin(), path_cmds.end(), cmd) != path_cmds.end() || (cmd == "get" && words.size() == 1) ||
                             (cmd == "put" && words.size() > 1);

    if (is_path_cmd) {

        if (storage_ && connected()) {
            std::string abs = normalizePath(t_text);
            std::string parent;
            std::string prefix;
            if (!t_text.empty() && t_text.back() == '/') {
                parent = abs;
                prefix = "";
            } else {
                parent = dirName(abs);
                prefix = baseName(abs);
            }

            auto r = storage_->tryListDrive(parent, scope_);
            if (!r.hasError()) {
                for (const auto& folder : r.value().folders) {
                    if (folder.name_.rfind(prefix, 0) == 0) {
                        const std::string candidate = (t_text.find('/') != std::string::npos)
                                                          ? (parent == "/" ? "/" + folder.name_ + "/" : parent + "/" + folder.name_ + "/")
                                                          : folder.name_ + "/";
                        matches.insert(candidate);
                    }
                }
                for (const auto& file : r.value().files) {
                    if (file.name_.rfind(prefix, 0) == 0) {
                        const std::string candidate = (t_text.find('/') != std::string::npos)
                                                          ? (parent == "/" ? "/" + file.name_ : parent + "/" + file.name_)
                                                          : file.name_;
                        matches.insert(candidate);
                    }
                }
            }
        }
        return {matches.begin(), matches.end()};
    }

    return {};
}

std::string DatastoreShell::getPrompt() const {
    return promptString();
}

void DatastoreShell::showHelp() const {
    const_cast<DatastoreShell*>(this)->printHelp();
}

bool DatastoreShell::handleMetaCommand(const std::string& t_line) {
    // Single funnel for every interactive line (base run() -> execute() ->
    // here): expansion, shell escapes, pipelines, builtins. Returns false
    // only for AngelScript fallback, which base execute() then evaluates.
    const int rc = dispatchLine(t_line);
    if (rc == 127)
        return false;
    last_exit_code_ = rc;
    return true;
}

// True when the line carries top-level pipeline syntax: a single `|` or a
// `>` redirection outside quotes and parentheses, excluding the AngelScript
// operators `||`, `>=`, `>>` and anything inside a `<...>` template bracket
// pair. This keeps one-liners like `a || b`, `x >= 1` or `array<int> v` on
// the AngelScript path while `cat f | grep x` / `ls > out.txt` run as
// pipelines. Wrap an expression in extra parens to force script evaluation.
static bool hasTopLevelPipe(const std::string& t_line) {
    int depth = 0;
    bool in_single = false;
    bool in_double = false;
    bool angle_open = false;
    for (size_t i = 0; i < t_line.size(); ++i) {
        const char c = t_line[i];
        if (c == '\'' && !in_double) {
            in_single = !in_single;
            continue;
        }
        if (c == '"' && !in_single) {
            in_double = !in_double;
            continue;
        }
        if (in_single || in_double)
            continue;
        if (c == '(') {
            ++depth;
            continue;
        }
        if (c == ')') {
            if (depth > 0)
                --depth;
            continue;
        }
        if (depth != 0)
            continue;
        if (c == '<') {
            angle_open = true;
            continue;
        }
        if (c == '|') {
            if (i + 1 < t_line.size() && t_line[i + 1] == '|') {
                ++i; // logical-or → AngelScript (skip both bars)
                continue;
            }
            return true;
        }
        if (c == '>') {
            if (angle_open)
                continue; // template bracket close (array<int>) → AngelScript
            if (i + 1 < t_line.size() && (t_line[i + 1] == '=' || t_line[i + 1] == '>'))
                continue; // >=, >> → AngelScript
            return true;
        }
    }
    return false;
}

int DatastoreShell::dispatchLine(const std::string& t_text) {
    if (t_text.empty())
        return 0;
    // Expand $NAME / ${NAME} from REPL globals first (numeric + string).
    // Unknown names stay literal; use $$ for a literal dollar.
    const std::string line = expandDollarVariables(t_text);
    if (line[0] == '!') {
        runShellCommand(line.substr(1));
        return 0;
    }

    if (hasTopLevelPipe(line)) {
        const int pipeline_rc = executePipeline(line);
        last_exit_code_ = pipeline_rc;
        return pipeline_rc;
    }

    auto words = splitWords(line);
    if (words.empty())
        return 0;

    // Returns 127 for AngelScript fallback; the caller (handleMetaCommand)
    // maps that back to base execute(). Never calls base execute() here —
    // that would recurse through handleMetaCommand.
    int rc = dispatch(words);
    if (rc == 127)
        return 127;
    last_exit_code_ = rc;
    return rc;
}

int DatastoreShell::runOneShot(const std::vector<std::string>& t_words) {
    if (t_words.empty())
        return 0;
    static constexpr std::array<std::string_view, 3> kAuthCmds = {"connect", "open", "login"};
    const bool is_auth_cmd = std::find(kAuthCmds.begin(), kAuthCmds.end(), t_words[0]) != kAuthCmds.end();
    if (!is_auth_cmd) {
        autoConnect();
    }
    int r = dispatch(t_words);
    if (r == 127) {
        // Evaluate as AngelScript statement / script
        std::string expr;
        for (size_t i = 0; i < t_words.size(); ++i) {
            if (i > 0)
                expr += " ";
            expr += t_words[i];
        }
        execute(expr);
        return 0;
    }
    return r == -1 ? 0 : r;
}

void DatastoreShell::runScript(const std::string& t_filename) {
    if (!p_script_engine_)
        return;
    const std::string filename = expandLocalPath(t_filename);
    CScriptBuilder builder;
    if (builder.StartNewModule(p_script_engine_, "main") < 0) {
        fmt::print(stderr, "Failed to create script module for '{}'\n", filename);
        return;
    }
    if (builder.AddSectionFromFile(filename.c_str()) < 0) {
        fmt::print(stderr, "Failed to load script section: '{}'\n", filename);
        return;
    }
    if (builder.BuildModule() < 0) {
        fmt::print(stderr, "Script compilation failed: '{}'\n", t_filename);
        return;
    }

    asIScriptModule* p_mod = p_script_engine_->GetModule("main");
    if (!p_mod)
        return;
    asIScriptFunction* p_func = p_mod->GetFunctionByDecl("void main()");
    if (!p_func) {
        fmt::print("Loaded script '{}' into REPL\n", t_filename);
        return;
    }

    asIScriptContext* p_ctx = p_script_engine_->CreateContext();
    if (!p_ctx)
        return;
    p_ctx->Prepare(p_func);
    const int r = p_ctx->Execute();
    if (r == asEXECUTION_EXCEPTION) {
        fmt::print(stderr, "Exception in script '{}': {}\n", t_filename, p_ctx->GetExceptionString());
    }
    p_ctx->Release();
}

} // namespace sgrn::datastore::shell
