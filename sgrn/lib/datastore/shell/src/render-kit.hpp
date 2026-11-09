#pragma once
#include <fmt/color.h>
#include <fmt/core.h>
#include <algorithm>
#include <cstddef>
#include <json/json.h>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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
// ── terminal rendering kit ─────────────────────────────────────────────────
// fmt does NOT gate colors on TTY itself, so styles below are explicit:
// plain text unless stdout is a terminal (with NO_COLOR unset and a sane
// TERM). Pipes, redirects and scripts always see clean output. Tables
// measure UTF-8 code points (not bytes) so multibyte names stay aligned.
inline bool useColor() {
#ifndef _WIN32
    if (!isatty(STDOUT_FILENO))
        return false;
#endif
    if (std::getenv("NO_COLOR") != nullptr)
        return false;
    const char* term = std::getenv("TERM");
    return term == nullptr || std::string(term) != "dumb";
}

inline fmt::text_style styleHeader() {
    return useColor() ? (fg(fmt::color::white) | fmt::emphasis::bold) : fmt::text_style{};
}

inline fmt::text_style styleDir() {
    return useColor() ? fg(fmt::color::cyan) : fmt::text_style{};
}

inline fmt::text_style styleOk() {
    return useColor() ? fg(fmt::color::green) : fmt::text_style{};
}

inline fmt::text_style styleWarn() {
    return useColor() ? fg(fmt::color::yellow) : fmt::text_style{};
}

// Visible width of a string in terminal columns (approximate: one column
// per Unicode code point; CJK wide chars will be one short, which is
// acceptable for a debug shell).
inline size_t displayWidth(std::string_view t_text) {
    size_t width = 0;
    for (unsigned char c : t_text) {
        if ((c & 0xC0) != 0x80)
            ++width;
    }
    return width;
}

inline std::string padCell(std::string_view t_text, size_t t_width, bool t_right) {
    const size_t pad = t_width > displayWidth(t_text) ? t_width - displayWidth(t_text) : 0;
    if (t_right)
        return std::string(pad, ' ') + std::string(t_text);
    return std::string(t_text) + std::string(pad, ' ');
}

// Aligned columnar output with a bold header and a rule line.
// t_right lists column indices to right-align (ids, sizes).
// Prints nothing when there are no rows (same contract as before).
inline void printTable(std::vector<std::string> t_headers, std::vector<std::vector<std::string>> t_rows, std::vector<size_t> t_right = {}) {
    if (t_rows.empty())
        return;
    const size_t cols = t_headers.size();
    std::vector<size_t> widths(cols, 0);
    for (size_t i = 0; i < cols; ++i)
        widths[i] = displayWidth(t_headers[i]);
    for (const auto& row : t_rows) {
        for (size_t i = 0; i < cols && i < row.size(); ++i)
            widths[i] = std::max(widths[i], displayWidth(row[i]));
    }
    auto is_right = [&](size_t t_i) { return std::find(t_right.begin(), t_right.end(), t_i) != t_right.end(); };
    std::string header_line, rule_line;
    for (size_t i = 0; i < cols; ++i) {
        if (i > 0) {
            header_line += "  ";
            rule_line += "  ";
        }
        header_line += padCell(t_headers[i], widths[i], is_right(i));
        rule_line += std::string(widths[i], '-');
    }
    fmt::print(styleHeader(), "{}\n", header_line);
    fmt::print("{}\n", rule_line);
    for (const auto& row : t_rows) {
        std::string line;
        for (size_t i = 0; i < cols; ++i) {
            if (i > 0)
                line += "  ";
            line += padCell(i < row.size() ? row[i] : "", widths[i], is_right(i));
        }
        fmt::print("{}\n", line);
    }
}

// Re-indents a JSON document for humans; prints raw text when unparsable.
inline void printJsonPretty(const std::string& t_raw) {
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value value;
    std::string errors;
    if (reader->parse(t_raw.data(), t_raw.data() + t_raw.size(), &value, &errors)) {
        std::string pretty = Json::StyledWriter().write(value);
        if (!pretty.empty() && pretty.back() != '\n')
            pretty += '\n';
        fmt::print("{}", pretty);
    } else {
        fmt::print("{}\n", t_raw);
    }
}

// One-time credentials: aligned keys plus an unmissable reminder.
inline void printCredentialsOnce(const std::string& t_token, const std::string& t_secret) {
    fmt::print("token:        {}\n", t_token);
    fmt::print("token_secret: {}\n", t_secret);
    fmt::print(styleWarn(), "(shown once — store them now)\n");
}

// Expands a leading `~` to $HOME for local filesystem paths (readline
// does not do it for us). Remote datastore paths are never passed here.
inline std::string expandLocalPath(const std::string& t_path) {
#ifndef _WIN32
    if (t_path == "~" || t_path.rfind("~/", 0) == 0) {
        if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
            return std::string(home) + t_path.substr(1);
        }
    }
#endif
    return t_path;
}
} // namespace sgrn::datastore::shell
