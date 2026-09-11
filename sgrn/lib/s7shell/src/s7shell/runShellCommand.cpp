#include <sgrn/s7shell/S7Shell.hpp>

namespace sgrn::s7shell::shell
{
void S7Shell::runShellCommand(const std::string& t_cmd) {
    /*
    if (!shell_escapes_enabled_) {
        fmt::print(stderr, fg(fmt::color::red), "[s7shell] Shell escapes are disabled (start with --allow-shell to enable)\n");
        return;
    }
    */
    fmt::print(fg(fmt::color::gray), "$ {}\n", t_cmd);

#ifdef _WIN32
    FILE* p_pipe = _popen(t_cmd.c_str(), "r");
#else
    FILE* p_pipe = popen(t_cmd.c_str(), "r");
#endif
    if (!p_pipe) {
        fmt::print(stderr, fg(fmt::color::red), "[s7shell] Failed to launch shell command\n");
        return;
    }
    char buf[4096];
    while (fgets(buf, sizeof(buf), p_pipe)) {
        fmt::print("{}", buf);
    }
#ifdef _WIN32
    int status = _pclose(p_pipe);
#else
    int status = pclose(p_pipe);
    if (WIFEXITED(status)) {
        status = WEXITSTATUS(status);
    }
#endif
    last_shell_exit_code_ = status;
    if (status != 0) {
        fmt::print(fg(fmt::color::yellow), "[s7shell] (exit code {})\n", status);
    }
}

} // namespace sgrn::s7shell::shell
