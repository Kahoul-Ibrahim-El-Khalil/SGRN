// sgrn/lib/datastore/src/bootstrap/secrets.cpp
//
// Secret-material primitives (CSPRNG generation, no-echo prompts, .env
// surgery) — definitions for include/sgrn/datastore/bootstrap/secrets.hpp.

#include <sgrn/datastore/bootstrap/secrets.hpp>

#include <fmt/core.h>
#include <sgrn/utils/env.hpp>
#include <cerrno>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <random>
#include <sys/random.h>
#include <termios.h>
#include <unistd.h>

namespace sgrn::datastore::bootstrap
{

// ---------------------------------------------------------------------------
// Generation
// ---------------------------------------------------------------------------

std::string generateSecretHex(size_t t_bytes) {
    std::vector<unsigned char> buf(t_bytes);
    size_t got = 0;
    while (got < t_bytes) {
        ssize_t n = ::getrandom(buf.data() + got, t_bytes - got, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::random_device rd; // getrandom unavailable (exotic kernel)
            while (got < t_bytes) {
                buf[got++] = rd();
            }
            break;
        }
        got += static_cast<size_t>(n);
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(t_bytes * 2);
    for (unsigned char byte : buf) {
        hex.push_back(kHex[byte >> 4]);
        hex.push_back(kHex[byte & 0xF]);
    }
    return hex;
}

// ---------------------------------------------------------------------------
// Prompts
// ---------------------------------------------------------------------------

std::string promptLine(const std::string& t_prompt) {
    fmt::print("{}", t_prompt);
    std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line)) {
        std::cin.clear();
        return {};
    }
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.pop_back();
    }
    return line;
}

std::string promptSecret(const std::string& t_prompt) {
    fmt::print("{}", t_prompt);
    std::fflush(stdout);
    std::string line;
    if (::isatty(STDIN_FILENO) == 0) {
        if (std::getline(std::cin, line)) {
            return line;
        }
        std::cin.clear();
        return {};
    }
    ::termios old_term{};
    if (::tcgetattr(STDIN_FILENO, &old_term) != 0) {
        std::getline(std::cin, line); // not a controllable terminal anyway
        return line;
    }
    ::termios hidden = old_term;
    hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
    ::tcsetattr(STDIN_FILENO, TCSANOW, &hidden);
    const bool read_line = static_cast<bool>(std::getline(std::cin, line));
    ::tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
    fmt::print("\n");
    if (!read_line) {
        std::cin.clear();
        line.clear();
    }
    while (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return line;
}

// ---------------------------------------------------------------------------
// .env surgery
// ---------------------------------------------------------------------------

bool upsertEnvVar(const std::filesystem::path& t_env_path, const std::string& t_key, const std::string& t_value) {
    std::ifstream in(t_env_path);
    if (!in) {
        return false;
    }
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string prefix = t_key + "=";
    bool changed = false;
    {
        std::string out;
        size_t pos = 0;
        while (pos < content.size()) {
            size_t eol = content.find('\n', pos);
            std::string line = content.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
            if (!changed && line.rfind(prefix, 0) == 0) {
                line = prefix + t_value;
                changed = true;
            }
            out += line;
            if (eol == std::string::npos) {
                break;
            }
            out += '\n';
            pos = eol + 1;
        }
        if (!changed) {
            if (!out.empty() && out.back() != '\n') {
                out += '\n';
            }
            out += prefix + t_value + "\n";
            changed = true;
        }
        content = std::move(out);
    }
    std::ofstream out_file(t_env_path, std::ios::binary | std::ios::trunc);
    if (!out_file) {
        return false;
    }
    out_file << content;
    return changed;
}

void resolveEnvPlaceholderKey(const std::filesystem::path& t_env_path, const std::string& t_key) {
    const std::string value = sgrn::utils::env::get(t_key.c_str(), "");
    static constexpr std::string_view kNeedle = "$ENV{";
    const size_t pos = value.find(kNeedle);
    if (pos == std::string::npos) {
        return;
    }
    const size_t close = value.find('}', pos);
    if (close == std::string::npos) {
        return;
    }
    const std::string inner = value.substr(pos + kNeedle.size(), close - pos - kNeedle.size());
    const std::string var_name = inner.empty() ? "CONDA_PREFIX" : inner;
    const std::string var_value = sgrn::utils::env::get(var_name.c_str(), "");
    if (var_value.size() < 2) {
        return; // nothing usable to substitute (no active micromamba env)
    }
    const std::string resolved = value.substr(0, pos) + var_value + value.substr(close + 1);
    if (resolved == value) {
        return;
    }
    ::setenv(t_key.c_str(), resolved.c_str(), 1);
    upsertEnvVar(t_env_path, t_key, resolved);
}
} // namespace sgrn::datastore::bootstrap
