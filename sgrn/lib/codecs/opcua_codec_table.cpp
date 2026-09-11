/*
 * Copyright (C) 2026 Kahoul Ibrahim El-Khalil
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

/**
 * @file  sgrn/codecs/opcua_codec_table.cpp
 * @brief Implementation of the OPC UA <-> S7 codec table.
 *
 * Each row supplies two small adapter functions:
 *   from_ua  -- UA wire bytes  -> s7codec::DecodedValue  (write path)
 *   to_ua    -- DecodedValue   -> UA_Variant              (read path)
 *
 * String and temporal types carry non-trivial adapters; all integer/float
 * types are one-liners.
 *
 * Enum types are NOT rows in this table.  Callers intercept
 * (typeKind == UA_DATATYPEKIND_ENUM) before reaching this table, cast the
 * wire bytes to UA_Int32, and then encode/decode via the underlying integer
 * row that matches the S7 storage width.
 */

#include "opcua_codec_table.hpp"

#include <cstring>
#include <open62541/types_generated.h>
#include <open62541/types_generated_handling.h>
#include <s7codec/codec.hpp>

using s7codec::DecodedValue;
using s7codec::Type;
using s7codec::ValueKind;

namespace sgrn::codecs
{

// ---------------------------------------------------------------------------
// Helper macros for boilerplate integer rows
// ---------------------------------------------------------------------------

// from_ua: check pointer identity against UA_TYPES[IDX], extract typed value
#define FROM_UA_SIGNED(IDX, CType)                                                                                                         \
    [](const UA_DataType* t, const uint8_t* p, DecodedValue& out) noexcept -> bool {                                                       \
        if (t != &UA_TYPES[IDX])                                                                                                           \
            return false;                                                                                                                  \
        out = DecodedValue::makeSigned(static_cast<int64_t>(*reinterpret_cast<const CType*>(p)));                                          \
        return true;                                                                                                                       \
    }

#define FROM_UA_UNSIGNED(IDX, CType)                                                                                                       \
    [](const UA_DataType* t, const uint8_t* p, DecodedValue& out) noexcept -> bool {                                                       \
        if (t != &UA_TYPES[IDX])                                                                                                           \
            return false;                                                                                                                  \
        out = DecodedValue::makeUnsigned(static_cast<uint64_t>(*reinterpret_cast<const CType*>(p)));                                       \
        return true;                                                                                                                       \
    }

// to_ua: setScalarCopy of a cast of the DecodedValue into the right UA type
#define TO_UA_SIGNED(IDX, CType)                                                                                                           \
    [](const DecodedValue& dv, Type /*s7*/, UA_Variant& out) noexcept -> bool {                                                            \
        CType v = static_cast<CType>(dv.asInt64());                                                                                        \
        return UA_Variant_setScalarCopy(&out, &v, &UA_TYPES[IDX]) == UA_STATUSCODE_GOOD;                                                   \
    }

#define TO_UA_UNSIGNED(IDX, CType)                                                                                                         \
    [](const DecodedValue& dv, Type /*s7*/, UA_Variant& out) noexcept -> bool {                                                            \
        CType v = static_cast<CType>(dv.kind() == ValueKind::UnsignedInt ? dv.u() : static_cast<uint64_t>(dv.asInt64()));                  \
        return UA_Variant_setScalarCopy(&out, &v, &UA_TYPES[IDX]) == UA_STATUSCODE_GOOD;                                                   \
    }

// ---------------------------------------------------------------------------
// Non-trivial adapters
// ---------------------------------------------------------------------------

static bool fromUaBool(const UA_DataType* t_ua_type, const uint8_t* tp_bytes, DecodedValue& t_decoded_value_out) noexcept {
    if (t_ua_type != &UA_TYPES[UA_TYPES_BOOLEAN])
        return false;
    t_decoded_value_out = DecodedValue::makeBool(*reinterpret_cast<const UA_Boolean*>(tp_bytes) != 0);
    return true;
}
static bool toUaBool(const DecodedValue& dv, Type /*s7*/, UA_Variant& out) noexcept {
    UA_Boolean v = (dv.kind() == ValueKind::Bool) ? dv.b() : (dv.asInt64() != 0);
    return UA_Variant_setScalarCopy(&out, &v, &UA_TYPES[UA_TYPES_BOOLEAN]) == UA_STATUSCODE_GOOD;
}

static bool fromUaFloat(const UA_DataType* t, const uint8_t* p, DecodedValue& out) noexcept {
    if (t != &UA_TYPES[UA_TYPES_FLOAT])
        return false;
    out = DecodedValue::makeFloat(*reinterpret_cast<const UA_Float*>(p));
    return true;
}
static bool toUaFloat(const DecodedValue& dv, Type /*s7*/, UA_Variant& out) noexcept {
    UA_Float v = (dv.kind() == ValueKind::Float) ? dv.f() : static_cast<UA_Float>(dv.asDouble());
    return UA_Variant_setScalarCopy(&out, &v, &UA_TYPES[UA_TYPES_FLOAT]) == UA_STATUSCODE_GOOD;
}

static bool fromUaDouble(const UA_DataType* t, const uint8_t* p, DecodedValue& out) noexcept {
    if (t != &UA_TYPES[UA_TYPES_DOUBLE])
        return false;
    out = DecodedValue::makeDouble(*reinterpret_cast<const UA_Double*>(p));
    return true;
}
static bool toUaDouble(const DecodedValue& dv, Type s7_type, UA_Variant& out) noexcept {
    UA_Double v = 0.0;
    if (s7_type == Type::Time) {
        // TIME is stored as Int32 milliseconds; expose as Double for OPC UA
        v = static_cast<UA_Double>(static_cast<int32_t>(dv.asInt64()));
    } else {
        v = (dv.kind() == ValueKind::Double) ? dv.d() : (dv.kind() == ValueKind::Float) ? static_cast<double>(dv.f()) : dv.asDouble();
    }
    return UA_Variant_setScalarCopy(&out, &v, &UA_TYPES[UA_TYPES_DOUBLE]) == UA_STATUSCODE_GOOD;
}

// String types: from_ua extracts the UA_String payload; to_ua uses the
// DecodedValue::String path (s7codec already decoded the bytes to std::string).
static bool fromUaString(const UA_DataType* t, const uint8_t* p, DecodedValue& out) noexcept {
    if (t != &UA_TYPES[UA_TYPES_STRING])
        return false;
    const auto* s = reinterpret_cast<const UA_String*>(p);
    if (!s->data) {
        out = DecodedValue::makeString(std::string{});
    } else {
        out = DecodedValue::makeString(std::string(reinterpret_cast<const char*>(s->data), s->length));
    }
    return true;
}
static bool toUaString(const DecodedValue& dv, Type /*s7*/, UA_Variant& out) noexcept {
    if (dv.kind() != ValueKind::String)
        return false;
    UA_String s = UA_STRING_ALLOC(dv.s().c_str());
    bool ok = UA_Variant_setScalarCopy(&out, &s, &UA_TYPES[UA_TYPES_STRING]) == UA_STATUSCODE_GOOD;
    UA_String_clear(&s);
    return ok;
}

// DateTime / DTL: from_ua reads raw UA_DateTime (100ns ticks since 1601);
// to_ua reconstructs from a DecodedValue::String written by s7codec.
// The actual encode/decode logic remains in write_handler.cpp and
// memory_to_ua.cpp because it needs additional node-context data
// (timezone, range checks). These adapters are provided for table
// completeness; dispatch sites that need the full context call the
// dedicated helpers directly and skip the table for these types.
static bool fromUaDateTime(const UA_DataType* t, const uint8_t* p, DecodedValue& out) noexcept {
    if (t != &UA_TYPES[UA_TYPES_DATETIME])
        return false;
    // Pass raw 100ns ticks as a signed int — callers with context
    // (encodeDateTimeOpcUaToS7) convert further.
    out = DecodedValue::makeSigned(static_cast<int64_t>(*reinterpret_cast<const UA_DateTime*>(p)));
    return true;
}
static bool toUaDateTime(const DecodedValue& dv, Type /*s7*/, UA_Variant& out) noexcept {
    // Called by setScalarFromDecoded path only when it can't handle DTL/DT
    // inline; returns false to let the caller fall through to its own logic.
    (void)dv;
    (void)out;
    return false; // sentinel: caller handles temporals directly
}

// ---------------------------------------------------------------------------
// The table — must match the 30 types in sgrn::scl::kTypeTable
// ---------------------------------------------------------------------------

const CodecEntry kCodecTable[32] = {
    // type                  name        bytes  ua_idx            str    temporal  from_ua to_ua
    {Type::Bool, "BOOL", 1, UA_TYPES_BOOLEAN, false, false, fromUaBool, toUaBool},

    {Type::SInt, "SINT", 1, UA_TYPES_SBYTE, false, false, FROM_UA_SIGNED(UA_TYPES_SBYTE, UA_SByte), TO_UA_SIGNED(UA_TYPES_SBYTE, UA_SByte)},

    {Type::USInt, "USINT", 1, UA_TYPES_BYTE, false, false, FROM_UA_UNSIGNED(UA_TYPES_BYTE, UA_Byte),
        TO_UA_UNSIGNED(UA_TYPES_BYTE, UA_Byte)},

    {Type::Byte, "BYTE", 1, UA_TYPES_BYTE, false, false, FROM_UA_UNSIGNED(UA_TYPES_BYTE, UA_Byte), TO_UA_UNSIGNED(UA_TYPES_BYTE, UA_Byte)},

    {Type::Char, "CHAR", 1, UA_TYPES_BYTE, false, false, FROM_UA_UNSIGNED(UA_TYPES_BYTE, UA_Byte), TO_UA_UNSIGNED(UA_TYPES_BYTE, UA_Byte)},

    {Type::Int, "INT", 2, UA_TYPES_INT16, false, false, FROM_UA_SIGNED(UA_TYPES_INT16, UA_Int16), TO_UA_SIGNED(UA_TYPES_INT16, UA_Int16)},

    {Type::UInt, "UINT", 2, UA_TYPES_UINT16, false, false, FROM_UA_UNSIGNED(UA_TYPES_UINT16, UA_UInt16),
        TO_UA_UNSIGNED(UA_TYPES_UINT16, UA_UInt16)},

    {Type::Word, "WORD", 2, UA_TYPES_UINT16, false, false, FROM_UA_UNSIGNED(UA_TYPES_UINT16, UA_UInt16),
        TO_UA_UNSIGNED(UA_TYPES_UINT16, UA_UInt16)},

    {Type::DInt, "DINT", 4, UA_TYPES_INT32, false, false, FROM_UA_SIGNED(UA_TYPES_INT32, UA_Int32), TO_UA_SIGNED(UA_TYPES_INT32, UA_Int32)},

    {Type::UDInt, "UDINT", 4, UA_TYPES_UINT32, false, false, FROM_UA_UNSIGNED(UA_TYPES_UINT32, UA_UInt32),
        TO_UA_UNSIGNED(UA_TYPES_UINT32, UA_UInt32)},

    {Type::DWord, "DWORD", 4, UA_TYPES_UINT32, false, false, FROM_UA_UNSIGNED(UA_TYPES_UINT32, UA_UInt32),
        TO_UA_UNSIGNED(UA_TYPES_UINT32, UA_UInt32)},

    {Type::Real, "REAL", 4, UA_TYPES_FLOAT, false, false, fromUaFloat, toUaFloat},

    {Type::LInt, "LINT", 8, UA_TYPES_INT64, false, false, FROM_UA_SIGNED(UA_TYPES_INT64, UA_Int64), TO_UA_SIGNED(UA_TYPES_INT64, UA_Int64)},

    {Type::ULInt, "ULINT", 8, UA_TYPES_UINT64, false, false, FROM_UA_UNSIGNED(UA_TYPES_UINT64, UA_UInt64),
        TO_UA_UNSIGNED(UA_TYPES_UINT64, UA_UInt64)},

    {Type::LWord, "LWORD", 8, UA_TYPES_UINT64, false, false, FROM_UA_UNSIGNED(UA_TYPES_UINT64, UA_UInt64),
        TO_UA_UNSIGNED(UA_TYPES_UINT64, UA_UInt64)},

    {Type::LReal, "LREAL", 8, UA_TYPES_DOUBLE, false, false, fromUaDouble, toUaDouble},

    {Type::WChar, "WCHAR", 2, UA_TYPES_UINT16, false, false, FROM_UA_UNSIGNED(UA_TYPES_UINT16, UA_UInt16),
        TO_UA_UNSIGNED(UA_TYPES_UINT16, UA_UInt16)},

    {Type::String, "STRING", 0, UA_TYPES_STRING, true, false, fromUaString, toUaString},

    {Type::WString, "WSTRING", 0, UA_TYPES_STRING, true, false, fromUaString, toUaString},

    {Type::XString, "XSTRING", 0, UA_TYPES_STRING, true, false, fromUaString, toUaString},

    {Type::XWString, "XWSTRING", 0, UA_TYPES_STRING, true, false, fromUaString, toUaString},

    {Type::Time, "TIME", 4, UA_TYPES_DOUBLE, false, true, fromUaDouble, toUaDouble},

    {Type::LTime, "LTIME", 8, UA_TYPES_INT64, false, true, FROM_UA_SIGNED(UA_TYPES_INT64, UA_Int64),
        TO_UA_SIGNED(UA_TYPES_INT64, UA_Int64)},

    {Type::Date, "DATE", 2, UA_TYPES_UINT16, false, true, FROM_UA_UNSIGNED(UA_TYPES_UINT16, UA_UInt16),
        TO_UA_UNSIGNED(UA_TYPES_UINT16, UA_UInt16)},

    {Type::TimeOfDay, "TOD", 4, UA_TYPES_STRING, true, true, fromUaString, toUaString},

    {Type::LTimeOfDay, "LTOD", 8, UA_TYPES_UINT64, false, true, FROM_UA_UNSIGNED(UA_TYPES_UINT64, UA_UInt64),
        TO_UA_UNSIGNED(UA_TYPES_UINT64, UA_UInt64)},

    {Type::DateTime, "DT", 8, UA_TYPES_DATETIME, false, true, fromUaDateTime, toUaDateTime},

    {Type::DTL, "DTL", 12, UA_TYPES_DATETIME, false, true, fromUaDateTime, toUaDateTime},

    {Type::LDT, "LDT", 8, UA_TYPES_DATETIME, false, true, fromUaDateTime, toUaDateTime},

    {Type::LDTL, "LDTL", 8, UA_TYPES_DATETIME, false, true, fromUaDateTime, toUaDateTime},

    {Type::Timer, "TIMER", 2, UA_TYPES_UINT16, false, true, FROM_UA_UNSIGNED(UA_TYPES_UINT16, UA_UInt16),
        TO_UA_UNSIGNED(UA_TYPES_UINT16, UA_UInt16)},

    {Type::Counter, "COUNTER", 2, UA_TYPES_UINT16, false, false, FROM_UA_UNSIGNED(UA_TYPES_UINT16, UA_UInt16),
        TO_UA_UNSIGNED(UA_TYPES_UINT16, UA_UInt16)},
};

const CodecEntry* codecEntryFor(s7codec::Type t_type) noexcept {
    for (const CodecEntry& e : kCodecTable) {
        if (e.type == t_type)
            return &e;
    }
    return nullptr;
}

} // namespace sgrn::codecs
