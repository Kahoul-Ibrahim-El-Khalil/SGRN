#include <sgrn/crud/AuditLogger.hpp>
#include <sgrn/crud/CrudViewEngine.hpp>
#include <sgrn/crud/FilterHookRegistry.hpp>
#include <sgrn/crud/Validators.hpp>

#include <drogon/HttpAppFramework.h>
#include <drogon/orm/DbClient.h>
#include <drogon/orm/Exception.h>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <memory>
#include <sstream>
#include <string_view>

namespace sgrn::crud
{

namespace
{

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
                    out += "\\u00";
                    out += "0123456789abcdef"[c >> 4];
                    out += "0123456789abcdef"[c & 0xf];
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

bool isValidJsonDocument(std::string_view raw) {
    if (raw.empty())
        return false;
    Json::Value ignored;
    Json::CharReaderBuilder builder;
    std::string errors;
    const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    return reader->parse(raw.data(), raw.data() + raw.size(), &ignored, &errors);
}

drogon::HttpResponsePtr makeError(
    std::string_view msg, drogon::HttpStatusCode status = drogon::k400BadRequest, std::string_view scope = "Client") {
    std::string body;
    body.reserve(msg.size() + scope.size() + 32);
    body += R"({"error":)";
    appendJsonEscapedString(body, msg);
    body += R"(,"scope":)";
    appendJsonEscapedString(body, scope);
    body += '}';

    auto resp = drogon::HttpResponse::newCustomHttpResponse(std::move(body));
    resp->setStatusCode(status);
    resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
    return resp;
}

const CrudFieldSpec* findField(const CrudViewSpec& spec, std::string_view name) {
    for (const auto& f : spec.fields) {
        if (f.name == name) {
            return &f;
        }
    }
    return nullptr;
}

std::string_view fieldTypeToPgCast(FieldType type) {
    switch (type) {
        case FieldType::Int:
            return "::integer";
        case FieldType::BigInt:
            return "::bigint";
        case FieldType::Bool:
            return "::boolean";
        case FieldType::Timestamp:
            return "::timestamptz";
        case FieldType::Jsonb:
            return "::jsonb";
        case FieldType::Text:
        default:
            return "::text";
    }
}

void appendFieldValueRaw(std::string& out, const drogon::orm::Field& field, FieldType type) {
    if (field.isNull()) {
        out += "null";
        return;
    }

    switch (type) {
        case FieldType::Int: {
            out += std::to_string(field.as<int64_t>());
            break;
        }
        case FieldType::Bool: {
            out += (field.as<bool>() ? "true" : "false");
            break;
        }
        case FieldType::BigInt: {
            appendJsonEscapedString(out, field.as<std::string_view>());
            break;
        }
        case FieldType::Jsonb: {
            std::string_view raw = field.as<std::string_view>();
            if (isValidJsonDocument(raw)) {
                out.append(raw.data(), raw.size());
            } else {
                appendJsonEscapedString(out, raw);
            }
            break;
        }
        case FieldType::Timestamp:
        case FieldType::Text:
        default: {
            appendJsonEscapedString(out, field.as<std::string_view>());
            break;
        }
    }
}

void appendRowJson(std::string& out, const drogon::orm::Row& row, const CrudViewSpec& spec) {
    out += '{';
    bool first = true;
    for (size_t i = 0; i < row.size(); ++i) {
        if (!first)
            out += ',';
        first = false;

        std::string_view col_name = row[i].name();
        appendJsonEscapedString(out, col_name);
        out += ':';

        const CrudFieldSpec* field = findField(spec, col_name);
        FieldType type = field ? field->type : FieldType::Text;

        if (col_name == spec.primary_key && spec.primary_key_type == "int") {
            type = FieldType::Int;
        }

        appendFieldValueRaw(out, row[i], type);
    }
    out += '}';
}

} // namespace

drogon::Task<drogon::HttpResponsePtr> CrudViewEngine::executeList(
    const CrudViewSpec& spec, drogon::HttpRequestPtr req, std::string tenant) {
    std::vector<std::string> select_cols;
    if (!spec.primary_key.empty())
        select_cols.push_back(spec.primary_key);
    if (!spec.tenant_column.empty())
        select_cols.push_back(spec.tenant_column);

    for (const auto& field : spec.fields) {
        if (field.readable && field.name != spec.primary_key && field.name != spec.tenant_column) {
            select_cols.push_back(field.name);
        }
    }

    std::ostringstream sql;
    sql << "SELECT ";
    for (size_t i = 0; i < select_cols.size(); ++i) {
        if (i > 0)
            sql << ", ";
        sql << "\"" << select_cols[i] << "\"";
    }
    sql << " FROM \"" << spec.table_schema << "\".\"" << spec.table_name << "\" WHERE 1=1";

    std::vector<std::string> params;
    int param_idx = 1;

    if (!spec.tenant_column.empty() && !tenant.empty() && spec.policy.read_scope == ReadScope::Org) {
        sql << " AND \"" << spec.tenant_column << "\" = $" << param_idx++;
        params.push_back(tenant);
    }

    if (spec.policy.soft_delete) {
        sql << " AND \"deleted_at\" IS NULL";
    }

    // Filter query parameters PostgREST format: col=op.value
    auto parameters = req->getParameters();
    for (const auto& [param, value] : parameters) {
        if (param == "order" || param == "limit" || param == "offset") {
            continue;
        }

        const CrudFieldSpec* field = findField(spec, param);
        if (!field || !field->filterable) {
            co_return makeError("Unsupported filter column: " + param, drogon::k400BadRequest, "Query");
        }

        auto dot_pos = value.find('.');
        if (dot_pos == std::string::npos) {
            co_return makeError(
                "Invalid filter format for column '" + param + "'. Expected operator.value", drogon::k400BadRequest, "Query");
        }

        std::string op_str = value.substr(0, dot_pos);
        std::string raw_val = value.substr(dot_pos + 1);

        uint8_t required_op = Op::None;
        if (op_str == "eq")
            required_op = Op::Eq;
        else if (op_str == "neq")
            required_op = Op::Neq;
        else if (op_str == "gt")
            required_op = Op::Gt;
        else if (op_str == "gte")
            required_op = Op::Gte;
        else if (op_str == "lt")
            required_op = Op::Lt;
        else if (op_str == "lte")
            required_op = Op::Lte;
        else if (op_str == "like")
            required_op = Op::Like;
        else if (op_str == "in")
            required_op = Op::In;

        if (required_op == Op::None || !(field->operators & required_op)) {
            co_return makeError("Operator '" + op_str + "' not allowed on field '" + param + "'", drogon::k400BadRequest, "Query");
        }

        std::string_view cast = fieldTypeToPgCast(field->type);

        if (op_str == "eq") {
            sql << " AND \"" << param << "\" = $" << param_idx++ << cast;
            params.push_back(raw_val);
        } else if (op_str == "neq") {
            sql << " AND \"" << param << "\" != $" << param_idx++ << cast;
            params.push_back(raw_val);
        } else if (op_str == "gt") {
            sql << " AND \"" << param << "\" > $" << param_idx++ << cast;
            params.push_back(raw_val);
        } else if (op_str == "gte") {
            sql << " AND \"" << param << "\" >= $" << param_idx++ << cast;
            params.push_back(raw_val);
        } else if (op_str == "lt") {
            sql << " AND \"" << param << "\" < $" << param_idx++ << cast;
            params.push_back(raw_val);
        } else if (op_str == "lte") {
            sql << " AND \"" << param << "\" <= $" << param_idx++ << cast;
            params.push_back(raw_val);
        } else if (op_str == "like") {
            sql << " AND \"" << param << "\" LIKE $" << param_idx++;
            params.push_back(raw_val);
        } else if (op_str == "in") {
            std::stringstream ss(raw_val);
            std::string item;
            std::vector<std::string> items;
            while (std::getline(ss, item, ',')) {
                if (!item.empty())
                    items.push_back(item);
            }
            if (items.empty()) {
                co_return makeError("Empty list for 'in' operator on column '" + param + "'", drogon::k400BadRequest, "Query");
            }
            sql << " AND \"" << param << "\" IN (";
            for (size_t idx = 0; idx < items.size(); ++idx) {
                if (idx > 0)
                    sql << ", ";
                sql << "$" << param_idx++ << cast;
                params.push_back(items[idx]);
            }
            sql << ")";
        }
    }

    // Order clause
    std::string order_param = req->getParameter("order");
    if (!order_param.empty()) {
        std::string order_col = order_param;
        std::string direction = "ASC";
        auto desc_pos = order_param.find(".desc");
        if (desc_pos != std::string::npos) {
            order_col = order_param.substr(0, desc_pos);
            direction = "DESC";
        }

        const CrudFieldSpec* field = findField(spec, order_col);
        if (!field && order_col != spec.primary_key) {
            co_return makeError("Invalid sort column: " + order_col, drogon::k400BadRequest, "Query");
        }
        sql << " ORDER BY \"" << order_col << "\" " << direction;
    } else if (!spec.default_order.empty()) {
        sql << " ORDER BY " << spec.default_order;
    }

    // Limit and Offset
    uint32_t limit = spec.policy.max_limit;
    std::string limit_str = req->getParameter("limit");
    if (!limit_str.empty()) {
        try {
            uint32_t req_limit = std::stoul(limit_str);
            limit = std::min(req_limit, spec.policy.max_limit);
        } catch (...) {
        }
    }
    sql << " LIMIT " << limit;

    std::string offset_str = req->getParameter("offset");
    if (!offset_str.empty()) {
        try {
            uint32_t offset = std::stoul(offset_str);
            sql << " OFFSET " << offset;
        } catch (...) {
        }
    }

    try {
        auto db = drogon::app().getDbClient();
        drogon::orm::Result r = co_await db->execSqlCoro<std::string>(sql.str(), params);

        std::string body;
        body.reserve(r.size() * 128 + 2);
        body += '[';
        bool first_row = true;
        for (const auto& row : r) {
            if (!first_row)
                body += ',';
            first_row = false;
            appendRowJson(body, row, spec);
        }
        body += ']';

        auto resp = drogon::HttpResponse::newCustomHttpResponse(std::move(body));
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        co_return resp;
    } catch (const drogon::orm::DrogonDbException& e) {
        LOG_ERROR << "Database error in executeList: " << e.base().what();
        co_return makeError("Database error during list operation", drogon::k500InternalServerError, "Database");
    }
}

drogon::Task<drogon::HttpResponsePtr> CrudViewEngine::executeGet(
    const CrudViewSpec& spec, drogon::HttpRequestPtr req, std::string tenant, std::string id) {
    (void)req;
    std::vector<std::string> select_cols;
    if (!spec.primary_key.empty())
        select_cols.push_back(spec.primary_key);
    if (!spec.tenant_column.empty())
        select_cols.push_back(spec.tenant_column);

    for (const auto& field : spec.fields) {
        if (field.readable && field.name != spec.primary_key && field.name != spec.tenant_column) {
            select_cols.push_back(field.name);
        }
    }

    std::ostringstream sql;
    sql << "SELECT ";
    for (size_t i = 0; i < select_cols.size(); ++i) {
        if (i > 0)
            sql << ", ";
        sql << "\"" << select_cols[i] << "\"";
    }
    sql << " FROM \"" << spec.table_schema << "\".\"" << spec.table_name << "\" WHERE \"" << spec.primary_key << "\" = $1";

    std::vector<std::string> params{id};
    int param_idx = 2;

    if (!spec.tenant_column.empty() && !tenant.empty() && spec.policy.read_scope == ReadScope::Org) {
        sql << " AND \"" << spec.tenant_column << "\" = $" << param_idx++;
        params.push_back(tenant);
    }

    if (spec.policy.soft_delete) {
        sql << " AND \"deleted_at\" IS NULL";
    }

    try {
        auto db = drogon::app().getDbClient();
        drogon::orm::Result r = co_await db->execSqlCoro<std::string>(sql.str(), params);

        if (r.empty()) {
            co_return makeError("No matching record found", drogon::k404NotFound, "NotFound");
        }

        std::string body;
        body.reserve(256);
        appendRowJson(body, r[0], spec);

        auto resp = drogon::HttpResponse::newCustomHttpResponse(std::move(body));
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        co_return resp;
    } catch (const drogon::orm::DrogonDbException& e) {
        LOG_ERROR << "Database error in executeGet: " << e.base().what();
        co_return makeError("Database error during get operation", drogon::k500InternalServerError, "Database");
    }
}

drogon::Task<drogon::HttpResponsePtr> CrudViewEngine::executeCreate(
    const CrudViewSpec& spec, drogon::HttpRequestPtr req, std::string tenant) {
    auto json_req = req->getJsonObject();
    if (!json_req) {
        co_return makeError("Missing or invalid JSON body", drogon::k400BadRequest, "Body");
    }

    std::vector<std::string> cols;
    std::vector<std::string> params;

    if (!spec.tenant_column.empty() && !tenant.empty()) {
        cols.push_back(spec.tenant_column);
        params.push_back(tenant);
    }

    for (const auto& field : spec.fields) {
        if (!field.writable || field.name == spec.primary_key || field.name == spec.tenant_column || field.soft_delete_field) {
            continue;
        }

        if (json_req->isMember(field.name)) {
            const auto& val = (*json_req)[field.name];
            std::string err;
            if (!Validators::validateField(field, val, err)) {
                co_return makeError(err, drogon::k400BadRequest, "Body");
            }

            cols.push_back(field.name);
            if (val.isString()) {
                params.push_back(val.asString());
            } else if (val.isBool()) {
                params.push_back(val.asBool() ? "true" : "false");
            } else if (val.isObject() || val.isArray()) {
                Json::FastWriter w;
                params.push_back(w.write(val));
            } else {
                params.push_back(val.asString());
            }
        }
    }

    std::ostringstream sql;
    sql << "INSERT INTO \"" << spec.table_schema << "\".\"" << spec.table_name << "\" (";
    for (size_t i = 0; i < cols.size(); ++i) {
        if (i > 0)
            sql << ", ";
        sql << "\"" << cols[i] << "\"";
    }
    sql << ") VALUES (";
    for (size_t i = 0; i < cols.size(); ++i) {
        if (i > 0)
            sql << ", ";
        sql << "$" << (i + 1);
    }
    sql << ") RETURNING *";

    try {
        auto db = drogon::app().getDbClient();
        drogon::orm::Result r = co_await db->execSqlCoro<std::string>(sql.str(), params);

        if (r.empty()) {
            co_return makeError("Failed to insert record", drogon::k500InternalServerError, "Database");
        }

        std::string body;
        body.reserve(256);
        appendRowJson(body, r[0], spec);

        if (spec.policy.audit) {
            AuditLogger::logMutation(spec.table_name, "CREATE", tenant, "");
        }

        auto resp = drogon::HttpResponse::newCustomHttpResponse(std::move(body));
        resp->setStatusCode(drogon::k201Created);
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        co_return resp;
    } catch (const drogon::orm::DrogonDbException& e) {
        LOG_ERROR << "Database error in executeCreate: " << e.base().what();
        co_return makeError("Database constraint error: " + std::string(e.base().what()), drogon::k400BadRequest, "Database");
    }
}

drogon::Task<drogon::HttpResponsePtr> CrudViewEngine::executeUpdate(
    const CrudViewSpec& spec, drogon::HttpRequestPtr req, std::string tenant, std::string id) {
    auto json_req = req->getJsonObject();
    if (!json_req) {
        co_return makeError("Missing or invalid JSON body", drogon::k400BadRequest, "Body");
    }

    std::vector<std::string> set_clauses;
    std::vector<std::string> params;
    int param_idx = 1;

    for (const auto& field : spec.fields) {
        if (!field.writable || field.name == spec.primary_key || field.name == spec.tenant_column || field.soft_delete_field) {
            continue;
        }

        if (json_req->isMember(field.name)) {
            const auto& val = (*json_req)[field.name];
            std::string err;
            if (!Validators::validateField(field, val, err)) {
                co_return makeError(err, drogon::k400BadRequest, "Body");
            }

            std::string_view cast = fieldTypeToPgCast(field.type);
            set_clauses.push_back("\"" + field.name + "\" = $" + std::to_string(param_idx++) + std::string(cast));

            if (val.isString()) {
                params.push_back(val.asString());
            } else if (val.isBool()) {
                params.push_back(val.asBool() ? "true" : "false");
            } else if (val.isObject() || val.isArray()) {
                Json::FastWriter w;
                params.push_back(w.write(val));
            } else {
                params.push_back(val.asString());
            }
        }
    }

    if (set_clauses.empty()) {
        co_return makeError("No updatable fields supplied in request body", drogon::k400BadRequest, "Body");
    }

    std::ostringstream sql;
    sql << "UPDATE \"" << spec.table_schema << "\".\"" << spec.table_name << "\" SET ";
    for (size_t i = 0; i < set_clauses.size(); ++i) {
        if (i > 0)
            sql << ", ";
        sql << set_clauses[i];
    }
    sql << " WHERE \"" << spec.primary_key << "\" = $" << param_idx++;
    params.push_back(id);

    if (!spec.tenant_column.empty() && !tenant.empty() && spec.policy.write_scope == WriteScope::Org) {
        sql << " AND \"" << spec.tenant_column << "\" = $" << param_idx++;
        params.push_back(tenant);
    }

    if (spec.policy.soft_delete) {
        sql << " AND \"deleted_at\" IS NULL";
    }

    sql << " RETURNING *";

    try {
        auto db = drogon::app().getDbClient();
        drogon::orm::Result r = co_await db->execSqlCoro<std::string>(sql.str(), params);

        if (r.empty()) {
            co_return makeError("No matching record found to update", drogon::k404NotFound, "NotFound");
        }

        std::string body;
        body.reserve(256);
        appendRowJson(body, r[0], spec);

        if (spec.policy.audit) {
            AuditLogger::logMutation(spec.table_name, "UPDATE", tenant, id);
        }

        auto resp = drogon::HttpResponse::newCustomHttpResponse(std::move(body));
        resp->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        co_return resp;
    } catch (const drogon::orm::DrogonDbException& e) {
        LOG_ERROR << "Database error in executeUpdate: " << e.base().what();
        co_return makeError("Database constraint error during update", drogon::k400BadRequest, "Database");
    }
}

drogon::Task<drogon::HttpResponsePtr> CrudViewEngine::executeDelete(
    const CrudViewSpec& spec, drogon::HttpRequestPtr req, std::string tenant, std::string id) {
    (void)req;
    std::ostringstream sql;
    std::vector<std::string> params{id};
    int param_idx = 2;

    if (spec.policy.soft_delete) {
        sql << "UPDATE \"" << spec.table_schema << "\".\"" << spec.table_name << "\" SET \"deleted_at\" = NOW() WHERE \""
            << spec.primary_key << "\" = $1";
    } else {
        sql << "DELETE FROM \"" << spec.table_schema << "\".\"" << spec.table_name << "\" WHERE \"" << spec.primary_key << "\" = $1";
    }

    if (!spec.tenant_column.empty() && !tenant.empty() && spec.policy.delete_scope == DeleteScope::Org) {
        sql << " AND \"" << spec.tenant_column << "\" = $" << param_idx++;
        params.push_back(tenant);
    }

    try {
        auto db = drogon::app().getDbClient();
        co_await db->execSqlCoro<std::string>(sql.str(), params);

        if (spec.policy.audit) {
            AuditLogger::logMutation(spec.table_name, "DELETE", tenant, id);
        }

        auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k204NoContent);
        co_return resp;
    } catch (const drogon::orm::DrogonDbException& e) {
        LOG_ERROR << "Database error in executeDelete: " << e.base().what();
        co_return makeError("Database error during delete operation", drogon::k500InternalServerError, "Database");
    }
}

} // namespace sgrn::crud
