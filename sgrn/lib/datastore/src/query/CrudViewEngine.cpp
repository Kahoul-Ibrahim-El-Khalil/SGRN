#include <sgrn/datastore/query/CrudViewEngine.hpp>

#include <sgrn/datastore/core/db.hpp>
#include <sgrn/datastore/utils/respond.hpp>
#include <sgrn/datastore/utils/safe_access.hpp>

#include <fmt/core.h>
#include <algorithm>
#include <charconv>
#include <json/json.h>
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

Json::Value fieldToJson(const drogon::orm::Field& t_field) {
    if (t_field.isNull()) {
        return Json::Value::null;
    }
    try {
        return Json::Value(t_field.as<std::string>());
    } catch (const std::exception&) {
        return Json::Value::null;
    }
}

Json::Value rowToJson(const drogon::orm::Row& t_row) {
    Json::Value r(Json::objectValue);
    for (std::size_t i = 0; i < t_row.size(); ++i) {
        drogon::orm::Field f = t_row[i];
        r[f.name()] = fieldToJson(f);
    }
    return r;
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
        Json::Value rows = Json::arrayValue;
        for (const auto& row : result) {
            rows.append(rowToJson(row));
        }
        co_return sgrn::createJsonResponse(rows, k200OK);
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
        co_return sgrn::createJsonResponse(rowToJson(result[0]), k200OK);
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
        cols.emplace_back(field.name);
        binds.push_back((*json)[std::string(field.name)].asString());
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
        co_return sgrn::createJsonResponse(rowToJson(result[0]), k201Created);
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
        binds.push_back((*json)[std::string(field.name)].asString());
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
        co_return sgrn::createJsonResponse(rowToJson(result[0]), k200OK);
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
