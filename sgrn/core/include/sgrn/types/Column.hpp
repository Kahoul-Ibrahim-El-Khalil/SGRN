
#pragma once

// include/sgrn/types/Column.hpp
//
// Definition of the Column structure used to describe database result
// metadata and control JSON transformation behavior for Drogon-based APIs.
//
#include <string_view>

namespace sgrn
{

/**
 * @brief Represents metadata for a database column.
 *
 * Each Column instance defines the column's name and its corresponding
 * logical or database type. This structure is used by query utilities
 * (e.g. `transformQueryResultToJsonArray`) to determine how to extract
 * and serialize data from Drogon ORM query results.
 */
struct Column {
    /**
     * @brief Enumerates supported data types for columns.
     *
     * These map directly to both C++ primitive types and PostgreSQL
     * column types, enabling type-safe conversion from query results
     * to JSON representations.
     */
    enum class Type {
        // Generic types
        INT,    ///< General integer (platform-dependent size)
        BOOL,   ///< Boolean value
        DOUBLE, ///< Double-precision floating point
        STRING, ///< Text or character data

        // PostgreSQL-specific numeric types
        INT32,   ///< 32-bit integer (PostgreSQL INT)
        INT64,   ///< 64-bit integer (PostgreSQL BIGINT)
        FLOAT,   ///< 32-bit floating point
        NUMERIC, ///< Arbitrary-precision numeric

        // Temporal types
        DATE,        ///< Calendar date (YYYY-MM-DD)
        TIME,        ///< Time of day (HH:MM:SS)
        TIMESTAMP,   ///< Date and time without timezone
        TIMESTAMPTZ, ///< Timestamp with timezone

        // JSON and special types
        JSON,  ///< JSON textual representation
        JSONB, ///< JSON binary representation
        UUID,  ///< Universally Unique Identifier
        BYTEA, ///< Binary data (byte array)
        INET,  ///< IP address (IPv4 or IPv6)
        ARRAY, ///< SQL array type

        // Fallback
        NULL_TYPE ///< Explicit null or unsupported type
    };

    /**
     * @brief Name of the column as it appears in the query result.
     *
     * Used as both the key for accessing ORM values and the field name
     * in the generated JSON object.
     */
    std::string_view name;

    /**
     * @brief Logical or database type of the column.
     *
     * Determines the C++ type used for extraction and JSON serialization.
     */
    Type type;
};

} // namespace sgrn
