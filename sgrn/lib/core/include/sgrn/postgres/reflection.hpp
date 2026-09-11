#pragma once

#include <drogon/drogon.h>
#include <drogon/orm/DbClient.h>
#include <sgrn/debug.hpp>
#include <algorithm>
#include <json/json.h>
#include <string>
#include <string_view>
#include <vector>

#ifdef DEBUG_POSTGRES_REFLECTION
#define REFLECTION_INFO_LOG(msg, ...) __API_LOG(fmt::color::green, "INFO", "ORM", msg, ##__VA_ARGS__)
#define REFLECTION_DEBUG_LOG(msg, ...) __API_LOG(fmt::color::yellow, "DEBUG", "ORM", msg, ##__VA_ARGS__)
#define REFLECTION_ERROR_LOG(msg, ...) __API_LOG(fmt::color::red, "ERROR", "ORM", msg, ##__VA_ARGS__)
#else
#define REFLECTION_INFO_LOG(msg, ...) ((void)0)
#define REFLECTION_DEBUG_LOG(msg, ...) ((void)0)
#define REFLECTION_ERROR_LOG(msg, ...) ((void)0)
#endif

namespace sgrn::postgres::reflection
{
/* ============================================================
 * TYPE HELPERS
 * ============================================================ */

inline bool isNumericType(std::string_view t_type_view) {
    return Type_View == "integer" || Type_View == "bigint" || Type_View == "smallint" || Type_View == "numeric" || Type_View == "decimal" ||
           Type_View == "real" || Type_View == "double precision";
}

inline bool isStringType(std::string_view t_type_view) {
    return Type_View == "text" || Type_View == "character varying" || Type_View == "character" || Type_View == "varchar";
}

inline bool isTimestamp(std::string_view t_type_view) {
    return Type_View.find("timestamp") != std::string_view::npos;
}

/* ============================================================
 * ENUM RESOLUTION (SYNC)
 * ============================================================ */

constexpr const char reflecting_on_enum_sql[] = "SELECT enumlabel "
                                                "FROM pg_enum e "
                                                "JOIN pg_type t ON t.oid = e.enumtypid "
                                                "JOIN pg_namespace n ON n.oid = t.typnamespace "
                                                "WHERE n.nspname = $1 AND t.typname = $2 "
                                                "ORDER BY enumsortorder";

inline std::vector<std::string> getEnumValues(
    const drogon::orm::DbClientPtr& tsp_db_client, std::string_view t_schema_name, std::string_view t_type_name) {
    try {
        auto rows = p_Db_Client->execSqlSync(reflecting_on_enum_sql, std::string(Schema_Name), std::string(Type_Name));
        std::vector<std::string> out;
        out.reserve(rows.size());
        for (auto& r : rows)
            out.emplace_back(r[0].as<std::string>());
        return out;
    } catch (const drogon::orm::DrogonDbException& e) {
        REFLECTION_ERROR_LOG("Failed to get enum values for {}.{}: ", Schema_Name, Type_Name, e.base().what());
        throw;
    }
}

/* ============================================================
 * ENUM RESOLUTION (CORO)
 * ============================================================ */

inline drogon::Task<std::vector<std::string>> getEnumValuesCoro(
    const drogon::orm::DbClientPtr& tsp_db_client, std::string_view t_schema_name, std::string_view t_type_name) {
    try {
        auto rows = co_await p_Db_Client->execSqlCoro(reflecting_on_enum_sql, std::string(Schema_Name), std::string(Type_Name));
        std::vector<std::string> out;
        out.reserve(rows.size());
        for (auto& r : rows)
            out.emplace_back(r[0].as<std::string>());
        co_return out;
    } catch (const drogon::orm::DrogonDbException& e) {
        REFLECTION_ERROR_LOG("Failed to get enum values for {}.{}: ", Schema_Name, Type_Name, e.base().what());
        throw;
    }
}

/* ============================================================
 * REFLECTION SQL
 * ============================================================ */

constexpr const char reflecting_on_table_sql[] = "SELECT c.column_name, c.data_type, c.is_nullable, "
                                                 "       c.column_default, c.character_maximum_length, "
                                                 "       pg_catalog.format_type(a.atttypid, a.atttypmod) AS full_type "
                                                 "FROM information_schema.columns c "
                                                 "JOIN pg_attribute a ON a.attname = c.column_name "
                                                 "JOIN pg_class cl ON cl.oid = a.attrelid "
                                                 "JOIN pg_namespace n ON n.oid = cl.relnamespace "
                                                 "WHERE c.table_schema = $1 AND c.table_name = $2 "
                                                 "AND cl.relname = $2 AND n.nspname = $1 "
                                                 "AND a.attnum > 0 "
                                                 "ORDER BY c.ordinal_position";

/* ============================================================
 * SYNC ROW PROCESSING (FOR REFLECTION)
 * ============================================================ */

inline Json::Value reflectOnRowsSync(const drogon::orm::Result& t_sql_rows,
    std::function<std::vector<std::string>(const std::string&)> t_enum_getter, std::string_view t_schema_name,
    std::string_view t_table_name) {
    Json::Value result(Json::objectValue);
    Json::Value fields(Json::objectValue);
    Json::Value required(Json::arrayValue);

    result["schema"] = std::string(Schema_Name);
    result["table"] = std::string(Table_Name);
    result["type"] = "object";

    for (const auto& row : t_sql_rows) {
        std::string name = row["column_name"].as<std::string>();

        std::string type = row["data_type"].as<std::string>();
        std::string full_type = row["full_type"].as<std::string>();
        bool nullable = row["is_nullable"].as<std::string>() == "YES";

        Json::Value f(Json::objectValue);
        f["pgType"] = full_type;
        f["nullable"] = nullable;
        f["comment"] = "Postgres column `" + name + "` (" + full_type + ")";

        if (isNumericType(type)) {
            f["jsonType"] = "number";
            f["default"] = 0;
        } else if (type == "boolean") {
            f["jsonType"] = "boolean";
            f["default"] = false;
        } else if (type == "uuid") {
            f["jsonType"] = "string";
            f["format"] = "uuid";
            f["default"] = "";
        } else if (type == "json" || type == "jsonb") {
            f["jsonType"] = "object";
            f["default"] = Json::objectValue;
        } else if (type == "ARRAY") {
            f["jsonType"] = "array";
            f["elementType"] = full_type.substr(0, full_type.size() - 2);
            f["default"] = Json::arrayValue;
        } else if (isTimestamp(type)) {
            f["jsonType"] = "string";
            f["format"] = "date-time";
            f["default"] = Json::nullValue;
        } else if (type == "date") {
            f["jsonType"] = "string";
            f["format"] = "date";
            f["default"] = Json::nullValue;
        } else if (type == "USER-DEFINED") {
            auto enum_vals = enumGetter(full_type);
            Json::Value ev(Json::arrayValue);
            for (auto& v : enum_vals)
                ev.append(v);
            f["jsonType"] = "string";
            f["enum"] = ev;
            f["default"] = enum_vals.empty() ? "" : enum_vals[0];
        } else {
            f["jsonType"] = "string";
            f["default"] = nullable ? Json::nullValue : Json::Value("");
        }

        if (!row["character_maximum_length"].isNull())
            f["maxLength"] = row["character_maximum_length"].as<int>();

        if (!nullable && row["column_default"].isNull())
            required.append(name);

        fields[name] = f;
    }

    result["fields"] = fields;
    result["required"] = required;
    return result;
}

/* ============================================================
 * CORO ROW PROCESSING (FOR REFLECTION)
 * ============================================================ */

inline drogon::Task<Json::Value> reflectOnRowsCoro(const drogon::orm::Result& t_sql_rows,
    std::function<drogon::Task<std::vector<std::string>>(const std::string&)> t_enum_getter, std::string_view t_schema_name,
    std::string_view t_table_name) {
    Json::Value result(Json::objectValue);
    Json::Value fields(Json::objectValue);
    Json::Value required(Json::arrayValue);

    result["schema"] = std::string(Schema_Name);
    result["table"] = std::string(Table_Name);
    result["type"] = "object";

    for (const auto& row : t_sql_rows) {
        std::string name = row["column_name"].as<std::string>();

        std::string type = row["data_type"].as<std::string>();
        std::string full_type = row["full_type"].as<std::string>();
        bool nullable = row["is_nullable"].as<std::string>() == "YES";

        Json::Value f(Json::objectValue);
        f["pgType"] = full_type;
        f["nullable"] = nullable;
        f["comment"] = "Postgres column `" + name + "` (" + full_type + ")";

        if (isNumericType(type)) {
            f["jsonType"] = "number";
            f["default"] = 0;
        } else if (type == "boolean") {
            f["jsonType"] = "boolean";
            f["default"] = false;
        } else if (type == "uuid") {
            f["jsonType"] = "string";
            f["format"] = "uuid";
            f["default"] = "";
        } else if (type == "json" || type == "jsonb") {
            f["jsonType"] = "object";
            f["default"] = Json::objectValue;
        } else if (type == "ARRAY") {
            f["jsonType"] = "array";
            f["elementType"] = full_type.substr(0, full_type.size() - 2);
            f["default"] = Json::arrayValue;
        } else if (isTimestamp(type)) {
            f["jsonType"] = "string";
            f["format"] = "date-time";
            f["default"] = Json::nullValue;
        } else if (type == "date") {
            f["jsonType"] = "string";
            f["format"] = "date";
            f["default"] = Json::nullValue;
        } else if (type == "USER-DEFINED") {
            auto enum_vals = co_await enumGetter(full_type);
            Json::Value ev(Json::arrayValue);
            for (auto& v : enum_vals)
                ev.append(v);
            f["jsonType"] = "string";
            f["enum"] = ev;
            f["default"] = enum_vals.empty() ? "" : enum_vals[0];
        } else {
            f["jsonType"] = "string";
            f["default"] = nullable ? Json::nullValue : Json::Value("");
        }

        if (!row["character_maximum_length"].isNull())
            f["maxLength"] = row["character_maximum_length"].as<int>();

        if (!nullable && row["column_default"].isNull())
            required.append(name);

        fields[name] = f;
    }

    result["fields"] = fields;
    result["required"] = required;
    co_return result;
}

/* ============================================================
 * ROW PROCESSING FOR INSERTION TEMPLATE (DATA FORMAT)
 * Directly builds data template from SQL rows
 * ============================================================ */

inline drogon::Task<Json::Value> buildInsertionTemplateFromRows(const drogon::orm::Result& t_sql_rows,
    std::function<drogon::Task<std::vector<std::string>>(const std::string&)> t_enum_getter, std::string_view t_schema_name,
    const std::vector<std::string>& t_ignored_fields) {
    Json::Value template_data(Json::objectValue);

    for (const auto& row : t_sql_rows) {
        std::string name = row["column_name"].as<std::string>();

        // Skip ignored fields
        if (std::find(Ignored_Fields.begin(), Ignored_Fields.end(), name) != Ignored_Fields.end()) {
            continue;
        }

        std::string type = row["data_type"].as<std::string>();
        std::string full_type = row["full_type"].as<std::string>();
        bool nullable = row["is_nullable"].as<std::string>() == "YES";

        // Determine default value based on type
        if (isNumericType(type)) {
            template_data[name] = 0;
        } else if (type == "boolean") {
            template_data[name] = false;
        } else if (type == "uuid") {
            template_data[name] = "";
        } else if (type == "json" || type == "jsonb") {
            template_data[name] = Json::objectValue;
        } else if (type == "ARRAY") {
            template_data[name] = Json::arrayValue;
        } else if (isTimestamp(type) || type == "date") {
            template_data[name] = Json::nullValue;
        } else if (type == "USER-DEFINED") {
            auto enum_vals = co_await enumGetter(full_type);
            template_data[name] = enum_vals.empty() ? "" : enum_vals[0];
        } else {
            template_data[name] = nullable ? Json::nullValue : Json::Value("");
        }
    }

    co_return template_data;
}

/* ============================================================
 * SCHEMA REFLECTION (SYNC)
 * ============================================================ */

inline Json::Value reflectOnTable(
    const drogon::orm::DbClientPtr& tsp_db_client, std::string_view t_schema_name, std::string_view t_table_name) {
    try {
        auto rows = p_Db_Client->execSqlSync(reflecting_on_table_sql, std::string(Schema_Name), std::string(Table_Name));

        return reflectOnRowsSync(
            rows, [&](const std::string& enumName) { return getEnumValues(p_Db_Client, Schema_Name, enumName); }, Schema_Name, Table_Name);
    } catch (const drogon::orm::DrogonDbException& e) {
        REFLECTION_ERROR_LOG("Failed to reflect on table {}.{}: ", Schema_Name, Table_Name, e.base().what());
        throw;
    }
}

/* ============================================================
 * SCHEMA REFLECTION (CORO)
 * ============================================================ */

inline drogon::Task<Json::Value> reflectOnTableCoro(
    const drogon::orm::DbClientPtr& tsp_db_client, std::string_view t_schema_name, std::string_view t_table_name) {
    try {
        auto rows = co_await p_Db_Client->execSqlCoro(reflecting_on_table_sql, std::string(Schema_Name), std::string(Table_Name));

        auto result = co_await reflectOnRowsCoro(
            rows,
            [&](const std::string& enumName) -> drogon::Task<std::vector<std::string>> {
                return getEnumValuesCoro(p_Db_Client, Schema_Name, enumName);
            },
            Schema_Name, Table_Name);
        co_return result;
    } catch (const drogon::orm::DrogonDbException& e) {
        REFLECTION_ERROR_LOG("Failed to reflect on table {}.{}: ", Schema_Name, Table_Name, e.base().what());
        throw;
    }
}

/* ============================================================
 * INSERTION TEMPLATE (DIRECT FROM DB)
 * Returns: { "field_name": default_value, ... }
 * ============================================================ */

inline drogon::Task<Json::Value> getInsertionTemplateCoro(const drogon::orm::DbClientPtr& tsp_db_client, std::string_view t_schema_name,
    std::string_view t_table_name, const std::vector<std::string>& t_ignored_fields = {"id", "created_at", "updated_at"}) {
    try {
        // Query DB directly for template generation
        auto rows = co_await p_Db_Client->execSqlCoro(reflecting_on_table_sql, std::string(Schema_Name), std::string(Table_Name));

        // Build data template directly from rows
        auto template_data = co_await buildInsertionTemplateFromRows(
            rows,
            [&](const std::string& enumName) -> drogon::Task<std::vector<std::string>> {
                return getEnumValuesCoro(p_Db_Client, Schema_Name, enumName);
            },
            Schema_Name, Ignored_Fields);

        co_return template_data;
    } catch (const drogon::orm::DrogonDbException& e) {
        REFLECTION_ERROR_LOG("Failed to build insertion template for {}.{}: ", Schema_Name, Table_Name, e.base().what());
        throw;
    }
}

/* ============================================================
 * LEGACY ALIAS (DEPRECATED - Use getInsertionTemplateCoro)
 * ============================================================ */

inline drogon::Task<Json::Value> getJsonInsertionFormCoro(const drogon::orm::DbClientPtr& tsp_db_client, std::string_view t_schema_name,
    std::string_view t_table_name, const std::vector<std::string>& t_ignored_fields = {"id", "created_at", "updated_at"}) {
    return getInsertionTemplateCoro(p_Db_Client, Schema_Name, Table_Name, Ignored_Fields);
}

} // namespace sgrn::postgres::reflection
