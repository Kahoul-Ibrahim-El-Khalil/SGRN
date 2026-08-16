#include <fmt/core.h>
#include <sgrn/scl/schema/DbSymbolsParser.hpp>
#include <sgrn/scl/schema/SchemaSerializer.hpp>
#include <sgrn/utils/strings.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <optional>
#include <regex>
#include <string>
#include <utility>

namespace sgrn::scl
{

namespace str = sgrn::utils::strings;

using sgrn::Result;
using sgrn::scl::Error;
namespace
{

// -----------------------------------------------------------------------
// Offset tracker — S7 alignment rules
// -----------------------------------------------------------------------

struct OffsetTracker {
    int current_byte_{0};
    int current_bit_{0};

    void alignTo(int t_alignment) {
        if (current_bit_ > 0) {
            current_byte_++;
            current_bit_ = 0;
        }
        if (t_alignment > 1 && current_byte_ % t_alignment != 0)
            current_byte_ += t_alignment - (current_byte_ % t_alignment);
    }

    void advance(DbField& t_field) {
        // Boolean handling (bit-packing)
        if (t_field.type == DataType::Bool && t_field.count <= 1) {
            if (current_bit_ >= 8) {
                current_byte_++;
                current_bit_ = 0;
            }
            t_field.offset = current_byte_;
            t_field.bit_index = current_bit_;
            current_bit_++;
            return;
        }
        if (t_field.type == DataType::Bool && t_field.count > 1) {
            if (current_bit_ > 0) {
                current_byte_++;
                current_bit_ = 0;
            }
            alignTo(2);
            t_field.offset = current_byte_;
            t_field.bit_index = 0;
            int byte_count = (t_field.count + 7) / 8;
            if (byte_count % 2 != 0)
                byte_count++;
            current_byte_ += byte_count;
            return;
        }

        // Non-Bool fields
        int element_size = s7codec::primitiveSize(t_field.type);
        if (t_field.type == DataType::Struct)
            element_size = t_field.struct_size;

        int total_size = 0;
        int num_elements = std::max(1, t_field.count);

        // ── String / WString / XString / XWString ──────────────────────────────
        if (t_field.type == DataType::String || t_field.type == DataType::WString || t_field.type == DataType::XString ||
            t_field.type == DataType::XWString) {
            // For strings: struct_size holds the per‑element capacity (if > 0),
            // otherwise count holds the scalar capacity (legacy).
            // We normalise by setting struct_size to the capacity and count to 1
            // for scalar strings.
            int capacity = t_field.struct_size > 0 ? t_field.struct_size : t_field.count;
            if (t_field.struct_size == 0) {
                // This is a scalar string – treat it as one element of capacity 'capacity'
                t_field.struct_size = capacity;
                t_field.count = 1;
                num_elements = 1;
            }
            // Now compute total size: capacity bytes per string * number of strings
            int elem_span = s7codec::typeSpanBytes(t_field.type, capacity);
            total_size = elem_span * num_elements;

            // Alignment: strings are aligned to 2-byte boundary (like S7)
            if (s7codec::primitiveSize(t_field.type) >= 2 || t_field.type == DataType::XString || t_field.type == DataType::XWString)
                alignTo(2);
        } else {
            // Primitive types (non-string, non-struct)
            total_size = element_size * num_elements;
        }

        // Advance to next byte boundary if currently inside a bit
        if (current_bit_ > 0) {
            current_byte_++;
            current_bit_ = 0;
        }

        // Dynamic fields get a 4-byte header
        if (t_field.is_dynamic) {
            alignTo(4);
            t_field.offset = current_byte_;
            current_byte_ += 4; // header space
        }

        // Align to element size or 2-byte boundary for structs/arrays
        if (element_size >= 2 || t_field.type == DataType::Struct || t_field.count > 1 || t_field.type == DataType::XString ||
            t_field.type == DataType::XWString) {
            alignTo(2);
        }

        // Set final offset and zero bit index (bit-index is only for BOOL)
        if (!t_field.is_dynamic)
            t_field.offset = current_byte_;
        t_field.bit_index = 0;

        // Reserve space
        current_byte_ += total_size;
    }
    int getTotalSize() const {
        int size = current_byte_;
        if (current_bit_ > 0)
            size++;
        if (size % 2 != 0)
            size++;
        return size;
    }
};

int calculateMaxDepth(const std::vector<DbField>& t_fields) {
    int max = 0;
    for (const auto& f : t_fields) {
        if (f.type == DataType::Struct) {
            max = std::max(max, 1 + calculateMaxDepth(f.children));
        } else {
            max = std::max(max, 1);
        }
    }
    return max;
}

// -----------------------------------------------------------------------
// AST Lexer / Parser
// -----------------------------------------------------------------------

namespace ast
{

enum class TokenType { Identifier, StringLiteral, Number, Keyword, Punctuation, EndOfFile, Error };

struct Token {
    TokenType type;
    std::string value;
    int line;
    int col;
};

class Lexer {
    std::string source_;
    size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;

    char peek() const {
        return pos_ < source_.length() ? source_[pos_] : '\0';
    }
    char advance() {
        if (pos_ >= source_.length())
            return '\0';
        char c = source_[pos_++];
        if (c == '\n') {
            line_++;
            col_ = 1;
        } else {
            col_++;
        }
        return c;
    }
    void skipWhitespace() {
        while (true) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                advance();
            } else if (c == '/' && pos_ + 1 < source_.length() && source_[pos_ + 1] == '/') {
                while (peek() != '\n' && peek() != '\0')
                    advance();
            } else if (c == '(' && pos_ + 1 < source_.length() && source_[pos_ + 1] == '*') {
                advance();
                advance();
                while (peek() != '\0') {
                    if (peek() == '*' && pos_ + 1 < source_.length() && source_[pos_ + 1] == ')') {
                        advance();
                        advance();
                        break;
                    }
                    advance();
                }
            } else
                break;
        }
    }

public:
    Lexer(std::string t_src)
        : source_(std::move(t_src)) {
    }

    Token nextToken() {
        skipWhitespace();
        Token t;
        t.line = line_;
        t.col = col_;
        char c = peek();

        if (c == '\0') {
            t.type = TokenType::EndOfFile;
            return t;
        }

        if (std::isalpha(c) || c == '_' || c == '#') {
            while (std::isalnum(peek()) || peek() == '_' || peek() == '#')
                t.value += advance();
            // Create an uppercase copy for keyword lookup to preserve original case for identifiers.
            // The parser always stores and compares kEywords in upper case.
            std::string upper_val = t.value;
            for (char& ch : upper_val)
                ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));

            static constexpr std::array kEywords = {
                std::string_view{"TYPE"},
                std::string_view{"END_TYPE"},
                std::string_view{"DATA_BLOCK"},
                std::string_view{"END_DATA_BLOCK"},
                std::string_view{"STRUCT"},
                std::string_view{"END_STRUCT"},
                std::string_view{"VAR"},
                std::string_view{"END_VAR"},
                std::string_view{"VAR_INPUT"},
                std::string_view{"VAR_OUTPUT"},
                std::string_view{"VAR_IN_OUT"},
                std::string_view{"VAR_TEMP"},
                std::string_view{"ARRAY"},
                std::string_view{"OF"},
                std::string_view{"BEGIN"},
                std::string_view{"VERSION"},
                std::string_view{"NON_RETAIN"},
                std::string_view{"TITLE"},
                std::string_view{"AUTHOR"},
                std::string_view{"FAMILY"},
                std::string_view{"NAME"},
                std::string_view{"KNOW_HOW_PROTECT"},
                std::string_view{"EXTERNAL"},
                std::string_view{"READ_ONLY"},
                std::string_view{"DB"},
                std::string_view{"#BIG_ENDIAN"},
                std::string_view{"#LITTLE_ENDIAN"},
                std::string_view{"#UNIT"},
                std::string_view{"#RANGE"},
                std::string_view{"#ENUM"},
                std::string_view{"#EVENT_TRIGGER"},
                std::string_view{"XSTRING"},
                std::string_view{"XWSTRING"},
                std::string_view{"#DYNAMIC"},
                std::string_view{"#MODBUS_HOLDING"},
                std::string_view{"#MODBUS_INPUT"},
                std::string_view{"#MODBUS_COIL"},
                std::string_view{"#MODBUS_DISCRETE"},
            };

            if (std::find(kEywords.begin(), kEywords.end(), std::string_view{upper_val}) != kEywords.end()) {
                t.type = TokenType::Keyword;
                t.value = std::move(upper_val);
            } else {
                t.type = TokenType::Identifier;
            }
            return t;
        }

        if (std::isdigit(c)) {
            while (std::isdigit(peek()) || peek() == '.') {
                if (peek() == '.' && pos_ + 1 < source_.length() && source_[pos_ + 1] == '.')
                    break; // stop at ..
                t.value += advance();
            }
            t.type = TokenType::Number;
            return t;
        }

        if (c == '"' || c == '\'') {
            char quote = advance();
            while (peek() != quote && peek() != '\0')
                t.value += advance();
            if (peek() == quote)
                advance();
            t.type = TokenType::StringLiteral;
            return t;
        }

        if (c == ':' || c == ';' || c == '[' || c == ']' || c == '{' || c == '}' || c == '.' || c == '=' || c == ',' || c == '(' ||
            c == ')' || c == '-') {
            t.value += advance();
            if (t.value == ":" && peek() == '=')
                t.value += advance();
            else if (t.value == "." && peek() == '.')
                t.value += advance();
            t.type = TokenType::Punctuation;
            return t;
        }
        t.type = TokenType::Error;
        t.value += advance();
        return t;
    }
};

class AstParser {
    Lexer lexer_;
    Token current_;
    Token previous_;
    ParseResult result_;
    std::map<std::string, UdtDefinition> udt_map_;
    uint16_t m_last_db_number_ = 0;

    bool m_has_error_ = false;
    std::string m_error_msg_;

    void setError(const std::string& t_msg) {
        if (!m_has_error_) {
            m_has_error_ = true;
            m_error_msg_ = t_msg;
        }
    }

    void advance() {
        if (m_has_error_)
            return;
        previous_ = current_;
        current_ = lexer_.nextToken();
    }
    bool check(TokenType t_type) const {
        if (m_has_error_)
            return false;
        return current_.type == t_type;
    }
    bool checkKeyword(const std::string& t_w) const {
        if (m_has_error_)
            return false;
        return current_.type == TokenType::Keyword && current_.value == t_w;
    }
    bool checkPunctuation(const std::string& t_p) const {
        if (m_has_error_)
            return false;
        return current_.type == TokenType::Punctuation && current_.value == t_p;
    }

    bool match(TokenType t_type) {
        if (check(t_type)) {
            advance();
            return true;
        }
        return false;
    }
    bool matchKeyword(const std::string& t_w) {
        if (checkKeyword(t_w)) {
            advance();
            return true;
        }
        return false;
    }
    bool matchPunctuation(const std::string& t_p) {
        if (checkPunctuation(t_p)) {
            advance();
            return true;
        }
        return false;
    }

    void expect(TokenType t_type, const std::string& t_msg) {
        if (!match(t_type))
            setError(fmt::format("Line {}:{} - {} (got: '{}')", current_.line, current_.col, t_msg, current_.value));
    }
    void expectKeyword(const std::string& t_w, const std::string& t_msg) {
        if (!matchKeyword(t_w))
            setError(fmt::format("Line {}:{} - {} (got: '{}')", current_.line, current_.col, t_msg, current_.value));
    }
    void expectPunctuation(const std::string& t_p, const std::string& t_msg) {
        if (!matchPunctuation(t_p))
            setError(fmt::format("Line {}:{} - {} (got: '{}')", current_.line, current_.col, t_msg, current_.value));
    }

    void parseAttributes(s7codec::Endian& t_out_endian, bool& t_trigger_events, sgrn::scl::ModbusArea& t_out_modbus_area) {
        while (!m_has_error_) {
            if (checkKeyword("TITLE") || checkKeyword("AUTHOR") || checkKeyword("FAMILY") || checkKeyword("NAME") ||
                checkKeyword("VERSION") || checkKeyword("NON_RETAIN") || checkKeyword("KNOW_HOW_PROTECT") || checkKeyword("EXTERNAL") ||
                checkKeyword("READ_ONLY")) {
                advance();
                if (matchPunctuation("="))
                    advance();
                else if (matchPunctuation(":"))
                    advance();
                matchPunctuation(";");
            } else if (matchKeyword("#BIG_ENDIAN")) {
                t_out_endian = s7codec::Endian::Big;
            } else if (matchKeyword("#LITTLE_ENDIAN")) {
                t_out_endian = s7codec::Endian::Little;
            } else if (matchKeyword("#EVENT_TRIGGER")) {
                t_trigger_events = true;
            } else if (matchKeyword("#MODBUS_HOLDING")) {
                t_out_modbus_area = sgrn::scl::ModbusArea::Holding;
            } else if (matchKeyword("#MODBUS_INPUT")) {
                t_out_modbus_area = sgrn::scl::ModbusArea::Input;
            } else if (matchKeyword("#MODBUS_COIL")) {
                t_out_modbus_area = sgrn::scl::ModbusArea::Coil;
            } else if (matchKeyword("#MODBUS_DISCRETE")) {
                t_out_modbus_area = sgrn::scl::ModbusArea::Discrete;
            } else if (matchPunctuation("{")) {
                while (!checkPunctuation("}") && !check(TokenType::EndOfFile) && !m_has_error_) {
                    advance();
                }
                matchPunctuation("}");
            } else {
                break;
            }
        }
    }

public:
    AstParser(std::string t_src, const std::map<std::string, UdtDefinition>& tp_global_udts)
        : lexer_(std::move(t_src)) {
        udt_map_ = tp_global_udts;
        advance();
    }

    Result<ParseResult, Error> parse() {
        while (!check(TokenType::EndOfFile) && !m_has_error_) {
            if (matchKeyword("TYPE"))
                parseUdt();
            else if (matchKeyword("DATA_BLOCK"))
                parseDb();
            else
                advance();
        }

        if (m_has_error_) {
            return Result<ParseResult, Error>::Error(Error{SchemaCode::ParseError, m_error_msg_});
        }

        bool changed = true;
        for (int pass = 0; pass < 10 && changed; ++pass) {
            changed = false;
            for (auto& udt : result_.udts) {
                if (resolveFields(udt.fields))
                    changed = true;
            }
            for (auto& db : result_.dbs) {
                if (resolveFields(db.fields))
                    changed = true;
            }
        }

        return result_;
    }

private:
    bool resolveFields(std::vector<DbField>& t_fields) {
        bool changed = false;
        for (auto& f : t_fields) {
            if (f.type == s7codec::Type::Struct && f.children.empty() && !f.udt_name.empty()) {
                if (udt_map_.count(f.udt_name)) {
                    f.children = udt_map_[f.udt_name].fields;
                    f.struct_size = udt_map_[f.udt_name].size_bytes;
                    changed = true;
                }
            }
            if (!f.children.empty()) {
                if (resolveFields(f.children))
                    changed = true;
            }
        }
        return changed;
    }

    void parseUdt() {
        std::string t_name;
        if (match(TokenType::StringLiteral) || match(TokenType::Identifier)) {
            t_name = previous_.value;
        } else {
            setError(fmt::format("Line {}:{} - Expected UDT name", current_.line, current_.col));
            return;
        }

        UdtDefinition udt;
        udt.name = t_name;
        udt.udt_number = extractNumber(t_name, "UDT");

        sgrn::scl::ModbusArea unused_modbus{sgrn::scl::ModbusArea::None};
        parseAttributes(udt.endianness, udt.trigger_events, unused_modbus);
        if (m_has_error_)
            return;

        OffsetTracker t_tracker;
        if (matchKeyword("STRUCT")) {
            udt.fields = parseStructFields(t_tracker, "END_STRUCT", udt.endianness);
            udt.size_bytes = t_tracker.getTotalSize();
        } else {
            while (!checkKeyword("END_TYPE") && !check(TokenType::EndOfFile) && !m_has_error_) {
                if (checkKeyword("DATA_BLOCK") || checkKeyword("TYPE")) {
                    setError(fmt::format("Line {}:{} - Missing END_TYPE for UDT '{}'", current_.line, current_.col, t_name));
                    return;
                }
                if (checkKeyword("VAR") || checkKeyword("VAR_INPUT") || checkKeyword("VAR_OUTPUT") || checkKeyword("VAR_IN_OUT") ||
                    checkKeyword("VAR_TEMP")) {
                    advance();
                    auto t_fields = parseStructFields(t_tracker, "END_VAR", udt.endianness);
                    udt.fields.insert(udt.fields.end(), t_fields.begin(), t_fields.end());
                } else {
                    advance();
                }
            }
            udt.size_bytes = t_tracker.getTotalSize();
        }

        while (!checkKeyword("END_TYPE") && !check(TokenType::EndOfFile) && !m_has_error_) {
            if (checkKeyword("DATA_BLOCK") || checkKeyword("TYPE")) {
                setError(fmt::format("Line {}:{} - Missing END_TYPE for UDT '{}'", current_.line, current_.col, t_name));
                return;
            }
            advance();
        }

        if (!matchKeyword("END_TYPE")) {
            setError(fmt::format("Line {}:{} - Missing END_TYPE for UDT '{}'", current_.line, current_.col, t_name));
            return;
        }

        udt.max_depth = calculateMaxDepth(udt.fields);
        result_.udts.push_back(udt);
        udt_map_[t_name] = udt;
    }

    void parseDb() {
        std::string t_name;
        int number = 0;

        if (match(TokenType::StringLiteral) || match(TokenType::Identifier)) {
            t_name = previous_.value;
            number = extractNumber(t_name, "DB");

            if (number == 0 && matchKeyword("DB")) {
                if (match(TokenType::Number)) {
                    number = std::stoi(previous_.value);
                }
            }
        } else if (matchKeyword("DB")) {
            if (match(TokenType::Number)) {
                number = std::stoi(previous_.value);
                t_name = "DB" + previous_.value;
            } else {
                t_name = "DB";
            }
        } else {
            setError(fmt::format("Line {}:{} - Expected DB name or 'DB <num>'", current_.line, current_.col));
            return;
        }

        if (number == 0) {
            number = ++m_last_db_number_;
        } else {
            m_last_db_number_ = number;
        }

        DataBlockRegistry db;
        db.db_name = t_name;
        db.db_number = number;

        parseAttributes(db.endianness, db.trigger_events, db.modbus_area);
        if (m_has_error_)
            return;

        OffsetTracker t_tracker;
        while (!checkKeyword("BEGIN") && !checkKeyword("END_DATA_BLOCK") && !check(TokenType::EndOfFile) && !m_has_error_) {
            if (checkKeyword("DATA_BLOCK") || checkKeyword("TYPE")) {
                setError(fmt::format("Line {}:{} - Missing BEGIN/END_DATA_BLOCK for DB '{}'", current_.line, current_.col, t_name));
                return;
            }

            if (checkKeyword("VAR") || checkKeyword("VAR_INPUT") || checkKeyword("VAR_OUTPUT") || checkKeyword("VAR_IN_OUT") ||
                checkKeyword("VAR_TEMP") || checkKeyword("STRUCT")) {
                std::string end_kw = checkKeyword("STRUCT") ? "END_STRUCT" : "END_VAR";
                advance();
                auto t_fields = parseStructFields(t_tracker, end_kw, db.endianness);
                db.fields.insert(db.fields.end(), t_fields.begin(), t_fields.end());
            } else {
                advance();
            }
        }
        db.size_bytes = t_tracker.getTotalSize();
        db.max_depth = calculateMaxDepth(db.fields);

        if (matchKeyword("BEGIN")) {
            while (!checkKeyword("END_DATA_BLOCK") && !check(TokenType::EndOfFile) && !m_has_error_) {
                if (checkKeyword("DATA_BLOCK") || checkKeyword("TYPE")) {
                    setError(fmt::format("Line {}:{} - Missing END_DATA_BLOCK for DB '{}'", current_.line, current_.col, t_name));
                    return;
                }
                advance();
            }
        }

        if (!matchKeyword("END_DATA_BLOCK")) {
            setError(fmt::format("Line {}:{} - Missing END_DATA_BLOCK for DB '{}'", current_.line, current_.col, t_name));
            return;
        }

        result_.dbs.push_back(db);
    }

    std::vector<DbField> parseStructFields(
        OffsetTracker& t_tracker, const std::string& t_end_keyword, s7codec::Endian t_block_endian, int t_depth = 0) {
        if (t_depth > 1000) {
            setError(fmt::format("Line {}:{} - Maximum recursion depth exceeded", current_.line, current_.col));
            return {};
        }
        std::vector<DbField> t_fields;
        while (!checkKeyword(t_end_keyword) && !check(TokenType::EndOfFile) && !m_has_error_) {
            if (checkKeyword("DATA_BLOCK") || checkKeyword("TYPE")) {
                setError(fmt::format("Line {}:{} - Missing '{}', found new block", current_.line, current_.col, t_end_keyword));
                break;
            }
            if (match(TokenType::Identifier) || match(TokenType::StringLiteral)) {
                std::string field_name = previous_.value;

                if (matchPunctuation("{")) {
                    while (!checkPunctuation("}") && !check(TokenType::EndOfFile))
                        advance();
                    matchPunctuation("}");
                }

                expectPunctuation(":", "Expected ':' after field name");
                DbField f = parseType(t_block_endian, t_depth);
                f.name = field_name;
                f.endianness = t_block_endian;

                if (matchPunctuation(":=")) {
                    while (!checkPunctuation(";") && !check(TokenType::EndOfFile))
                        advance();
                }

                while (checkKeyword("#UNIT") || checkKeyword("#RANGE") || checkKeyword("#ENUM") || checkKeyword("#EVENT_TRIGGER") ||
                       checkKeyword("#DYNAMIC") || checkKeyword("#BIG_ENDIAN") || checkKeyword("#LITTLE_ENDIAN")) {
                    if (matchKeyword("#UNIT")) {
                        bool has_paren = matchPunctuation("(");
                        if (match(TokenType::StringLiteral)) {
                            f.unit = previous_.value;
                            if (has_paren)
                                expectPunctuation(")", "Expected ')' after #UNIT string");
                        } else {
                            setError(fmt::format("Line {}:{} - Expected string literal after #UNIT", current_.line, current_.col));
                        }
                    } else if (matchKeyword("#RANGE")) {
                        expectPunctuation("(", "Expected '(' after #RANGE");
                        if (match(TokenType::Number)) {
                            f.min_val = std::stod(previous_.value);
                        } else if (matchPunctuation("-")) {
                            if (match(TokenType::Number))
                                f.min_val = -std::stod(previous_.value);
                        }
                        expectPunctuation(",", "Expected ',' in #RANGE");
                        if (match(TokenType::Number)) {
                            f.max_val = std::stod(previous_.value);
                        } else if (matchPunctuation("-")) {
                            if (match(TokenType::Number))
                                f.max_val = -std::stod(previous_.value);
                        }
                        expectPunctuation(")", "Expected ')' after #RANGE");
                    } else if (matchKeyword("#ENUM")) {
                        expectPunctuation("(", "Expected '(' after #ENUM");
                        int current_enum_idx = 0;
                        while (!checkPunctuation(")") && !check(TokenType::EndOfFile) && !m_has_error_) {
                            std::string enum_name;
                            if (match(TokenType::Identifier) || match(TokenType::StringLiteral)) {
                                enum_name = previous_.value;
                            } else {
                                setError(fmt::format("Line {}:{} - Expected identifier in #ENUM", current_.line, current_.col));
                                break;
                            }

                            if (matchPunctuation("=")) {
                                bool val_neg = matchPunctuation("-");
                                if (match(TokenType::Number)) {
                                    current_enum_idx = std::stoi(previous_.value) * (val_neg ? -1 : 1);
                                } else {
                                    setError(fmt::format("Line {}:{} - Expected number after '=' in #ENUM", current_.line, current_.col));
                                    break;
                                }
                            }

                            f.enum_map[current_enum_idx] = enum_name;
                            current_enum_idx++;

                            if (matchPunctuation(",")) {
                                continue;
                            } else {
                                break;
                            }
                        }
                        expectPunctuation(")", "Expected ')' after #ENUM");
                    } else if (matchKeyword("#EVENT_TRIGGER")) {
                        f.trigger_events = true;
                    } else if (matchKeyword("#DYNAMIC")) {
                        f.is_dynamic = true;
                    } else if (matchKeyword("#BIG_ENDIAN")) {
                        f.endianness = s7codec::Endian::Big;
                    } else if (matchKeyword("#LITTLE_ENDIAN")) {
                        f.endianness = s7codec::Endian::Little;
                    }
                }

                expectPunctuation(";", "Expected ';' after field definition");

                t_tracker.advance(f);
                t_fields.push_back(f);
            } else {
                advance();
            }
        }
        matchKeyword(t_end_keyword);
        return t_fields;
    }

    DbField parseType(s7codec::Endian t_block_endian, int t_depth = 0) {
        DbField f;
        if (matchKeyword("ARRAY")) {
            expectPunctuation("[", "Expected '[' in array definition");
            int lo = 0, hi = 0;

            bool lo_neg = matchPunctuation("-");
            if (match(TokenType::Number))
                lo = std::stoi(previous_.value) * (lo_neg ? -1 : 1);
            else if (lo_neg)
                setError(fmt::format("Line {}:{} - Expected number after '-' in array bounds", current_.line, current_.col));

            expectPunctuation("..", "Expected '..' in array bounds");

            bool hi_neg = matchPunctuation("-");
            if (match(TokenType::Number))
                hi = std::stoi(previous_.value) * (hi_neg ? -1 : 1);
            else if (hi_neg)
                setError(fmt::format("Line {}:{} - Expected number after '-' in array bounds", current_.line, current_.col));

            expectPunctuation("]", "Expected ']' in array definition");
            expectKeyword("OF", "Expected OF in array definition");
            f = parsePrimitiveOrUdt();
            int array_count = hi - lo + 1;
            if (array_count <= 0) {
                setError(fmt::format("Line {}:{} - Invalid Array Bounds: lower bound ({}) is greater than upper bound ({}).", current_.line,
                    current_.col, lo, hi));
                return f;
            }

            if (f.type == DataType::String || f.type == DataType::WString || f.type == DataType::XString || f.type == DataType::XWString) {
                f.struct_size = f.count;
            }
            f.count = array_count;
            f.endianness = t_block_endian;

        } else if (matchKeyword("STRUCT")) {
            f.type = s7codec::Type::Struct;
            f.endianness = t_block_endian;
            OffsetTracker sub_tracker;
            f.children = parseStructFields(sub_tracker, "END_STRUCT", t_block_endian, t_depth + 1);
            f.struct_size = sub_tracker.getTotalSize();
            f.count = 1;
        } else {
            f = parsePrimitiveOrUdt();
            if (f.count <= 0)
                f.count = 1;
            f.endianness = t_block_endian;
        }
        return f;
    }

    DbField parsePrimitiveOrUdt() {
        DbField f;
        if (match(TokenType::Identifier) || match(TokenType::StringLiteral) || match(TokenType::Keyword)) {
            std::string type_name = previous_.value;
            sgrn::utils::strings::trim(type_name);
            std::string upper = sgrn::utils::strings::toUpper(type_name);

            s7codec::Type t;
            if (s7codec::stringToType(upper.c_str(), t)) {
                f.type = t;

                if (t == s7codec::Type::String || t == s7codec::Type::WString || t == s7codec::Type::XString ||
                    t == s7codec::Type::XWString) {
                    // Default capacity
                    int capacity = (t == s7codec::Type::XString || t == s7codec::Type::XWString) ? 1000 : 254;

                    // Parse optional [length] or (length)
                    if (matchPunctuation("[") || matchPunctuation("(")) {
                        std::string closing = previous_.value == "[" ? "]" : ")";
                        if (match(TokenType::Number))
                            capacity = std::stoi(previous_.value);
                        matchPunctuation(closing); // consume ']' or ')'
                    }

                    // Store capacity in struct_size, mark as scalar (count = 1)
                    // (If later wrapped in an ARRAY, parseType() will set count to array size)
                    f.struct_size = capacity;
                    f.count = 1;
                }
            } else {
                // UDT reference
                f.type = s7codec::Type::Struct;
                f.udt_name = type_name;
                if (udt_map_.count(type_name)) {
                    f.children = udt_map_[type_name].fields;
                    f.struct_size = udt_map_[type_name].size_bytes;
                }
            }
        } else {
            setError(fmt::format("Line {}:{} - Expected type identifier (got: '{}')", current_.line, current_.col, current_.value));
        }
        return f;
    }
    int extractNumber(const std::string& t_name, const std::string& t_prefix) {
        std::string upper = sgrn::utils::strings::toUpper(t_name);
        if (upper.find(t_prefix + " ") == 0) {
            try {
                return std::stoi(upper.substr(t_prefix.length() + 1));
            } catch (...) {
            }
        } else if (upper.find(t_prefix) == 0) {
            try {
                return std::stoi(upper.substr(t_prefix.length()));
            } catch (...) {
            }
        }
        return 0;
    }
};

} // namespace ast

} // anonymous namespace

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

Result<ParseResult, Error> DbSymbolsParser::parseExportFile(
    const std::string& t_filepath, std::map<std::string, UdtDefinition>* tp_global_udts) {
    std::ifstream file(t_filepath);
    if (!file.is_open())
        return Err::FileNotFound("could not open export file: {}", t_filepath);

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line))
        lines.push_back(line);

    return parseLines(lines, tp_global_udts);
}

Result<ParseResult, Error> DbSymbolsParser::parseLines(
    const std::vector<std::string>& t_lines, std::map<std::string, UdtDefinition>* tp_global_udts) {
    if (t_lines.empty()) {
        return Result<ParseResult, Error>::Error(Error{SchemaCode::ParseError, "symbol lines are empty"});
    }

    std::string source;
    for (const auto& l : t_lines)
        source += l + '\n';

    return parseString(source, tp_global_udts);
}

Result<ParseResult, Error> DbSymbolsParser::parseString(
    const std::string& t_content, std::map<std::string, UdtDefinition>* tp_global_udts) {
    if (t_content.empty()) {
        return Result<ParseResult, Error>::Error(Error{SchemaCode::ParseError, "symbol content is empty"});
    }

    std::map<std::string, UdtDefinition> udt_map;
    if (tp_global_udts)
        udt_map = *tp_global_udts;

    ast::AstParser parser(t_content, udt_map);
    Result<ParseResult, Error> result = parser.parse();

    if (result.hasError()) {
        return result;
    }

    if (tp_global_udts) {
        for (const auto& udt : result.value().udts)
            (*tp_global_udts)[udt.name] = udt;
    }

    if (result.value().dbs.empty() && result.value().udts.empty()) {
        return Result<ParseResult, Error>::Error(Error{SchemaCode::ParseError, "no DB schemas or UDTs found in symbols (check format)"});
    }

    return result;
}

Result<ParseResult, Error> DbSymbolsParser::parseCollection(const std::vector<std::string>& t_filepaths) {
    if (t_filepaths.empty())
        return ParseResult{};

    std::map<std::string, UdtDefinition> tp_global_udts;
    int prev_count = -1;

    while (static_cast<int>(tp_global_udts.size()) != prev_count) {
        prev_count = static_cast<int>(tp_global_udts.size());
        for (const auto& path : t_filepaths) {
            parseExportFile(path, &tp_global_udts);
        }
    }

    ParseResult consolidated;
    for (const auto& path : t_filepaths) {
        auto res = parseExportFile(path, &tp_global_udts);
        if (res.hasError()) {
            if (res.error().string().find("no DB schemas or UDTs found") != std::string::npos)
                continue;
            return Error(res.error());
        }

        auto& t_p = res.value();
        consolidated.dbs.insert(consolidated.dbs.end(), std::make_move_iterator(t_p.dbs.begin()), std::make_move_iterator(t_p.dbs.end()));
        consolidated.warnings.insert(consolidated.warnings.end(), t_p.warnings.begin(), t_p.warnings.end());
    }

    for (auto& [t_name, udt] : tp_global_udts) {
        consolidated.udts.push_back(std::move(udt));
    }

    return consolidated;
}

} // namespace sgrn::scl
