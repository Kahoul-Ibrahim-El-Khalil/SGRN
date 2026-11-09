// pipeline.cpp — process piping, REPL variable assignment and local commands.
#include "shell-common.hpp"
namespace sgrn::datastore::shell
{

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
            } else if (token.find('.') != std::string::npos || token.find('/') != std::string::npos || token[0] == '~') {
                target_file = expandLocalPath(token);
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

void DatastoreShell::runShellCommand(const std::string& t_cmd) {
    if (t_cmd.empty())
        return;
    int rc = std::system(t_cmd.c_str());
    (void)rc;
}

} // namespace sgrn::datastore::shell
