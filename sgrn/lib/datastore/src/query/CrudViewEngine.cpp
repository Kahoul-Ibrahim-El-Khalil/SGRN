#include <sgrn/datastore/query/CrudViewEngine.hpp>

#include <sgrn/datastore/core/db.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe_access.hpp>

#include <fmt/core.h>
#include <fmt/format.h>
#include <algorithm>
#include <charconv>
#include <iterator>
#include <json/json.h>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sgrn::datastore::query
{
using namespace drogon;

namespace
{

// --- whitelist lookups -----------------------------------------------------

const Field* findField(const CrudViewSpec& t_spec, std::string_view t_name) {
    auto it = std::find_if(t_spec.fields.begin(), t_spec.fields.end(), [&](const Field& f) { return f.name == t_name; });
    return it == t_spec.fields.end() ? nullptr : &(*it);
}

// Output-side column typing: the SELECT list is whitelist-built (pk +
// tenant + spec.fields), so every returned column is one of these three.
// Anything else defaults to Text rather than crashing (unreachable in
// practice, but never fail a read on it).
FieldType resolveColumnType(const CrudViewSpec& t_spec, std::string_view t_name) {
    if (t_name == t_spec.pk.name) {
        return t_spec.pk.type;
    }
    if (t_name == t_spec.tenant_column) {
        return FieldType::Text;
    }
    const Field* field = findField(t_spec, t_name);
    return field != nullptr ? field->type : FieldType::Text;
}

// Parses "eq.5" -> {Op::Eq, "5"}. Unknown operator prefix => nullopt, which
// callers turn into a 400, never a silently-ignored filter.
std::optional<std::pair<Op, std::string>> splitOpValue(const std::string& t_raw) {
    static const std::unordered_map<std::string, Op> kOps = {
        {"eq", Op::Eq},
        {"neq", Op::Neq},
        {"gt", Op::Gt},
        {"gte", Op::Gte},
        {"lt", Op::Lt},
        {"lte", Op::Lte},
        {"like", Op::Like},
        {"in", Op::In},
    };
    auto dot = t_raw.find('.');
    if (dot == std::string::npos) {
        return std::nullopt;
    }
    auto it = kOps.find(t_raw.substr(0, dot));
    if (it == kOps.end()) {
        return std::nullopt;
    }
    return std::make_pair(it->second, t_raw.substr(dot + 1));
}

std::string_view opSql(Op t_op) {
    switch (t_op) {
        case Op::Eq:
            return "=";
        case Op::Neq:
            return "!=";
        case Op::Gt:
            return ">";
        case Op::Gte:
            return ">=";
        case Op::Lt:
            return "<";
        case Op::Lte:
            return "<=";
        case Op::Like:
            return "LIKE";
        case Op::In:
            return "IN";
        default:
            return "=";
    }
}

Json::Value fieldToJson(const drogon::orm::Field& t_field, FieldType t_type) {
    if (t_field.isNull()) {
        return Json::Value::null;
    }
    try {
        switch (t_type) {
            case FieldType::Int:
                return Json::Value(t_field.as<int32_t>());
            case FieldType::Bool:
                return Json::Value(t_field.as<bool>());
            case FieldType::BigInt:
                // Deliberate, not a bug: JS Number loses precision past
                // 2^53-1, so int8 stays a decimal string on the wire.
                return Json::Value(t_field.as<std::string>());
            case FieldType::Jsonb: {
                const std::string raw = t_field.as<std::string>();
                Json::Value parsed;
                Json::CharReaderBuilder builder;
                std::string errors;
                const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
                if (reader->parse(raw.data(), raw.data() + raw.size(), &parsed, &errors)) {
                    return parsed;
                }
                // Corrupt payload must not throw out of the coroutine —
                // degrade to the raw text as a string.
                return Json::Value(raw);
            }
            case FieldType::Text:
            case FieldType::Timestamp:
            default:
                return Json::Value(t_field.as<std::string>());
        }
    } catch (const std::exception&) {
        return Json::Value::null;
    }
}

Json::Value rowToJson(const drogon::orm::Row& t_row, const CrudViewSpec& t_spec) {
    Json::Value r(Json::objectValue);
    for (std::size_t i = 0; i < t_row.size(); ++i) {
        drogon::orm::Field f = t_row[i];
        r[f.name()] = fieldToJson(f, resolveColumnType(t_spec, f.name()));
    }
    return r;
}

// --- raw-string list serialization (executeViewList only) ------------------
// Builds the response body directly instead of a Json::Value DOM that
// createJsonResponse would flatten back to text (two full passes over up to
// max_limit rows).

// Escapes exactly what the JSON spec requires — quote, backslash, and
// control characters below 0x20. Bytes 0x20 and above, including multi-byte
// UTF-8 sequences, pass through unescaped: valid JSON strings may contain
// raw UTF-8, and escaping it would double-encode. No UTF-8 validation here;
// Postgres text columns are already valid UTF-8 by the time they arrive.
void appendJsonEscapedString(std::string& out, std::string_view raw) {
    out += '"';
    for (unsigned char c : raw) {
        switch (c) {
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (c < 0x20) {
                    fmt::format_to(std::back_inserter(out), "\\u{:04x}", static_cast<unsigned>(c));
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

// Validity gate for the jsonb splice below: valid documents go out
// byte-exact with no DOM round-trip; anything else (corrupt or empty text)
// degrades to a quoted string so one bad value can never corrupt the
// surrounding array/object.
bool isValidJsonDocument(std::string_view t_raw) {
    if (t_raw.empty()) {
        return false;
    }
    Json::Value ignored;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    return reader->parse(t_raw.data(), t_raw.data() + t_raw.size(), &ignored, &errors);
}

void appendFieldValueRaw(std::string& out, const drogon::orm::Field& t_field, FieldType t_type) {
    if (t_field.isNull()) {
        out += "null";
        return;
    }
    try {
        switch (t_type) {
            case FieldType::Int:
                // The field text is already canonical decimal from Postgres —
                // valid JSON number syntax, splice unquoted.
                out += t_field.as<std::string>();
                break;
            case FieldType::BigInt:
                // Stays quoted (see fieldToJson): digits only, so the
                // escaper just wraps in quotes through one code path.
                appendJsonEscapedString(out, t_field.as<std::string>());
                break;
            case FieldType::Bool:
                // Reuse drogon's own text→bool parsing, not a hand-rolled
                // take on Postgres's 't'/'f' convention.
                out += t_field.as<bool>() ? "true" : "false";
                break;
            case FieldType::Jsonb: {
                // The column text IS the JSON document — splice raw, no
                // parse, no re-serialize. This is the whole point of the
                // raw-string path for this type.
                const std::string raw = t_field.as<std::string>();
                if (isValidJsonDocument(raw)) {
                    out += raw;
                } else {
                    appendJsonEscapedString(out, raw);
                }
                break;
            }
            case FieldType::Text:
            case FieldType::Timestamp:
            default:
                appendJsonEscapedString(out, t_field.as<std::string>());
                break;
        }
    } catch (const std::exception&) {
        out += "null";
    }
}

// Human-readable JSON type name for 400 messages (never leaks values).
std::string jsonTypeName(const Json::Value& t_value) {
    switch (t_value.type()) {
        case Json::nullValue:
            return "null";
        case Json::intValue:
        case Json::uintValue:
            return "integer";
        case Json::realValue:
            return "number";
        case Json::stringValue:
            return "string";
        case Json::booleanValue:
            return "boolean";
        case Json::arrayValue:
            return "array";
        case Json::objectValue:
            return "object";
        default:
            return "value";
    }
}

std::string fieldTypeName(FieldType t_type) {
    switch (t_type) {
        case FieldType::Int:
            return "integer";
        case FieldType::BigInt:
            return "bigint";
        case FieldType::Text:
            return "text";
        case FieldType::Bool:
            return "boolean";
        case FieldType::Timestamp:
            return "timestamp";
        case FieldType::Jsonb:
            return "jsonb";
    }
    return "value";
}

// Parses a decimal ([+-]?digits) or hex (0x/0X hexdigits) integer literal.
// Faulty input, overflow, and out-of-range magnitudes yield nullopt —
// callers turn that into a 400, never a truncated or wrapped bind.
std::optional<int64_t> parseIntegerLiteral(const std::string& t_in, bool t_big) {
    if (t_in.empty())
        return std::nullopt;

    // Hex: 0x prefix + at least one hex digit, magnitude checked below.
    if (t_in.size() > 2 && t_in[0] == '0' && (t_in[1] == 'x' || t_in[1] == 'X')) {
        uint64_t acc = 0;
        for (size_t i = 2; i < t_in.size(); ++i) {
            const char c = t_in[i];
            unsigned digit = 0;
            if (c >= '0' && c <= '9')
                digit = static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                digit = static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                digit = static_cast<unsigned>(c - 'A' + 10);
            else
                return std::nullopt;
            if (acc > (std::numeric_limits<uint64_t>::max() - digit) / 16)
                return std::nullopt;
            acc = acc * 16 + digit;
        }
        if (t_big) {
            if (acc > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                return std::nullopt;
        } else if (acc > static_cast<uint64_t>(std::numeric_limits<int32_t>::max())) {
            return std::nullopt;
        }
        return static_cast<int64_t>(acc);
    }

    // Decimal: optional sign, then digits only (from_chars rejects the empty
    // tail and reports overflow instead of wrapping).
    std::string_view view(t_in);
    if (!view.empty() && view.front() == '+')
        view.remove_prefix(1);
    if (view.empty())
        return std::nullopt;
    int64_t value = 0;
    const auto [ptr, ec] = std::from_chars(view.data(), view.data() + view.size(), value);
    if (ec != std::errc() || ptr != view.data() + view.size())
        return std::nullopt;
    if (!t_big && (value < std::numeric_limits<int32_t>::min() || value > std::numeric_limits<int32_t>::max()))
        return std::nullopt;
    return value;
}

// Integer bind shared by Int (32-bit) and BigInt (64-bit) columns. Accepts
// JSON integers, integral reals (30.0 — exactly representable, and jsoncpp
// itself reports them convertible via isInt64), decimal strings, and 0x hex
// strings; anything else — booleans, non-integral reals, null, containers,
// non-numeric or out-of-range strings — is a 400. Renders the canonical
// decimal form Postgres casts implicitly.
sgrn::Result<std::string> integerBind(std::string_view t_field, const Json::Value& t_value, bool t_big) {
    const FieldType type = t_big ? FieldType::BigInt : FieldType::Int;
    auto fail = [&]() {
        return sgrn::Result<std::string>::Error(
            fmt::format("Field '{}' expects {} but got {}", t_field, fieldTypeName(type), jsonTypeName(t_value)));
    };
    if (t_value.isInt64()) {
        const int64_t v = t_value.asInt64();
        if (!t_big && (v < std::numeric_limits<int32_t>::min() || v > std::numeric_limits<int32_t>::max()))
            return sgrn::Result<std::string>::Error(fmt::format("Field '{}' value out of range for {}", t_field, fieldTypeName(type)));
        return std::to_string(v);
    }
    if (t_value.isUInt64()) {
        const uint64_t u = t_value.asUInt64();
        const uint64_t limit =
            t_big ? static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) : static_cast<uint64_t>(std::numeric_limits<int32_t>::max());
        if (u > limit)
            return sgrn::Result<std::string>::Error(fmt::format("Field '{}' value out of range for {}", t_field, fieldTypeName(type)));
        return std::to_string(u);
    }
    if (t_value.isString()) {
        auto parsed = parseIntegerLiteral(t_value.asString(), t_big);
        if (!parsed.has_value())
            return fail();
        return std::to_string(parsed.value());
    }
    return fail();
}

std::vector<std::string> splitCsv(const std::string& t_in) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : t_in) {
        if (c == ',') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// Explicit column list (pk + tenant + whitelisted fields) so reads honour
// the same whitelist as filters/writes. This is deliberately stricter than
// `SELECT *`: columns omitted from spec.fields (e.g. password hashes,
// token secrets) are never returned, even though they exist on the table.
// (Spec text shows `SELECT *`; the whitelist intent — "nothing outside this
// is reachable" — requires the explicit list.)
std::string selectList(const CrudViewSpec& t_spec) {
    std::string out{std::string(t_spec.pk.name)};
    out += ", ";
    out += std::string(t_spec.tenant_column);
    for (const auto& f : t_spec.fields) {
        if (f.name == t_spec.pk.name || f.name == t_spec.tenant_column) {
            continue;
        }
        out += ", ";
        out += std::string(f.name);
    }
    return out;
}

} // namespace

sgrn::Result<std::string> fieldValueToBind(std::string_view t_field_name, FieldType t_type, const Json::Value& t_value) {
    switch (t_type) {
        case FieldType::Int:
            return integerBind(t_field_name, t_value, /*big=*/false);
        case FieldType::BigInt:
            return integerBind(t_field_name, t_value, /*big=*/true);
        case FieldType::Text:
            if (!t_value.isString()) {
                return sgrn::Result<std::string>::Error(
                    fmt::format("Field '{}' expects text but got {}", t_field_name, jsonTypeName(t_value)));
            }
            return t_value.asString();
        case FieldType::Bool:
            if (!t_value.isBool()) {
                return sgrn::Result<std::string>::Error(
                    fmt::format("Field '{}' expects boolean but got {}", t_field_name, jsonTypeName(t_value)));
            }
            return t_value.asBool() ? "true" : "false";
        case FieldType::Timestamp:
            // ISO strings only; epoch numbers are rejected so a wrong unit
            // (seconds vs milliseconds) can never silently shift a timestamp.
            if (!t_value.isString() || t_value.asString().empty()) {
                return sgrn::Result<std::string>::Error(
                    fmt::format("Field '{}' expects timestamp but got {}", t_field_name, jsonTypeName(t_value)));
            }
            return t_value.asString();
        case FieldType::Jsonb: {
            // Any JSON value is a valid jsonb document — re-serialize it
            // instead of asString(), which throws on objects/arrays.
            Json::StreamWriterBuilder builder;
            builder["indentation"] = "";
            return Json::writeString(builder, t_value);
        }
    }
    return sgrn::Result<std::string>::Error(fmt::format("Field '{}' has unknown type", t_field_name));
}

// --- LIST --------------------------------------------------------------

drogon::Task<HttpResponsePtr> executeViewList(const CrudViewSpec& t_spec, HttpRequestPtr tsp_req, std::string t_tenant) {
    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return sgrn::createJsonResponse(db_res);
    }

    // Tenant predicate is ALWAYS bound as $1, before any client-controlled
    // filter is considered. This is the direct replacement for
    // injectIsolationFilters()'s query-string rewriting.
    std::string sql = fmt::format("SELECT {} FROM {} WHERE {} = $1", selectList(t_spec), t_spec.read_relation, t_spec.tenant_column);
    std::vector<std::string> binds{t_tenant};

    for (const auto& [key, raw] : tsp_req->getParameters()) {
        if (key == "order" || key == "limit" || key == "offset") {
            continue;
        }

        const Field* field = findField(t_spec, key);
        auto parsed = splitOpValue(raw);
        if (field == nullptr || field->filter_ops == 0 || !parsed.has_value() ||
            !(field->filter_ops & static_cast<uint8_t>(parsed->first))) {
            // Explicit rejection, not silent ignoring — an unlisted column
            // or disallowed operator is a client error, not a no-op.
            co_return sgrn::createErrorResponse(fmt::format("Unsupported filter: {}", key), k400BadRequest, "Query");
        }
        if (parsed->first == Op::In) {
            auto parts = splitCsv(parsed->second);
            if (parts.empty()) {
                co_return sgrn::createErrorResponse(fmt::format("Unsupported filter: {}", key), k400BadRequest, "Query");
            }
            std::string placeholders;
            for (const auto& p : parts) {
                binds.push_back(p);
                if (!placeholders.empty()) {
                    placeholders += ", ";
                }
                placeholders += fmt::format("${}", binds.size());
            }
            sql += fmt::format(" AND {} IN ({})", field->name, placeholders);
        } else {
            binds.push_back(parsed->second);
            sql += fmt::format(" AND {} {} ${}", field->name, opSql(parsed->first), binds.size());
        }
    }

    std::string order_col(t_spec.default_order);
    bool desc = false;
    if (auto it = tsp_req->getParameters().find("order"); it != tsp_req->getParameters().end()) {
        const std::string& order_raw = it->second;
        desc = order_raw.size() >= 5 && order_raw.compare(order_raw.size() - 5, 5, ".desc") == 0;
        std::string col = order_raw.substr(0, order_raw.find('.'));
        const Field* f = findField(t_spec, col);
        if (f == nullptr) {
            co_return sgrn::createErrorResponse("Unsupported sort column", k400BadRequest, "Query");
        }
        order_col = std::string(f->name);
    }
    sql += fmt::format(" ORDER BY {} {}", order_col, desc ? "DESC" : "ASC");

    std::size_t limit = t_spec.max_limit;
    if (auto it = tsp_req->getParameters().find("limit"); it != tsp_req->getParameters().end()) {
        std::size_t parsed_limit = limit;
        auto [ptr, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), parsed_limit);
        if (ec == std::errc()) {
            limit = std::min(parsed_limit, t_spec.max_limit); // client can shrink but never exceed max_limit
        }
    }
    sql += fmt::format(" LIMIT {}", limit);

    if (auto it = tsp_req->getParameters().find("offset"); it != tsp_req->getParameters().end()) {
        std::size_t offset = 0;
        auto [ptr, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), offset);
        if (ec == std::errc()) {
            sql += fmt::format(" OFFSET {}", offset);
        }
    }

    try {
        auto result = co_await sgrn::datastore::core::execSqlCoroVec(db_res.value(), sql, binds);
        // Single-pass serialization: build the response body directly
        // instead of a Json::Value DOM (see appendFieldValueRaw above).
        std::string body;
        body.reserve(result.size() * 128);
        body += '[';
        bool first_row = true;
        for (const auto& row : result) {
            if (!first_row) {
                body += ',';
            }
            first_row = false;
            body += '{';
            bool first_col = true;
            for (std::size_t i = 0; i < row.size(); ++i) {
                drogon::orm::Field f = row[i];
                if (!first_col) {
                    body += ',';
                }
                first_col = false;
                appendJsonEscapedString(body, f.name());
                body += ':';
                appendFieldValueRaw(body, f, resolveColumnType(t_spec, f.name()));
            }
            body += '}';
        }
        body += ']';
        co_return sgrn::createJsonResponse(std::move(body), k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createErrorResponse(e.what(), k500InternalServerError, "Database");
    }
}

// --- GET one -------------------------------------------------------------

drogon::Task<HttpResponsePtr> executeViewGet(const CrudViewSpec& t_spec, HttpRequestPtr, std::string t_tenant, std::string t_id) {
    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return sgrn::createJsonResponse(db_res);
    }

    std::string sql = fmt::format(
        "SELECT {} FROM {} WHERE {} = $1 AND {} = $2", selectList(t_spec), t_spec.read_relation, t_spec.pk.name, t_spec.tenant_column);
    try {
        auto result = co_await db_res.value()->execSqlCoro(sql, t_id, t_tenant);
        if (result.empty()) {
            co_return sgrn::createErrorResponse("No matching row", k404NotFound, "NotFound");
        }
        co_return sgrn::createJsonResponse(rowToJson(result[0], t_spec), k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createErrorResponse(e.what(), k500InternalServerError, "Database");
    }
}

// --- INSERT ----------------------------------------------------------------

drogon::Task<HttpResponsePtr> executeViewInsert(const CrudViewSpec& t_spec, HttpRequestPtr tsp_req, std::string t_tenant) {
    auto json = tsp_req->getJsonObject();
    if (!json) {
        co_return sgrn::createErrorResponse("Expected JSON body", k400BadRequest, "Body");
    }

    // tenant_column is always first and always server-derived — the body
    // cannot set it, even if it includes a key with that name.
    std::vector<std::string> cols{std::string(t_spec.tenant_column)};
    std::vector<std::string> binds{t_tenant};
    for (const auto& field : t_spec.fields) {
        if (!field.insertable || !json->isMember(std::string(field.name))) {
            continue;
        }
        auto bind_res = fieldValueToBind(field.name, field.type, (*json)[std::string(field.name)]);
        if (bind_res.hasError()) {
            co_return sgrn::createErrorResponse(bind_res.error(), k400BadRequest, "Body");
        }
        cols.emplace_back(field.name);
        binds.push_back(std::move(bind_res).value());
    }

    std::string col_list;
    std::string placeholder_list;
    for (std::size_t i = 0; i < cols.size(); ++i) {
        if (i) {
            col_list += ", ";
            placeholder_list += ", ";
        }
        col_list += cols[i];
        placeholder_list += fmt::format("${}", i + 1);
    }
    std::string sql =
        fmt::format("INSERT INTO {} ({}) VALUES ({}) RETURNING {}", t_spec.write_table, col_list, placeholder_list, selectList(t_spec));

    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return sgrn::createJsonResponse(db_res);
    }
    try {
        auto result = co_await sgrn::datastore::core::execSqlCoroVec(db_res.value(), sql, binds);
        if (result.empty()) {
            co_return sgrn::createErrorResponse("Insert returned no row", k500InternalServerError, "Database");
        }
        co_return sgrn::createJsonResponse(rowToJson(result[0], t_spec), k201Created);
    } catch (const std::exception& e) {
        co_return sgrn::createErrorResponse(e.what(), k500InternalServerError, "Database");
    }
}

// --- UPDATE ------------------------------------------------------------

drogon::Task<HttpResponsePtr> executeViewUpdate(
    const CrudViewSpec& t_spec, HttpRequestPtr tsp_req, std::string t_tenant, std::string t_id) {
    auto json = tsp_req->getJsonObject();
    if (!json) {
        co_return sgrn::createErrorResponse("Expected JSON body", k400BadRequest, "Body");
    }

    std::vector<std::string> binds;
    std::string set_clause;
    for (const auto& field : t_spec.fields) {
        if (!field.updatable || !json->isMember(std::string(field.name))) {
            continue;
        }
        // The tenant column can never be updated, even if a generator bug
        // ever marked it updatable — defence in depth.
        if (field.name == t_spec.tenant_column) {
            continue;
        }
        auto bind_res = fieldValueToBind(field.name, field.type, (*json)[std::string(field.name)]);
        if (bind_res.hasError()) {
            co_return sgrn::createErrorResponse(bind_res.error(), k400BadRequest, "Body");
        }
        binds.push_back(std::move(bind_res).value());
        if (!set_clause.empty()) {
            set_clause += ", ";
        }
        set_clause += fmt::format("{} = ${}", field.name, binds.size());
    }
    if (set_clause.empty()) {
        co_return sgrn::createErrorResponse("No updatable fields supplied", k400BadRequest, "Body");
    }

    binds.push_back(t_id);
    binds.push_back(t_tenant);
    std::string sql = fmt::format("UPDATE {} SET {} WHERE {} = ${} AND {} = ${} RETURNING {}", t_spec.write_table, set_clause,
        t_spec.pk.name, binds.size() - 1, t_spec.tenant_column, binds.size(), selectList(t_spec));

    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return sgrn::createJsonResponse(db_res);
    }
    try {
        auto result = co_await sgrn::datastore::core::execSqlCoroVec(db_res.value(), sql, binds);
        if (result.empty()) {
            co_return sgrn::createErrorResponse("No matching row", k404NotFound, "NotFound");
        }
        co_return sgrn::createJsonResponse(rowToJson(result[0], t_spec), k200OK);
    } catch (const std::exception& e) {
        co_return sgrn::createErrorResponse(e.what(), k500InternalServerError, "Database");
    }
}

// --- DELETE ------------------------------------------------------------

drogon::Task<HttpResponsePtr> executeViewDelete(const CrudViewSpec& t_spec, HttpRequestPtr, std::string t_tenant, std::string t_id) {
    std::string sql = fmt::format("DELETE FROM {} WHERE {} = $1 AND {} = $2", t_spec.write_table, t_spec.pk.name, t_spec.tenant_column);

    auto db_res = sgrn::datastore::core::getDbClient();
    if (db_res.hasError()) {
        co_return sgrn::createJsonResponse(db_res);
    }
    try {
        co_await db_res.value()->execSqlCoro(sql, t_id, t_tenant);
        co_return drogon::HttpResponse::newHttpResponse(k204NoContent, drogon::CT_NONE);
    } catch (const std::exception& e) {
        co_return sgrn::createErrorResponse(e.what(), k500InternalServerError, "Database");
    }
}

} // namespace sgrn::datastore::query
