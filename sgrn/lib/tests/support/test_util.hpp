#pragma once

// test_util.hpp — shared host-side helpers for C++ integration tests.
//
// Zero-dependency (POSIX + stdlib only) so any component's tests can include
// this header without pulling in SGRN libraries or a test framework. Fixture
// headers (pg_fixture.hpp, gateway_fixture.hpp) build on these primitives.
//
// Conventions shared with tests/run_tests.py:
//   - a fixture that cannot start sets `available() == false` + `skipReason()`
//     and the test prints "SKIP: <reason>" then returns 0 (ctest PASS),
//     matching the Python/TS skip semantics.

#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace sgrn::test
{

// ── Filesystem ───────────────────────────────────────────────────────────────

inline bool pathExists(const std::string& t_path) {
    return ::access(t_path.c_str(), F_OK) == 0;
}

inline bool isDirectory(const std::string& t_path) {
    struct ::stat st;
    return ::stat(t_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

/// Repository root. Honours SGRN_REPO_ROOT, else walks up from the test
/// binary's real path looking for the repo marker (CMakePresets.json) so a
/// ctest-invoked binary works from any build directory.
inline std::string repoRoot() {
    if (const char* p_env = ::getenv("SGRN_REPO_ROOT"); p_env && *p_env)
        return p_env;

    namespace fs = std::filesystem;
    char buf[4096] = {};
    const ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len <= 0)
        return {};
    fs::path dir = fs::path(std::string_view(buf, static_cast<std::size_t>(len))).parent_path();
    while (!dir.empty()) {
        if (pathExists((dir / "CMakePresets.json").string()))
            return dir.string();
        if (!dir.has_parent_path() || dir == dir.parent_path())
            break;
        dir = dir.parent_path();
    }
    return {};
}

/// mkdtemp under /tmp with the given prefix; throws std::runtime_error on
/// failure (a test that cannot create a temp dir cannot meaningfully run).
inline std::string makeTempDir(const std::string& t_prefix) {
    std::string tpl = "/tmp/" + t_prefix + "-XXXXXX";
    std::vector<char> buf(tpl.begin(), tpl.end());
    buf.push_back('\0');
    if (!::mkdtemp(buf.data()))
        throw std::runtime_error(std::string("mkdtemp failed: ") + std::strerror(errno));
    return buf.data();
}

/// Read/write whole small text files (config templates for child processes).
inline std::string readFile(const std::string& t_path) {
    std::FILE* fp = std::fopen(t_path.c_str(), "rb");
    if (!fp)
        throw std::runtime_error(std::string("fopen(") + t_path + "): " + std::strerror(errno));
    std::string out;
    char buf[8192];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0)
        out.append(buf, n);
    std::fclose(fp);
    return out;
}

inline void writeFile(const std::string& t_path, std::string_view t_contents) {
    std::FILE* fp = std::fopen(t_path.c_str(), "wb");
    if (!fp)
        throw std::runtime_error(std::string("fopen(") + t_path + "): " + std::strerror(errno));
    if (!t_contents.empty())
        std::fwrite(t_contents.data(), 1, t_contents.size(), fp);
    std::fclose(fp);
}

// ── Networking ───────────────────────────────────────────────────────────────

/// Bind a loopback socket to :0, read the OS-assigned port, close the socket
/// and return it (0 on failure). The classic bind-then-release race with
/// another process claiming the same port is narrow and shared by every
/// subprocess-based fixture; consumers must treat bind failure as retryable.
inline int freePort() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return 0;
    ::sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<::sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 1) != 0) {
        ::close(fd);
        return 0;
    }
    ::socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<::sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return 0;
    }
    const int port = ::ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

// ── Process management ───────────────────────────────────────────────────────

struct SpawnResult {
    ::pid_t pid = -1;
    std::string error;
};

/// fork/exec with stdout+stderr redirected to t_log_path (empty → /dev/null).
/// `t_env_extra` entries are added to the inherited environment; neither the
/// parent's environment nor the child's argv strings are retained by callers
/// after this returns (execv permutes nothing — our own copies).
inline SpawnResult spawnProcess(const std::vector<std::string>& t_argv, const std::string& t_cwd,
    const std::map<std::string, std::string>& t_env_extra, const std::string& t_log_path = {}) {
    SpawnResult res;

    // Build argv/env once so the child only does execv (no allocation-wise
    // fork pitfalls beyond strdup, which is fine pre-exec in a single thread).
    std::vector<char*> argv_c;
    argv_c.reserve(t_argv.size() + 1);
    for (const std::string& a : t_argv)
        argv_c.push_back(::strdup(a.c_str()));
    argv_c.push_back(nullptr);

    // Environment: inherited + extras
    std::vector<std::string> env_entries;
    for (char** e = ::environ; e && *e; ++e)
        env_entries.emplace_back(*e);
    for (const auto& [k, v] : t_env_extra)
        env_entries.emplace_back(k + "=" + v);

    std::vector<char*> env_c;
    env_c.reserve(env_entries.size() + 1);
    for (const std::string& e : env_entries)
        env_c.push_back(::strdup(e.c_str()));
    env_c.push_back(nullptr);

    const int redirect_fd =
        t_log_path.empty() ? ::open("/dev/null", O_WRONLY) : ::open(t_log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (redirect_fd < 0) {
        res.error = std::string("open(log): ") + std::strerror(errno);
        return res;
    }

    const ::pid_t pid = ::fork();
    if (pid < 0) {
        res.error = std::string("fork: ") + std::strerror(errno);
        ::close(redirect_fd);
        return res;
    }
    if (pid == 0) {
        // child
        ::close(STDIN_FILENO);
        ::open("/dev/null", O_RDONLY);
        ::dup2(redirect_fd, STDOUT_FILENO);
        ::dup2(redirect_fd, STDERR_FILENO);
        ::close(redirect_fd);
        if (!t_cwd.empty() && ::chdir(t_cwd.c_str()) != 0)
            _exit(127);
        ::execv(argv_c[0], argv_c.data());
        _exit(127); // execv failed — binary missing or not executable
    }

    ::close(redirect_fd);
    for (char* a : argv_c)
        if (a)
            ::free(a);
    for (char* e : env_c)
        if (e)
            ::free(e);

    res.pid = pid;
    return res;
}

/// Run a short-lived helper to completion synchronously; returns the exit
/// status (127 mirrors execv failure) and a description on spawn failure.
inline int runProcess(const std::vector<std::string>& t_argv, const std::string& t_cwd,
    const std::map<std::string, std::string>& t_env_extra, std::string& t_out_error) {
    SpawnResult res = spawnProcess(t_argv, t_cwd, t_env_extra);
    if (res.pid < 0) {
        t_out_error = res.error;
        return -1;
    }
    int status = 0;
    if (::waitpid(res.pid, &status, 0) < 0) {
        t_out_error = std::string("waitpid: ") + std::strerror(errno);
        return -1;
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        t_out_error = std::string("killed by signal ") + std::to_string(WTERMSIG(status));
    return -1;
}

/// RAII child process: SIGTERM on destruction (SIGKILL after a grace period).
class Process {
public:
    Process() = default;
    explicit Process(SpawnResult t_res)
        : pid_(t_res.pid) {
    }

    ~Process() {
        stop();
    }
    Process(Process&& t_other) noexcept
        : pid_(std::exchange(t_other.pid_, -1)) {
    }
    Process& operator=(Process&& t_other) noexcept {
        if (this != &t_other) {
            stop();
            pid_ = std::exchange(t_other.pid_, -1);
        }
        return *this;
    }
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    bool live() const {
        if (pid_ <= 0)
            return false;
        // /proc/<pid>/stat state field: 'Z' = zombie (exited, unreaped).
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/stat", pid_);
        std::FILE* fp = std::fopen(path, "r");
        if (!fp)
            return false;
        int pid_no = 0;
        char comm[256] = {}, state = 0;
        const bool ok = std::fscanf(fp, "%d %255[^)] %c", &pid_no, comm, &state) == 3;
        std::fclose(fp);
        return ok && state != 'Z' && state != 'X';
    }
    ::pid_t pid() const {
        return pid_;
    }

    /// Terminate (idempotent): SIGTERM, wait up to ~3s, then SIGKILL.
    void stop() {
        if (pid_ <= 0)
            return;
        const ::pid_t pid = pid_;
        pid_ = -1;
        if (::kill(pid, SIGTERM) != 0)
            return;
        for (int i = 0; i < 600; ++i) {
            const ::pid_t w = ::waitpid(pid, nullptr, WNOHANG);
            if (w == pid)
                return;
            if (w < 0 && errno != EINTR && errno != 0)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ::kill(pid, SIGKILL);
        ::waitpid(pid, nullptr, 0);
    }

private:
    ::pid_t pid_ = -1;
};

} // namespace sgrn::test
