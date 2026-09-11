// DatastoreShell.cpp — Implementation of AngelScript-powered datastore shell
#include <sgrn/datastore/shell/DatastoreShell.hpp>

#include <fmt/core.h>
#include <scriptbuilder/scriptbuilder.h>
#include <stdexcept>
#include <string>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#ifndef _WIN32
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
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
        return cmdConnect(no_args) == 0;
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
#include <readline/history.h>
#include <readline/readline.h>
#include <set>

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
#endif
}

std::vector<std::string> DatastoreShell::generateCompletions(const std::string& t_line_buffer, const std::string& t_text, int t_start) {
    std::set<std::string> matches;

    const std::string before = t_line_buffer.substr(0, t_start);
    auto words = splitWords(before);

    if (words.empty()) {
        static constexpr std::array<std::string_view, 32> builtins = {"connect", "login", "logout", "orgs", "domains", "statuses", "users",
            "useradd", "services", "service-add", "service-token-rotate", "passwd", "whoami", "stats", "info", "constraints", "ls", "cd",
            "cat", "get", "put", "mkdir", "mv", "rm", "tree", "du", "session", "scope", "zip", "help", "quit", "exit"};
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

    if (cmd == "scope") {
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
    if (t_line.empty()) {
        return true;
    }
    // Shell escape: !cmd
    if (t_line[0] == '!') {
        runShellCommand(t_line.substr(1));
        return true;
    }

    auto words = splitWords(t_line);
    if (words.empty()) {
        return true;
    }

    // Try executing as a shell command first
    const int rc = dispatch(words);
    if (rc != 127) {
        last_exit_code_ = rc;
        return true;
    }

    // If dispatch returned 127 (command not recognized), fall through to AngelScript evaluation
    return false;
}

std::string DatastoreShell::pipeToProcess(const std::string& t_input, const std::string& t_cmd) {
    if (t_cmd.empty())
        return t_input;
#ifndef _WIN32
    int in_fd[2];
    int out_fd[2];
    if (pipe(in_fd) != 0 || pipe(out_fd) != 0) {
        fmt::print(stderr, "pipe creation failed\n");
        return "";
    }

    pid_t pid = fork();
    if (pid == 0) {
        close(in_fd[1]);
        close(out_fd[0]);
        dup2(in_fd[0], STDIN_FILENO);
        dup2(out_fd[1], STDOUT_FILENO);
        close(in_fd[0]);
        close(out_fd[1]);

        execl("/bin/sh", "sh", "-c", t_cmd.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    close(in_fd[0]);
    close(out_fd[1]);

    std::thread writer([&]() {
        size_t written = 0;
        while (written < t_input.size()) {
            ssize_t n = write(in_fd[1], t_input.data() + written, t_input.size() - written);
            if (n <= 0)
                break;
            written += static_cast<size_t>(n);
        }
        close(in_fd[1]);
    });

    std::string result;
    char buffer[4096];
    ssize_t bytes_read;
    while ((bytes_read = read(out_fd[0], buffer, sizeof(buffer))) > 0) {
        result.append(buffer, static_cast<size_t>(bytes_read));
    }
    close(out_fd[0]);

    if (writer.joinable())
        writer.join();

    int status = 0;
    waitpid(pid, &status, 0);
    return result;
#else
    fmt::print(stderr, "process piping is not supported on Windows\n");
    return "";
#endif
}

void DatastoreShell::setReplVariable(const std::string& t_var_name, const std::string& t_val) {
    if (!p_repl_module_) {
        p_repl_module_ = p_script_engine_ ? p_script_engine_->GetModule("repl", asGM_ALWAYS_CREATE) : nullptr;
    }
    if (!p_repl_module_) {
        fmt::print(stderr, "setReplVariable: REPL script module unavailable\n");
        return;
    }

    int var_idx = p_repl_module_->GetGlobalVarIndexByName(t_var_name.c_str());
    if (var_idx < 0) {
        std::string decl = "string " + t_var_name + ";";
        p_repl_module_->CompileGlobalVar("repl", decl.c_str(), 0);
        var_idx = p_repl_module_->GetGlobalVarIndexByName(t_var_name.c_str());
    }

    if (var_idx >= 0) {
        void* p_var = p_repl_module_->GetAddressOfGlobalVar(var_idx);
        if (p_var) {
            *static_cast<std::string*>(p_var) = t_val;
            fmt::print("Assigned {} bytes to variable '{}'\n", t_val.size(), t_var_name);
            return;
        }
    }
    fmt::print(stderr, "Failed to assign variable '{}'\n", t_var_name);
}

int DatastoreShell::executePipeline(const std::string& t_line) {
    std::string line = t_line;
    std::string target_var;

    // Check for leading variable assignment: `string var = ...` or `var = ...`
    size_t eq_pos = line.find('=');
    if (eq_pos != std::string::npos && line.find("==") == std::string::npos) {
        std::string lhs = line.substr(0, eq_pos);
        auto lhs_words = splitWords(lhs);
        if (!lhs_words.empty()) {
            target_var = lhs_words.back();
            line = line.substr(eq_pos + 1);
        }
    }

    // Check for trailing `> target` (where target can be variable or file)
    size_t last_gt = line.find_last_of('>');
    std::string target_file;
    if (last_gt != std::string::npos && (last_gt == 0 || line[last_gt - 1] != '-')) {
        std::string trailing = line.substr(last_gt + 1);
        auto trail_words = splitWords(trailing);
        if (!trail_words.empty()) {
            std::string token = trail_words.back();
            bool is_ident = !token.empty() && (std::isalpha(token[0]) || token[0] == '_');
            if (is_ident) {
                for (char c : token) {
                    if (!std::isalnum(c) && c != '_') {
                        is_ident = false;
                        break;
                    }
                }
            }
            if (is_ident && target_var.empty()) {
                target_var = token;
                line = line.substr(0, last_gt);
            } else if (token.find('.') != std::string::npos || token.find('/') != std::string::npos) {
                target_file = token;
                line = line.substr(0, last_gt);
            }
        }
    }

    // Split remaining pipeline commands by `|` or `>` respecting parentheses `!(...)`
    std::vector<std::string> raw_stages;
    std::string cur;
    int paren_depth = 0;
    for (char c : line) {
        if (c == '(')
            paren_depth++;
        else if (c == ')') {
            if (paren_depth > 0)
                paren_depth--;
        }

        if ((c == '|' || c == '>') && paren_depth == 0) {
            if (!cur.empty()) {
                raw_stages.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        raw_stages.push_back(cur);

    if (raw_stages.empty())
        return 1;

    // Helper to unwrap `!(cmd)` syntax
    auto unwrapCmd = [](std::string t_cmd) -> std::string {
        while (!t_cmd.empty() && std::isspace(t_cmd.front()))
            t_cmd.erase(t_cmd.begin());
        while (!t_cmd.empty() && std::isspace(t_cmd.back()))
            t_cmd.pop_back();

        if (t_cmd.rfind("!(", 0) == 0 && !t_cmd.empty() && t_cmd.back() == ')') {
            return t_cmd.substr(2, t_cmd.size() - 3);
        }
        if (!t_cmd.empty() && t_cmd[0] == '!') {
            return t_cmd.substr(1);
        }
        return t_cmd;
    };

    auto stage1_words = splitWords(unwrapCmd(raw_stages[0]));
    if (stage1_words.empty())
        return 1;

    std::string current_bytes;
    if (stage1_words[0] == "cat" && stage1_words.size() > 1) {
        const std::string remote = normalizePath(stage1_words[1]);
        if (!client_) {
            fmt::print(stderr, "cat: not connected\n");
            return 1;
        }
        auto r = client_->doDownload(remote, scope_);
        if (!r.ok) {
            fmt::print(stderr, "cat: {}: {}\n", remote, r.message.empty() ? "download failed" : r.message);
            return 1;
        }
        current_bytes = std::move(r.bytes);
    } else {
        current_bytes = pipeToProcess("", unwrapCmd(raw_stages[0]));
    }

    for (size_t i = 1; i < raw_stages.size(); ++i) {
        current_bytes = pipeToProcess(current_bytes, unwrapCmd(raw_stages[i]));
    }

    if (!target_var.empty()) {
        setReplVariable(target_var, current_bytes);
    } else if (!target_file.empty()) {
        std::ofstream ofs(target_file, std::ios::binary);
        if (!ofs) {
            fmt::print(stderr, "pipeline: cannot open local file '{}'\n", target_file);
            return 1;
        }
        ofs.write(current_bytes.data(), static_cast<std::streamsize>(current_bytes.size()));
        fmt::print("Wrote {} bytes to file '{}'\n", current_bytes.size(), target_file);
    } else {
        fmt::print("{}", current_bytes);
    }

    return 0;
}

int DatastoreShell::dispatchLine(const std::string& t_text) {
    if (t_text.empty())
        return 0;
    if (t_text[0] == '!') {
        runShellCommand(t_text.substr(1));
        return 0;
    }

    if ((t_text.find('|') != std::string::npos || t_text.find('>') != std::string::npos || t_text.find("!(") != std::string::npos) &&
        t_text.find("()") == std::string::npos) {
        return executePipeline(t_text);
    }

    auto words = splitWords(t_text);
    if (words.empty())
        return 0;

    int rc = dispatch(words);
    if (rc == 127) {
        // Fall back to AngelScript string execution
        execute(t_text);
        return 0;
    }
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
    CScriptBuilder builder;
    if (builder.StartNewModule(p_script_engine_, "main") < 0) {
        fmt::print(stderr, "Failed to create script module for '{}'\n", t_filename);
        return;
    }
    if (builder.AddSectionFromFile(t_filename.c_str()) < 0) {
        fmt::print(stderr, "Failed to load script section: '{}'\n", t_filename);
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

// ── Path resolution & tiny helpers ───────────────────────────────────────────

std::vector<std::string> DatastoreShell::splitWords(const std::string& t_line) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false;
    for (char c : t_line) {
        if (c == '"') {
            in_quotes = !in_quotes;
        } else if (c == ' ' || c == '\t') {
            if (in_quotes) {
                cur += c;
            } else if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        } else {
            cur += c;
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

std::string DatastoreShell::normalizePath(const std::string& t_arg) const {
    std::string joined = (!t_arg.empty() && t_arg[0] == '/') ? t_arg : cwd_ + "/" + t_arg;
    std::vector<std::string> parts;
    std::string cur;
    for (char c : joined + "/") {
        if (c == '/') {
            if (cur.empty() || cur == ".") {
                // skip
            } else if (cur == "..") {
                if (!parts.empty())
                    parts.pop_back();
            } else {
                parts.push_back(cur);
            }
            cur.clear();
        } else {
            cur += c;
        }
    }
    std::string out = "/";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0)
            out += "/";
        out += parts[i];
    }
    return out;
}

std::string DatastoreShell::baseName(const std::string& t_path) {
    const size_t p = t_path.find_last_of('/');
    return (p == std::string::npos) ? t_path : t_path.substr(p + 1);
}

std::string DatastoreShell::dirName(const std::string& t_path) {
    if (t_path == "/")
        return "/";
    const size_t p = t_path.find_last_of('/');
    return (p == 0) ? "/" : t_path.substr(0, p);
}

std::string DatastoreShell::humanBytes(int64_t t_bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double v = static_cast<double>(t_bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    return fmt::format("{:.1f}{}", v, units[u]);
}

std::optional<DatastoreShell::Resolved> DatastoreShell::resolve(const std::string& t_abs) {
    if (t_abs == "/")
        return std::nullopt;
    const std::string parent = dirName(t_abs);
    const std::string name = baseName(t_abs);
    if (!storage_)
        return std::nullopt;
    auto r = storage_->tryListDrive(parent, scope_);
    if (r.hasError()) {
        fmt::print(stderr, "ls: {}: {}\n", parent, r.error());
        return std::nullopt;
    }
    for (const auto& f : r.value().folders) {
        if (f.name_ == name)
            return Resolved{f.id_, sgrn::datastore::client::DriveItemType::Folder, true};
    }
    for (const auto& f : r.value().files) {
        if (f.name_ == name)
            return Resolved{f.id_ != 0 ? std::optional<int64_t>(f.id_) : std::nullopt, sgrn::datastore::client::DriveItemType::File, false};
    }
    return std::nullopt;
}

bool DatastoreShell::requireId(const std::string& t_cmd, const std::string& t_path, const Resolved& t_hit) {
    if (!t_hit.id) {
        fmt::print(stderr, "{}: '{}': namespace entry has no id and cannot be addressed (move/delete inside it instead)\n", t_cmd, t_path);
        return false;
    }
    return true;
}

uint64_t DatastoreShell::duRecursive(const std::string& t_abs, bool& t_ok) {
    uint64_t total = 0;
    if (!storage_) {
        t_ok = false;
        return 0;
    }
    auto r = storage_->tryListDrive(t_abs, scope_);
    if (r.hasError()) {
        fmt::print(stderr, "du: {}: {}\n", t_abs, r.error());
        t_ok = false;
        return 0;
    }
    for (const auto& f : r.value().files)
        total += static_cast<uint64_t>(f.size);
    for (const auto& d : r.value().folders) {
        const std::string child = t_abs == "/" ? "/" + d.name_ : t_abs + "/" + d.name_;
        total += duRecursive(child, t_ok);
    }
    return total;
}

void DatastoreShell::treeRecursive(const std::string& t_abs, const std::string& t_prefix, bool& t_ok) {
    if (!storage_) {
        t_ok = false;
        return;
    }
    auto r = storage_->tryListDrive(t_abs, scope_);
    if (r.hasError()) {
        fmt::print(stderr, "tree: {}: {}\n", t_abs, r.error());
        t_ok = false;
        return;
    }
    const auto& folders = r.value().folders;
    const auto& files = r.value().files;
    for (size_t i = 0; i < folders.size() + files.size(); ++i) {
        const bool last = (i + 1 == folders.size() + files.size());
        const bool is_dir = i < folders.size();
        const std::string& name = is_dir ? folders[i].name_ : files[i - folders.size()].name_;
        fmt::print("{}{}── {}\n", t_prefix, last ? "└" : "├", is_dir ? name + "/" : name);
        if (is_dir) {
            const std::string child = t_abs == "/" ? "/" + name : t_abs + "/" + name;
            treeRecursive(child, t_prefix + (last ? "    " : "│   "), t_ok);
        }
    }
}

// ── Connection Helpers ───────────────────────────────────────────────────────

int DatastoreShell::establish(sgrn::datastore::client::DatastoreClientConfig t_cfg, const std::string& t_desc) {
    auto client = std::make_unique<sgrn::datastore::client::DatastoreClient>(std::move(t_cfg));
    if (!client->signIn())
        return -1;
    url_ = client->config().backend_url_;
    storage_ = std::make_unique<sgrn::datastore::client::StorageClient>(*client);
    client_ = std::move(client);
    cwd_ = "/";
    auth_desc_ = t_desc;
    fmt::print("Connected to {} as {}.\n", url_, t_desc);
    return 0;
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

int64_t DatastoreShell::resolveServiceId(const std::string& t_ref, bool& t_ok) {
    t_ok = true;
    try {
        size_t pos = 0;
        const long long id = std::stoll(t_ref, &pos);
        if (pos == t_ref.size() && id > 0)
            return static_cast<int64_t>(id);
    } catch (...) {
    }
    if (!client_) {
        t_ok = false;
        return 0;
    }
    auto res = client_->tryListServices();
    if (res.hasError()) {
        fmt::print(stderr, "services: {}\n", res.error());
        t_ok = false;
        return 0;
    }
    for (const auto& s : res.value()) {
        if (s.name_ == t_ref)
            return s.id_;
    }
    fmt::print(stderr, "service '{}' not found\n", t_ref);
    t_ok = false;
    return 0;
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

// ── Command implementations ──────────────────────────────────────────────────

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

int DatastoreShell::cmdConnect(const std::vector<std::string>& t_args) {
    std::string url = defaults_.url, token = defaults_.token, secret = defaults_.secret, email = defaults_.email,
                password = defaults_.password, session_token = defaults_.session_token;
    std::vector<std::string> positional;
    for (size_t i = 0; i < t_args.size(); ++i) {
        std::string err;
        takeConnFlag(t_args, i, url, token, secret, email, password, session_token, positional, err);
        if (!err.empty()) {
            fmt::print(stderr, "connect: {}\n", err);
            return 1;
        }
    }
    if (url.empty() && !positional.empty())
        url = positional.front();
    if (positional.size() > 1) {
        fmt::print(stderr, "connect: unexpected argument '{}'\n", positional[1]);
        return 1;
    }
    if (url.empty()) {
        fmt::print(stderr, "connect: need a URL (arg, --url, or SGRN_DATASTORE_URL)\n");
        return 1;
    }
    if (!email.empty()) {
        if (password.empty()) {
            fmt::print(stderr, "connect: email needs --password (or SGRN_DATASTORE_PASSWORD)\n");
            return 1;
        }
        sgrn::datastore::client::DatastoreClientConfig cfg;
        cfg.backend_url_ = url;
        cfg.auth_mode_ = sgrn::datastore::client::AuthMode::UserPassword;
        cfg.email_ = email;
        cfg.password_ = password;
        if (establish(std::move(cfg), "user") != 0) {
            fmt::print(stderr, "connect: user sign-in failed for {}\n", url);
            return 1;
        }
        return 0;
    }
    if (!session_token.empty()) {
        sgrn::datastore::client::DatastoreClientConfig cfg;
        cfg.backend_url_ = url;
        cfg.auth_mode_ = sgrn::datastore::client::AuthMode::SessionToken;
        cfg.session_token_ = session_token;
        if (establish(std::move(cfg), "session-token") != 0) {
            fmt::print(stderr, "connect: session token rejected by {}\n", url);
            return 1;
        }
        return 0;
    }
    if (token.empty()) {
        fmt::print(stderr, "connect: need credentials (--token/--email/--session-token, see 'help')\n");
        return 1;
    }
    sgrn::datastore::client::DatastoreClientConfig cfg;
    cfg.backend_url_ = url;
    cfg.token_ = token;
    cfg.secret_ = secret;
    if (establish(std::move(cfg), "service") != 0) {
        fmt::print(stderr, "connect: service sign-in failed for {}\n", url);
        return 1;
    }
    return 0;
}

int DatastoreShell::cmdLogin(const std::vector<std::string>& t_args) {
    std::string email = defaults_.email, password = defaults_.password, url = defaults_.url;
    std::string ignored_token, ignored_secret, ignored_session;
    std::vector<std::string> positional;
    for (size_t i = 0; i < t_args.size(); ++i) {
        std::string err;
        takeConnFlag(t_args, i, url, ignored_token, ignored_secret, email, password, ignored_session, positional, err);
        if (!err.empty()) {
            fmt::print(stderr, "login: {}\n", err);
            return 1;
        }
    }
    if (email.empty() && !positional.empty()) {
        email = positional.front();
        positional.erase(positional.begin());
    }
    if (!positional.empty()) {
        fmt::print(stderr, "login: unexpected argument '{}'\n", positional.front());
        return 1;
    }
    if (email.empty())
        email = promptVisibleInput("email: ");
    if (password.empty())
        password = promptSecretInput("password: ");
    if (url.empty() || email.empty() || password.empty()) {
        fmt::print(stderr, "login: need --url URL, an email and a password\n");
        return 1;
    }
    sgrn::datastore::client::DatastoreClientConfig cfg;
    cfg.backend_url_ = url;
    cfg.auth_mode_ = sgrn::datastore::client::AuthMode::UserPassword;
    cfg.email_ = email;
    cfg.password_ = password;
    if (establish(std::move(cfg), "user") != 0) {
        fmt::print(stderr, "login: sign-in failed for {} as {}\n", url, email);
        return 1;
    }
    return 0;
}

int DatastoreShell::cmdLogout() {
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
    return 0;
}

int DatastoreShell::cmdOrgs() {
    if (!client_)
        return 1;
    auto res = client_->tryListOrganisations();
    if (res.hasError()) {
        fmt::print(stderr, "orgs: {}\n", res.error());
        return 1;
    }
    for (const auto& o : res.value())
        fmt::print("{}  {}\n", o.id_, o.name_);
    return 0;
}

int DatastoreShell::cmdDomains(const std::vector<std::string>& t_args) {
    if (t_args.empty()) {
        fmt::print(stderr, "domains: missing organisation (domains <org>)\n");
        return 1;
    }
    if (!client_)
        return 1;
    auto res = client_->tryListDomains(t_args[0]);
    if (res.hasError()) {
        fmt::print(stderr, "domains: {}\n", res.error());
        return 1;
    }
    for (const auto& d : res.value())
        fmt::print("{}  {}\n", d.id_, d.name_);
    return 0;
}

int DatastoreShell::cmdStatuses(const std::vector<std::string>& t_args) {
    if (t_args.empty()) {
        fmt::print(stderr, "statuses: missing organisation (statuses <org>)\n");
        return 1;
    }
    if (!client_)
        return 1;
    auto res = client_->tryListStatuses(t_args[0]);
    if (res.hasError()) {
        fmt::print(stderr, "statuses: {}\n", res.error());
        return 1;
    }
    for (const auto& s : res.value())
        fmt::print("{}  {}\n", s.id_, s.name_);
    return 0;
}

int DatastoreShell::cmdUsers() {
    if (!client_)
        return 1;
    auto res = client_->tryListUsers();
    if (res.hasError()) {
        fmt::print(stderr, "users: {}\n", res.error());
        return 1;
    }
    for (const auto& u : res.value())
        fmt::print("{}  {}  {} {}  [{}] {}\n", u.id_, u.email_, u.first_name_, u.family_name_, u.status_, u.domain_);
    return 0;
}

int DatastoreShell::cmdUseradd(const std::vector<std::string>& t_args) {
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
                    u.first_name_ = v;
                else if (key == "family")
                    u.family_name_ = v;
                else if (key == "email")
                    u.email_ = v;
                else if (key == "password")
                    u.password_ = v;
                else if (key == "phone")
                    u.phone_number_ = v;
                else if (key == "org" || key == "organisation")
                    u.organisation_ = v;
                else if (key == "status")
                    u.status_ = v;
                else if (key == "domain")
                    u.domain_ = v;
                break;
            }
            if (!known) {
                fmt::print(stderr, "useradd: unknown option '{}'\n", a);
                return 1;
            }
            continue;
        }
        fmt::print(stderr, "useradd: unexpected argument '{}' (all fields are --flags)\n", a);
        return 1;
    }
    if (u.password_.empty())
        u.password_ = promptSecretInput("password: ");
    if (u.first_name_.empty() || u.family_name_.empty() || u.email_.empty() || u.password_.empty() || u.organisation_.empty() ||
        u.status_.empty()) {
        fmt::print(stderr, "useradd: --first/--family/--email/--password/--org/--status are required\n");
        return 1;
    }
    if (!client_)
        return 1;
    auto r = client_->registerUser(u);
    if (r.hasError()) {
        fmt::print(stderr, "useradd: {}\n", r.error());
        return 1;
    }
    fmt::print("{}\n", r.value());
    return 0;
}

int DatastoreShell::cmdServices() {
    if (!client_)
        return 1;
    auto res = client_->tryListServices();
    if (res.hasError()) {
        fmt::print(stderr, "services: {}\n", res.error());
        return 1;
    }
    for (const auto& s : res.value())
        fmt::print("{}  {}  [{}] {}  {}  token={}\n", s.id_, s.name_, s.is_active_ ? "active" : "inactive", s.domain_, s.created_at_,
            maskSecret(s.token_));
    return 0;
}

int DatastoreShell::cmdServiceAdd(const std::vector<std::string>& t_args) {
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
                s.name_ = v;
            else if (key == "org" || key == "organisation")
                s.organisation_ = v;
            else if (key == "kind")
                s.kind_ = v;
            else if (key == "domain")
                s.domain_ = v;
            break;
        }
        if (!consumed) {
            if (!a.empty() && a[0] != '-' && s.name_.empty()) {
                s.name_ = a;
                consumed = true;
            }
        }
        if (!consumed) {
            fmt::print(stderr, "service-add: unexpected argument '{}'\n", a);
            return 1;
        }
    }
    if (s.name_.empty()) {
        fmt::print(stderr, "service-add: --name is required\n");
        return 1;
    }
    if (!client_)
        return 1;
    auto r = client_->registerService(s);
    if (r.hasError()) {
        fmt::print(stderr, "service-add: {}\n", r.error());
        return 1;
    }
    fmt::print("{}\n", r.value().message_);
    if (!r.value().token_.empty() || !r.value().token_secret_.empty()) {
        fmt::print("token: {}\ntoken_secret: {}\n(shown once — store them now)\n", r.value().token_, r.value().token_secret_);
    }
    return 0;
}

int DatastoreShell::cmdServiceRotate(const std::vector<std::string>& t_args) {
    if (t_args.empty()) {
        fmt::print(stderr, "service-token-rotate: missing service (id or name)\n");
        return 1;
    }
    bool ok = false;
    const int64_t id = resolveServiceId(t_args[0], ok);
    if (!ok || id <= 0) {
        if (ok)
            fmt::print(stderr, "service-token-rotate: '{}' did not resolve to a service\n", t_args[0]);
        return 1;
    }
    if (!client_)
        return 1;
    auto r = client_->rotateServiceToken(id);
    if (r.hasError()) {
        fmt::print(stderr, "service-token-rotate: {}\n", r.error());
        return 1;
    }
    fmt::print("{}\n", r.value().message_);
    if (!r.value().token_.empty() || !r.value().token_secret_.empty()) {
        fmt::print("token: {}\ntoken_secret: {}\n(shown once — store them now)\n", r.value().token_, r.value().token_secret_);
    }
    return 0;
}

int DatastoreShell::cmdPasswd(const std::vector<std::string>& t_args) {
    std::string old_pw, new_pw;
    for (size_t i = 0; i < t_args.size(); ++i) {
        const std::string& a = t_args[i];
        if ((a == "--old" || a == "--old-password") && i + 1 < t_args.size())
            old_pw = t_args[++i];
        else if ((a == "--new" || a == "--new-password") && i + 1 < t_args.size())
            new_pw = t_args[++i];
        else {
            fmt::print(stderr, "passwd: unexpected argument '{}'\n", a);
            return 1;
        }
    }
    if (old_pw.empty())
        old_pw = promptSecretInput("current password: ");
    if (new_pw.empty())
        new_pw = promptSecretInput("new password: ");
    if (old_pw.empty() || new_pw.empty()) {
        fmt::print(stderr, "passwd: both current and new passwords are required\n");
        return 1;
    }
    std::string confirm = promptSecretInput("confirm new password: ");
    if (confirm != new_pw) {
        fmt::print(stderr, "passwd: new passwords do not match\n");
        return 1;
    }
    if (new_pw.size() < 8) {
        fmt::print(stderr, "passwd: new password must be at least 8 characters\n");
        return 1;
    }
    if (!client_)
        return 1;
    auto r = client_->updatePassword(old_pw, new_pw);
    if (r.hasError()) {
        fmt::print(stderr, "passwd: {}\n", r.error());
        return 1;
    }
    fmt::print("{}\nLog in again with the new password.\n", r.value());
    cmdLogout();
    return 0;
}

int DatastoreShell::cmdWhoami() {
    if (!client_)
        return 1;
    auto r = client_->userInfoJson();
    if (r.hasError()) {
        fmt::print(stderr, "whoami: {}\n", r.error());
        return 1;
    }
    fmt::print("{}\n", r.value());
    return 0;
}

int DatastoreShell::cmdStats() {
    if (!client_)
        return 1;
    auto r = client_->storageStatsJson();
    if (r.hasError()) {
        fmt::print(stderr, "stats: {}\n", r.error());
        return 1;
    }
    fmt::print("{}\n", r.value());
    return 0;
}

int DatastoreShell::cmdInfo() {
    if (!client_)
        return 1;
    auto r = client_->storageConstraintsJson();
    if (r.hasError()) {
        fmt::print(stderr, "info: {}\n", r.error());
        return 1;
    }
    fmt::print("{}\n", r.value());
    return 0;
}

int DatastoreShell::cmdLs(const std::vector<std::string>& t_args) {
    const std::string path = t_args.empty() ? cwd_ : normalizePath(t_args[0]);
    if (!storage_)
        return 1;
    auto r = storage_->tryListDrive(path, scope_);
    if (r.hasError()) {
        fmt::print(stderr, "ls: {}: {}\n", path, r.error());
        return 1;
    }
    for (const auto& d : r.value().folders)
        fmt::print("{}/\n", d.name_);
    for (const auto& f : r.value().files)
        fmt::print("{} ({})\n", f.name_, humanBytes(f.size));
    return 0;
}

int DatastoreShell::cmdCd(const std::vector<std::string>& t_args) {
    if (t_args.empty()) {
        cwd_ = "/";
        return 0;
    }
    const std::string path = normalizePath(t_args[0]);
    if (path == "/") {
        cwd_ = "/";
        return 0;
    }
    auto hit = resolve(path);
    if (!hit || !hit->is_dir) {
        fmt::print(stderr, "cd: {}: No such directory\n", path);
        return 1;
    }
    cwd_ = path;
    return 0;
}

int DatastoreShell::cmdCat(const std::vector<std::string>& t_args) {
    if (t_args.empty()) {
        fmt::print(stderr, "cat: missing file operand\n");
        return 1;
    }
    const std::string path = normalizePath(t_args[0]);
    if (!client_)
        return 1;
    auto r = client_->doDownload(path, scope_);
    if (!r.ok) {
        fmt::print(stderr, "cat: {}: {}\n", path, r.message.empty() ? "download failed" : r.message);
        return 1;
    }
    fmt::print("{}", r.bytes);
    return 0;
}

int DatastoreShell::cmdGet(const std::vector<std::string>& t_args) {
    if (t_args.empty()) {
        fmt::print(stderr, "get: missing remote path (get <remote> [local])\n");
        return 1;
    }
    const std::string remote = normalizePath(t_args[0]);
    std::string local = (t_args.size() > 1) ? t_args[1] : baseName(remote);
    if (std::filesystem::is_directory(local)) {
        local = (std::filesystem::path(local) / baseName(remote)).string();
    }
    if (!storage_ || !storage_->download(remote, local, scope_)) {
        fmt::print(stderr, "get: {}: download failed\n", remote);
        return 1;
    }
    return 0;
}

int DatastoreShell::cmdPut(const std::vector<std::string>& t_args) {
    if (t_args.empty()) {
        fmt::print(stderr, "put: missing local path (put <local> [remote])\n");
        return 1;
    }
    const std::string local = t_args[0];
    std::string remote = (t_args.size() > 1) ? normalizePath(t_args[1]) : normalizePath(baseName(local));
    if (remote == "/" || (t_args.size() > 1 && (t_args[1] == "." || t_args[1] == ".." || t_args[1].back() == '/'))) {
        remote = normalizePath(remote + "/" + baseName(local));
    } else {
        auto hit = resolve(remote);
        if (hit && hit->is_dir) {
            remote = normalizePath(remote + "/" + baseName(local));
        }
    }
    if (!storage_ || !storage_->upload(remote, local, scope_)) {
        fmt::print(stderr, "put: {} -> {}: upload failed\n", local, remote);
        return 1;
    }
    return 0;
}

int DatastoreShell::cmdMkdir(const std::vector<std::string>& t_args) {
    if (t_args.empty()) {
        fmt::print(stderr, "mkdir: missing operand\n");
        return 1;
    }
    const std::string path = normalizePath(t_args[0]);
    if (!storage_ || !storage_->createDirectory(path, scope_)) {
        fmt::print(stderr, "mkdir: cannot create directory '{}'\n", path);
        return 1;
    }
    return 0;
}

int DatastoreShell::cmdMv(const std::vector<std::string>& t_args) {
    if (t_args.size() < 2) {
        fmt::print(stderr, "mv: missing operand (mv <src> <dst>)\n");
        return 1;
    }
    const std::string src = normalizePath(t_args[0]);
    const std::string dst = normalizePath(t_args[1]);
    auto hit = resolve(src);
    if (!hit) {
        fmt::print(stderr, "mv: {}: No such file or directory\n", src);
        return 1;
    }
    if (!requireId("mv", src, *hit))
        return 1;
    std::string parent;
    std::string new_name;
    if (auto dst_hit = resolve(dst); dst_hit && dst_hit->is_dir) {
        parent = dst;
        new_name = baseName(src);
    } else {
        parent = dirName(dst);
        new_name = baseName(dst);
    }
    std::optional<int64_t> parent_id;
    if (parent != "/") {
        auto parent_hit = resolve(parent);
        if (!parent_hit || !parent_hit->is_dir) {
            fmt::print(stderr, "mv: {}: No such directory\n", parent);
            return 1;
        }
        parent_id = parent_hit->id;
    }
    if (!storage_ || !storage_->moveItem(*hit->id, hit->type, new_name, parent_id, scope_)) {
        fmt::print(stderr, "mv: {} -> {}: failed\n", src, dst);
        return 1;
    }
    return 0;
}

int DatastoreShell::cmdRm(const std::vector<std::string>& t_args) {
    bool recursive = false;
    std::vector<std::string> paths;
    for (const auto& a : t_args) {
        if (a == "-r" || a == "-R" || a == "--recursive")
            recursive = true;
        else
            paths.push_back(a);
    }
    if (paths.empty()) {
        fmt::print(stderr, "rm: missing operand (rm [-r] <path>)\n");
        return 1;
    }
    int rc = 0;
    for (const auto& p : paths) {
        const std::string path = normalizePath(p);
        if (path == "/") {
            fmt::print(stderr, "rm: refusing to remove root\n");
            rc = 1;
            continue;
        }
        auto hit = resolve(path);
        if (!hit) {
            fmt::print(stderr, "rm: {}: No such file or directory\n", path);
            rc = 1;
            continue;
        }
        if (hit->is_dir && !recursive) {
            fmt::print(stderr, "rm: {}: is a directory (use -r)\n", path);
            rc = 1;
            continue;
        }
        if (!requireId("rm", path, *hit)) {
            rc = 1;
            continue;
        }
        if (!storage_ || !storage_->deleteItem(*hit->id, hit->type, scope_)) {
            fmt::print(stderr, "rm: {}: failed\n", path);
            rc = 1;
        }
    }
    return rc;
}

int DatastoreShell::cmdTree(const std::vector<std::string>& t_args) {
    const std::string path = t_args.empty() ? cwd_ : normalizePath(t_args[0]);
    fmt::print("{}\n", path);
    bool ok = true;
    treeRecursive(path, "", ok);
    return ok ? 0 : 1;
}

int DatastoreShell::cmdDu(const std::vector<std::string>& t_args) {
    const std::string path = t_args.empty() ? cwd_ : normalizePath(t_args[0]);
    bool ok = true;
    const uint64_t total = duRecursive(path, ok);
    fmt::print("{} {}\n", humanBytes(static_cast<int64_t>(total)), path);
    return ok ? 0 : 1;
}

int DatastoreShell::cmdSession() {
    fmt::print("url: {}\n", url_.empty() ? "(not connected)" : url_);
    fmt::print("auth: {}\n", connected() ? auth_desc_ : "-");
    fmt::print("session: {}\n", connected() && client_->hasSessionToken() ? "active" : "none");
    fmt::print("scope: {}\n", sgrn::datastore::client::detail::storageScopeToString(scope_));
    fmt::print("cwd: {}\n", cwd_);
    return 0;
}

int DatastoreShell::cmdScope(const std::vector<std::string>& t_args) {
    if (t_args.empty()) {
        fmt::print("scope: {}\n", sgrn::datastore::client::detail::storageScopeToString(scope_));
        return 0;
    }
    auto parsed = parseScope(t_args[0]);
    if (!parsed) {
        fmt::print(stderr, "scope: unknown scope '{}' (personal|users|automated-services|domain|auto)\n", t_args[0]);
        return 1;
    }
    scope_ = *parsed;
    return 0;
}

int DatastoreShell::cmdZip(const std::vector<std::string>& t_args) {
    if (t_args.empty()) {
        fmt::print(stderr, "zip: missing remote path (zip <remote-dir> [local.zip])\n");
        return 1;
    }
    const std::string remote = normalizePath(t_args[0]);
    const std::string local = (t_args.size() > 1) ? t_args[1] : baseName(remote) + ".zip";
    if (!storage_ || !storage_->downloadZip(remote, local, scope_)) {
        fmt::print(stderr, "zip: {}: failed\n", remote);
        return 1;
    }
    return 0;
}

void DatastoreShell::printHelp() {
    fmt::print("datastore-shell — navigate the SGRN datastore API like a filesystem & AngelScript REPL.\n"
               "\n"
               "Unix-style Commands:\n"
               "  connect [--url URL] [--token T --secret S | --email E [--password P] | --session-token S] [URL]\n"
               "                                               sign in (mode picked from credentials)\n"
               "  login [--email E]                            interactive user sign-in (prompts, password hidden)\n"
               "  orgs                                           list organisations\n"
               "  domains <ORG>                                  list an organisation's domains\n"
               "  pwd                                                print remote working directory\n"
               "  ls [PATH]                                          list folders/ and files\n"
               "  cd <PATH>                                          change directory (.. and . work, never above /)\n"
               "  cat <FILE>                                         print a remote file to stdout\n"
               "  get <REMOTE> [LOCAL]                               download (default: basename)\n"
               "  put <LOCAL> [REMOTE]                               upload (default remote: cwd/basename)\n"
               "  mkdir <PATH>                                       create a folder\n"
               "  mv <SRC> <DST>                                     move/rename (dst dir keeps basename)\n"
               "  rm [-r] <PATH>                                     delete (refuses /; dirs need -r)\n"
               "  tree [PATH]                                        recursive listing\n"
               "  du [PATH]                                          recursive byte total\n"
               "  session                                            show url, auth, session and cwd\n"
               "  logout                                             revoke server session, drop local state\n"
               "  passwd [--old O --new N]                           change password (prompts hidden, min 8 chars)\n"
               "  whoami                                             show current user info as JSON\n"
               "  statuses <ORG>                                     valid account statuses for an org\n"
               "  users                                              list users (admin)\n"
               "  useradd --first F --family F --email E --password P --org O --status S [--phone P --domain D]\n"
               "                                               register a user (admin)\n"
               "  services                                           list automated services, tokens masked (admin)\n"
               "  service-add --name N [--org O --kind K --domain D]  register a service, prints credentials once (admin)\n"
               "  service-token-rotate <ID-or-NAME>                  rotate a service token, prints once (admin)\n"
               "  stats                                              storage stats as JSON\n"
               "  info | constraints                                 storage info/constraints as JSON\n"
               "  zip <REMOTE-DIR> [LOCAL.zip]                       download a folder as zip\n"
               "  scope [personal|users|automated-services|domain|auto]\n"
               "                                               show/switch namespace scope\n"
               "  !<cmd>                                             execute local shell command\n"
               "  help                                               this text\n"
               "  quit | exit                                        leave the shell\n"
               "\n"
               "AngelScript Functions & Admin API:\n"
               "  connectService(url, token, secret)\n"
               "  loginUser(url, email, password)\n"
               "  logout()\n"
               "  createUser(first, family, email, password, org, status) -> string\n"
               "  listUsers()\n"
               "  createService(name, org) -> string\n"
               "  listServices()\n"
               "  rotateServiceToken(nameOrId) -> string\n"
               "  listOrgs()\n"
               "  listDomains(org)\n"
               "  listStatuses(org)\n"
               "  whoami() -> string\n"
               "  storageStats() -> string\n"
               "  storageInfo() -> string\n"
               "  changePassword(oldPw, newPw)\n"
               "  setScope(scopeName)\n"
               "  getScope() -> string\n"
               "  currentPath() -> string\n"
               "  sessionInfo()\n"
               "  isConnected() -> bool\n"
               "\n"
               "Script Execution: datastore-shell script.as [args...]\n"
               "Env: SGRN_DATASTORE_URL/TOKEN/SECRET/EMAIL/PASSWORD/SESSION_TOKEN\n");
}

struct CommandDispatchEntry {
    std::string_view name;
    bool needs_session;
    std::function<int(DatastoreShell*, const std::vector<std::string>&)> handler;
};

int DatastoreShell::dispatch(const std::vector<std::string>& t_words) {
    if (t_words.empty())
        return 0;

    static const std::array<CommandDispatchEntry, 36> kCommandTable = {{{"help", false,
                                                                            [](DatastoreShell* shell, const std::vector<std::string>&) {
                                                                                shell->printHelp();
                                                                                return 0;
                                                                            }},
        {"-h", false,
            [](DatastoreShell* shell, const std::vector<std::string>&) {
                shell->printHelp();
                return 0;
            }},
        {"--help", false,
            [](DatastoreShell* shell, const std::vector<std::string>&) {
                shell->printHelp();
                return 0;
            }},
        {"quit", false, [](DatastoreShell*, const std::vector<std::string>&) { return -1; }},
        {"exit", false, [](DatastoreShell*, const std::vector<std::string>&) { return -1; }},
        {"connect", false, [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdConnect(args); }},
        {"open", false, [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdConnect(args); }},
        {"login", false, [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdLogin(args); }},
        {"pwd", false,
            [](DatastoreShell* shell, const std::vector<std::string>&) {
                fmt::print("{}\n", shell->cwd_);
                return 0;
            }},
        {"session", false, [](DatastoreShell* shell, const std::vector<std::string>&) { return shell->cmdSession(); }},
        {"scope", false, [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdScope(args); }},
        {"ls", true, [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdLs(args); }},
        {"cd", true, [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdCd(args); }},
        {"cat", true, [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdCat(args); }},
        {"get", true, [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdGet(args); }},
        {"put", true, [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdPut(args); }},
        {"mkdir", true, [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdMkdir(args); }},
        {"mv", true, [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdMv(args); }},
        {"rm", true, [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdRm(args); }},
        {"tree", true, [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdTree(args); }},
        {"du", true, [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdDu(args); }},
        {"logout", true, [](DatastoreShell* shell, const std::vector<std::string>&) { return shell->cmdLogout(); }},
        {"passwd", true, [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdPasswd(args); }},
        {"whoami", true, [](DatastoreShell* shell, const std::vector<std::string>&) { return shell->cmdWhoami(); }},
        {"orgs", true, [](DatastoreShell* shell, const std::vector<std::string>&) { return shell->cmdOrgs(); }},
        {"domains", true, [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdDomains(args); }},
        {"statuses", true, [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdStatuses(args); }},
        {"users", true, [](DatastoreShell* shell, const std::vector<std::string>&) { return shell->cmdUsers(); }},
        {"useradd", true, [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdUseradd(args); }},
        {"services", true, [](DatastoreShell* shell, const std::vector<std::string>&) { return shell->cmdServices(); }},
        {"service-add", true, [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdServiceAdd(args); }},
        {"service-token-rotate", true,
            [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdServiceRotate(args); }},
        {"stats", true, [](DatastoreShell* shell, const std::vector<std::string>&) { return shell->cmdStats(); }},
        {"info", true, [](DatastoreShell* shell, const std::vector<std::string>&) { return shell->cmdInfo(); }},
        {"constraints", true, [](DatastoreShell* shell, const std::vector<std::string>&) { return shell->cmdInfo(); }},
        {"zip", true, [](DatastoreShell* shell, const std::vector<std::string>& args) { return shell->cmdZip(args); }}}};

    const std::string& cmd = t_words[0];
    for (const auto& entry : kCommandTable) {
        if (entry.name == cmd) {
            if (entry.needs_session && !connected()) {
                fmt::print(stderr, "not connected (connect --url URL --token TOKEN, or login --email EMAIL)\n");
                return 1;
            }
            const std::vector<std::string> args(t_words.begin() + 1, t_words.end());
            return entry.handler(this, args);
        }
    }

    // Command not recognized as built-in shell command -> return 127 for AngelScript fallback
    return 127;
}

void DatastoreShell::runShellCommand(const std::string& t_cmd) {
    if (t_cmd.empty())
        return;
    int rc = std::system(t_cmd.c_str());
    (void)rc;
}

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
    engine->RegisterGlobalFunction(
        "void listDomains(const string &in)", asMETHOD(DatastoreShell, as_listDomains), asCALL_THISCALL_ASGLOBAL, this);
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
}

void DatastoreShell::as_connectService(const std::string& t_url, const std::string& t_token, const std::string& t_secret) {
    cmdConnect({"--url", t_url, "--token", t_token, "--secret", t_secret});
}

void DatastoreShell::as_loginUser(const std::string& t_url, const std::string& t_email, const std::string& t_password) {
    cmdLogin({"--url", t_url, "--email", t_email, "--password", t_password});
}

void DatastoreShell::as_logout() {
    cmdLogout();
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
    cmdUsers();
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
    cmdServices();
}

std::string DatastoreShell::as_rotateServiceToken(const std::string& t_name_or_id) {
    if (!client_)
        return "error: not connected";
    bool ok = false;
    int64_t id = resolveServiceId(t_name_or_id, ok);
    if (!ok || id <= 0)
        return "error: invalid service";
    auto r = client_->rotateServiceToken(id);
    if (r.hasError())
        return "error: " + r.error();
    return fmt::format("{} | token: {} secret: {}", r.value().message_, r.value().token_, r.value().token_secret_);
}

void DatastoreShell::as_listOrgs() {
    cmdOrgs();
}

void DatastoreShell::as_listDomains(const std::string& t_org) {
    cmdDomains({t_org});
}

void DatastoreShell::as_listStatuses(const std::string& t_org) {
    cmdStatuses({t_org});
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
    cmdPasswd({"--old", t_old_pw, "--new", t_new_pw});
}

void DatastoreShell::as_setScope(const std::string& t_name) {
    cmdScope({t_name});
}

std::string DatastoreShell::as_getScope() {
    return std::string(sgrn::datastore::client::detail::storageScopeToString(scope_));
}

std::string DatastoreShell::as_currentPath() {
    return cwd_;
}

void DatastoreShell::as_sessionInfo() {
    cmdSession();
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

} // namespace sgrn::datastore::shell
