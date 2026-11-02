#pragma once

// test_util.hpp — shared host-side helpers for C++ integration tests.
//
// Process spawning uses reproc (cross-platform C99/C++11 child-process
// control, linked via cmake/deps/reproc.cmake) instead of raw fork/exec/
// waitpid, so these helpers also compile on Windows; the only remaining
// platform-specific helper is freePort() (raw sockets). The public API
// (spawnProcess / runProcess / Process) is unchanged — see each declared
// below. Fixture headers pg_fixture + gateway_fixture build on these.
//
// Conventions shared with tests/run_tests.py:
//   - a fixture that cannot start sets `available() == false` + `skipReason()`
//     and the test prints "SKIP: <reason>" then returns 0 (ctest PASS),
//     matching the Python/TS skip semantics.

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <reproc++/reproc.hpp>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <system_error> // std::error_code (reproc++)

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

// ── Process management (reproc backend) ──────────────────────────────────────

struct SpawnResult {
    ::pid_t pid = -1;
    std::string error;
    /// Backend ownership: the started child's control object. Null when the
    /// spawn failed (in which case `pid < 0` and `error` describe why).
    std::unique_ptr<reproc::process> proc;
};

/// Start a child with stdout+stderr redirected (single shared fd, so the two
/// streams append sequentially) to t_log_path, or to /dev/null when empty; stdin
/// is connected to /dev/null. `t_env_extra` entries are added on top of the
/// inherited environment. Returns the child pid on success, pid == -1 + `error`
/// on failure. The returned SpawnResult must be handed to a `Process` (or its
/// moved-into control object dropped) so the child is always reaped.
inline SpawnResult spawnProcess(const std::vector<std::string>& t_argv, const std::string& t_cwd,
    const std::map<std::string, std::string>& t_env_extra, const std::string& t_log_path = {}) {
    SpawnResult res;
    if (t_argv.empty()) {
        res.error = "cannot start: empty argv";
        return res;
    }

    std::FILE* log_fp = nullptr;
    if (!t_log_path.empty()) {
        log_fp = std::fopen(t_log_path.c_str(), "wb");
        if (!log_fp) {
            res.error = std::string("fopen(") + t_log_path + "): " + std::strerror(errno);
            return res;
        }
    }

    reproc::options options;
    options.env.behavior = reproc::env::extend;
    if (!t_env_extra.empty())
        options.env.extra = reproc::env(t_env_extra);
    if (!t_cwd.empty())
        options.working_directory = t_cwd.c_str();
    // Child stdin is always /dev/null (immediate EOF — nothing reads it).
    options.redirect.in.type = reproc::redirect::discard;
    if (log_fp) {
        // Top-level redirect shorthand: the same FILE* backs both stdout and
        // stderr, so their writes append into one growing file (own-fd-per-
        // stream via redirect::path would interleave two file offsets).
        options.redirect.file = log_fp;
    } else {
        options.redirect.out.type = reproc::redirect::discard;
        options.redirect.err.type = reproc::redirect::discard;
    }
    // Shutdown escalation consumed by Process::stop() and the destructor.
    options.stop = {
        {reproc::stop::terminate, reproc::milliseconds(3000)},
        {reproc::stop::kill, reproc::infinite},
        {reproc::stop::noop, reproc::milliseconds(0)},
    };

    auto proc = std::make_unique<reproc::process>();
    if (const std::error_code ec = proc->start(t_argv, options)) {
        res.error = std::string("cannot start child: ") + ec.message();
        if (log_fp)
            std::fclose(log_fp);
        return res;
    }
    if (log_fp)
        std::fclose(log_fp); // the child holds its own dup; parent's copy can close
    res.pid = proc->pid().first;
    res.proc = std::move(proc);
    return res;
}

/// Run a short-lived helper to completion synchronously; returns the exit
/// status (127 mirrors exec failure) and a description on spawn/wait failure.
/// Statuses >= 128 report "killed by signal N" (reproc reports WTERMSIG+128);
/// a genuine application exit in that range would also map into the message.
inline int runProcess(const std::vector<std::string>& t_argv, const std::string& t_cwd,
    const std::map<std::string, std::string>& t_env_extra, std::string& t_out_error) {
    SpawnResult res = spawnProcess(t_argv, t_cwd, t_env_extra);
    if (res.pid < 0) {
        t_out_error = res.error;
        return -1;
    }
    const auto [status, ec] = res.proc->wait(reproc::infinite);
    if (ec) {
        t_out_error = std::string("reproc wait: ") + ec.message();
        return -1;
    }
    if (status < 0) {
        t_out_error = std::string("reproc wait: unexpected negative status ") + std::to_string(status);
        return -1;
    }
    if (status >= 128)
        t_out_error = std::string("killed by signal ") + std::to_string(status - 128);
    return status;
}

/// RAII child process: terminate on destruction (SIGTERM → ~3s → SIGKILL,
/// escalated by reproc's stored stop actions or this class's explicit stop()).
class Process {
public:
    Process() = default;
    explicit Process(SpawnResult t_res)
        : proc_(std::move(t_res.proc))
        , pid_(t_res.pid) {
    }

    ~Process() {
        stop();
    }
    Process(Process&& t_other) noexcept
        : proc_(std::exchange(t_other.proc_, nullptr))
        , pid_(std::exchange(t_other.pid_, -1)) {
    }
    Process& operator=(Process&& t_other) noexcept {
        if (this != &t_other) {
            stop();
            proc_ = std::exchange(t_other.proc_, nullptr);
            pid_ = std::exchange(t_other.pid_, -1);
        }
        return *this;
    }
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    /// Non-blocking liveness probe; reaps an exited child as a side effect.
    bool live() {
        if (!proc_)
            return false;
        const auto [status, ec] = proc_->wait(reproc::milliseconds(0));
        if (status >= 0)
            return false;                  // exited (possibly just reaped by this call)
        return ec == std::errc::timed_out; // still running
    }
    ::pid_t pid() const {
        return pid_;
    }

    /// Terminate (idempotent): SIGTERM, wait up to ~3s, then SIGKILL.
    void stop() {
        if (!proc_)
            return;
        auto proc = std::exchange(proc_, nullptr);
        pid_ = -1;
        (void)proc->stop({
            {reproc::stop::terminate, reproc::milliseconds(3000)},
            {reproc::stop::kill, reproc::infinite},
            {reproc::stop::noop, reproc::milliseconds(0)},
        });
    }

private:
    std::unique_ptr<reproc::process> proc_;
    ::pid_t pid_ = -1;
};

} // namespace sgrn::test
