#pragma once

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sgrn/Result.hpp>

namespace sgrn::utils::strings
{

#ifdef _WIN32
inline char* strndup(const char* t_str, size_t t_size) {
    size_t len = strnlen(t_str, t_size);
    char* new_str = (char*)malloc(len + 1);
    if (new_str == nullptr) {
        return nullptr;
    }
    memcpy(new_str, t_str, len);
    new_str[len] = '\0';
    return new_str;
}
#endif

constexpr const char str_utf8_bom[] = "\xEF\xBB\xBF";

/**
 * @brief Trim whitespace from the beginning and end of a string.
 */
inline std::string trim(std::string t_str) {
    auto not_space = [](unsigned char t_ch) { return !std::isspace(t_ch); };
    t_str.erase(t_str.begin(), std::find_if(t_str.begin(), t_str.end(), not_space));
    t_str.erase(std::find_if(t_str.rbegin(), t_str.rend(), not_space).base(), t_str.end());
    return t_str;
}

inline std::string& trimInPlace(std::string& t_str) {
    auto not_space = [](unsigned char t_ch) { return !std::isspace(t_ch); };

    t_str.erase(t_str.begin(), std::find_if(t_str.begin(), t_str.end(), not_space));

    t_str.erase(std::find_if(t_str.rbegin(), t_str.rend(), not_space).base(), t_str.end());

    return t_str;
}

/**
 * @brief Convert a string to uppercase.
 */
inline std::string toUpper(std::string t_str) {
    std::transform(t_str.begin(), t_str.end(), t_str.begin(), [](unsigned char t_ch) { return static_cast<char>(std::toupper(t_ch)); });
    return t_str;
}

/**
 * @brief Convert a string to lowercase.
 */
inline std::string toLower(std::string t_str) {
    std::transform(t_str.begin(), t_str.end(), t_str.begin(), [](unsigned char t_ch) { return static_cast<char>(std::tolower(t_ch)); });
    return t_str;
}

/**
 * @brief Normalize a name: trim, collapse multiple internal spaces, and convert to uppercase.
 *        Example: "  kahoul   ibrahim el-khalil  " -> "KAHOUL IBRAHIM EL-KHALIL"
 */
inline std::string normalizeName(std::string t_str) {
    std::string result;
    bool in_space = true; // start with true to trim leading
    for (unsigned char c : t_str) {
        if (std::isspace(c)) {
            if (!in_space) {
                result += ' ';
                in_space = true;
            }
        } else {
            result += static_cast<char>(std::toupper(c));
            in_space = false;
        }
    }
    // Remove trailing space if any
    if (!result.empty() && result.back() == ' ') {
        result.pop_back();
    }
    return result;
}

/**
 * @brief Join a list of strings with a separator.
 */
inline std::string join(const std::vector<std::string>& t_parts, std::string_view t_sep) {
    std::string result;
    for (size_t i = 0; i < t_parts.size(); ++i) {
        if (i > 0)
            result += t_sep;
        result += t_parts[i];
    }
    return result;
}

/**
 * @brief Split a string by a delimiter character.
 */
inline std::vector<std::string> split(std::string_view t_str, char t_delim) {
    std::vector<std::string> t_parts;
    size_t start = 0;
    size_t end = t_str.find(t_delim);
    while (end != std::string_view::npos) {
        t_parts.emplace_back(t_str.substr(start, end - start));
        start = end + 1;
        end = t_str.find(t_delim, start);
    }
    t_parts.emplace_back(t_str.substr(start));
    return t_parts;
}

/**
 * @brief Split a string by a delimiter and remove empty segments.
 */
inline std::vector<std::string> tokenize(std::string_view t_str, char t_delim) {
    std::vector<std::string> t_parts;
    size_t start = 0;
    size_t end = t_str.find(t_delim);
    while (end != std::string_view::npos) {
        if (end > start) {
            t_parts.emplace_back(t_str.substr(start, end - start));
        }
        start = end + 1;
        end = t_str.find(t_delim, start);
    }
    if (start < t_str.size()) {
        t_parts.emplace_back(t_str.substr(start));
    }
    return t_parts;
}

/**
 * @brief Parse an integer with a given base.
 */
inline std::optional<int> parseInt(const std::string& t_str, int t_base = 10) {
    try {
        size_t consumed = 0;
        int parsed = std::stoi(t_str, &consumed, t_base);
        if (consumed != t_str.size()) {
            return std::nullopt;
        }
        return parsed;
    } catch (...) {
        // safe to discard: parsing failure; returning nullopt captures semantic failure
        return std::nullopt;
    }
}

/**
 * @brief Parse an integer with flexible base detection (e.g., 0x prefix for hex).
 */
inline std::optional<int> parseIntFlexible(const std::string& t_str) {
    const std::string trimmed = trim(t_str);
    if (trimmed.size() > 2 && trimmed[0] == '0' && (trimmed[1] == 'x' || trimmed[1] == 'X')) {
        return parseInt(trimmed.substr(2), 16);
    }
    return parseInt(trimmed);
}

inline std::optional<uint64_t> parseUInt64(const std::string& t_str, int t_base = 10) {
    try {
        size_t consumed = 0;
        uint64_t parsed = std::stoull(t_str, &consumed, t_base);
        if (consumed != t_str.size()) {
            return std::nullopt;
        }
        return parsed;
    } catch (...) {
        // safe to discard: parsing failure; returning nullopt captures semantic failure
        return std::nullopt;
    }
}

inline std::optional<int64_t> parseInt64(const std::string& t_str, int t_base = 10) {
    try {
        size_t consumed = 0;
        int64_t parsed = std::stoll(t_str, &consumed, t_base);
        if (consumed != t_str.size()) {
            return std::nullopt;
        }
        return parsed;
    } catch (...) {
        // safe to discard: parsing failure; returning nullopt captures semantic failure
        return std::nullopt;
    }
}

inline std::optional<double> parseDouble(const std::string& t_str) {
    try {
        size_t consumed = 0;
        double parsed = std::stod(t_str, &consumed);
        if (consumed != t_str.size()) {
            return std::nullopt;
        }
        return parsed;
    } catch (...) {
        // safe to discard: parsing failure; returning nullopt captures semantic failure
        return std::nullopt;
    }
}

/**
 * @brief Split a command-line string into arguments, respecting quotes and backslashes.
 */
inline std::vector<std::string> splitArgs(std::string_view t_line) {
    std::vector<std::string> t_parts;
    std::string current;
    bool in_quotes = false;
    char quote_char = '\0';
    bool escaping = false;

    for (char t_ch : t_line) {
        if (escaping) {
            current.push_back(t_ch);
            escaping = false;
            continue;
        }
        if (t_ch == '\\') {
            escaping = true;
            continue;
        }
        if (in_quotes) {
            if (t_ch == quote_char) {
                in_quotes = false;
            } else {
                current.push_back(t_ch);
            }
            continue;
        }
        if (t_ch == '"' || t_ch == '\'') {
            in_quotes = true;
            quote_char = t_ch;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(t_ch))) {
            if (!current.empty()) {
                t_parts.push_back(current);
                current.clear();
            }
            continue;
        }
        current.push_back(t_ch);
    }
    if (!current.empty())
        t_parts.push_back(current);
    return t_parts;
}

inline std::string stripUtf8Bom(std::string t_s) {
    if (t_s.rfind(str_utf8_bom, 0) == 0)
        t_s.erase(0, std::string_view(str_utf8_bom).size());
    return t_s;
}

/// Strip inline comment (// ...) from a line
inline std::string stripComment(const std::string& t_line) {
    auto pos = t_line.find("//");
    return pos != std::string::npos ? t_line.substr(0, pos) : t_line;
}

inline std::vector<std::string> normalizeLines(const std::vector<std::string>& t_lines) {
    std::vector<std::string> normalized;
    normalized.reserve(t_lines.size());
    for (auto t_line : t_lines)
        normalized.push_back(stripUtf8Bom(std::move(t_line)));
    return normalized;
}

// Case conversion helpers

inline std::string toPascalCase(const std::string& t_s) {
    std::string result;
    bool next_upper = true;
    for (unsigned char c : t_s) {
        if (std::isalnum(c)) {
            if (next_upper) {
                result += static_cast<char>(std::toupper(c));
                next_upper = false;
            } else {
                result += static_cast<char>(c);
            }
        } else {
            next_upper = true;
        }
    }
    return result;
}

inline std::string toSnakeCase(const std::string& t_s) {
    std::string result;
    for (size_t i = 0; i < t_s.length(); ++i) {
        unsigned char c = t_s[i];
        if (std::isupper(c)) {
            if (i > 0 && result.back() != '_') {
                result += '_';
            }
            result += static_cast<char>(std::tolower(c));
        } else if (std::isalnum(c)) {
            result += static_cast<char>(c);
        } else {
            if (!result.empty() && result.back() != '_')
                result += '_';
        }
    }
    // Trim trailing underscore
    if (!result.empty() && result.back() == '_')
        result.pop_back();
    return result;
}

inline std::string toCamelCase(const std::string& t_s) {
    std::string pascal_case = toPascalCase(t_s);
    if (!pascal_case.empty()) {
        pascal_case[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(pascal_case[0])));
    }
    return pascal_case;
}

inline std::optional<std::string> utf16ToUtf8(const std::u16string& t_input) {
    std::string utf8;
    for (char16_t c16 : t_input) {
        if (c16 < 0x80) {
            utf8.push_back(static_cast<char>(c16));
        } else if (c16 < 0x800) {
            utf8.push_back(static_cast<char>(0xc0 | (c16 >> 6)));
            utf8.push_back(static_cast<char>(0x80 | (c16 & 0x3f)));
        } else {
            utf8.push_back(static_cast<char>(0xe0 | (c16 >> 12)));
            utf8.push_back(static_cast<char>(0x80 | ((c16 >> 6) & 0x3f)));
            utf8.push_back(static_cast<char>(0x80 | (c16 & 0x3f)));
        }
    }
    return utf8;
}

inline std::optional<std::u16string> utf8ToUtf16(const std::string& t_input) {
    std::u16string utf16;
    for (size_t i = 0; i < t_input.length(); ++i) {
        uint32_t cp = 0;
        uint8_t c = static_cast<uint8_t>(t_input[i]);
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xe0) == 0xc0) {
            if (i + 1 >= t_input.length())
                return std::nullopt;
            uint8_t b1 = static_cast<uint8_t>(t_input[++i]);
            cp = ((c & 0x1f) << 6) | (b1 & 0x3f);
        } else if ((c & 0xf0) == 0xe0) {
            if (i + 2 >= t_input.length())
                return std::nullopt;
            uint8_t b1 = static_cast<uint8_t>(t_input[++i]);
            uint8_t b2 = static_cast<uint8_t>(t_input[++i]);
            cp = ((c & 0x0f) << 12) | ((b1 & 0x3f) << 6) | (b2 & 0x3f);
        } else {
            return std::nullopt;
        }
        utf16.push_back(static_cast<char16_t>(cp));
    }
    return utf16;
}

inline std::optional<std::vector<uint8_t>> hexToBytes(const std::string_view t_hex) {
    std::string clean_hex;
    clean_hex.reserve(t_hex.size());
    for (char c : t_hex)
        if (std::isxdigit(static_cast<unsigned char>(c)))
            clean_hex.push_back(c);

    if (clean_hex.size() % 2 != 0)
        return std::nullopt;

    std::vector<uint8_t> bytes;
    bytes.reserve(clean_hex.size() / 2);
    for (size_t i = 0; i < clean_hex.size(); i += 2) {
        std::optional<int> v = parseInt(clean_hex.substr(i, 2), 16);
        if (!v.has_value())
            return std::nullopt;
        bytes.push_back(static_cast<uint8_t>(v.value()));
    }
    return bytes;
}

inline std::optional<std::string> sanitizeRelativeFilename(std::string t_path) {
    if (t_path.find('\0') != std::string::npos)
        return std::nullopt;
    if (t_path.find("..") != std::string::npos)
        return std::nullopt;
    while (!t_path.empty() && t_path.front() == '/')
        t_path = t_path.substr(1);
    return t_path;
}

/// Replace all occurrences of t_from with t_to in t_str (in-place).
///
/// Use case: The gateway HTTP adapter uses this to inject runtime HTML
/// (e.g. `<base href="/gateway/">` and `window.__SGRN_BASE__`) into the
/// cached `index.html` when serving behind a reverse proxy. Also used
/// by the bootstrap template engine to substitute `${POSTGRES_DB}`,
/// `${JWT_SECRET}`, etc. in generated config files.
///
/// Example:
///   sgrn::utils::strings::replaceAll(html, "<!-- HEAD -->", injected_script);
///
inline void replaceAll(std::string& t_str, std::string_view t_from, std::string_view t_to) {
    if (t_from.empty())
        return;
    std::size_t pos = 0;
    while ((pos = t_str.find(t_from, pos)) != std::string::npos) {
        t_str.replace(pos, t_from.length(), t_to);
        pos += t_to.length();
    }
}

/// Validate a bare filename (no path separators, no dot-dot, no null bytes).
///
/// Use case: The storage API handler calls this when a user requests a
/// file/folder rename (move) to prevent path-traversal injection via the
/// `new_name` field. It ensures the name is a single path component with
/// no `/`, `\`, `..`, or null bytes — rejecting any attempt to escape the
/// target directory.
///
/// Example:
///   auto result = sgrn::utils::strings::isValidFileName(new_name);
///   if (result.hasError())
///       return result.error();
///
/// @return Result<void, std::string> — success if the name is valid, or an
///         error string describing the validation failure if invalid.
inline sgrn::Result<void, std::string> isValidFileName(const std::string& t_name) {
    if (t_name.empty()) {
        return "Filename must not be empty";
    }
    if (t_name == "." || t_name == "..") {
        return "Filename must not be '.' or '..'";
    }
    if (t_name.find('/') != std::string::npos) {
        return "Filename must not contain path separator '/'";
    }
    if (t_name.find('\\') != std::string::npos) {
        return "Filename must not contain path separator '\\'";
    }
    if (t_name.find('\0') != std::string::npos) {
        return "Filename must not contain null bytes";
    }
    if (t_name.find("..") != std::string::npos) {
        return "Filename must not contain '..'";
    }
    return {};
}

inline bool fieldPathMatches(std::string_view t_pattern, std::string_view t_path) {
    size_t p_idx = 0;
    size_t t_idx = 0;
    while (p_idx < t_pattern.size() && t_idx < t_path.size()) {
        if (t_pattern.compare(p_idx, 3, "[*]") == 0 && t_path[t_idx] == '[') {
            p_idx += 3;
            while (t_idx < t_path.size() && t_path[t_idx] != ']')
                t_idx++;
            if (t_idx < t_path.size() && t_path[t_idx] == ']')
                t_idx++;
        } else if (t_pattern[p_idx] == '*') {
            p_idx++;
            while (t_idx < t_path.size() && t_path[t_idx] != '.' && t_path[t_idx] != '[')
                t_idx++;
        } else if (t_pattern[p_idx] == t_path[t_idx]) {
            p_idx++;
            t_idx++;
        } else {
            return false;
        }
    }
    while (p_idx < t_pattern.size()) {
        if (t_pattern.compare(p_idx, 3, "[*]") == 0)
            p_idx += 3;
        else if (t_pattern[p_idx] == '*')
            p_idx++;
        else
            break;
    }
    return p_idx == t_pattern.size() && t_idx == t_path.size();
}

} // namespace sgrn::utils::strings
