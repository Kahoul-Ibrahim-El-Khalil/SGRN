#pragma once

#include <string_view>

namespace sgrn::gateway::twin
{

/**
 * @brief Pre-compiled, exhaustive S7 Type Specification JSON.
 * This string is served directly to avoid runtime serialization overhead.
 * Covers legacy S7-300 and modern S7-1200/1500 types.
 *
 * NOTE: For variable-length types, 'n' represents the maximum capacity (MaxLen).
 */
constexpr std::string_view kS7TypeDictionaryJson = R"({
  "BOOL":    {"size_bits": 1,  "endian": "N/A",        "category": "Bit",     "c_type": "bool",           "description": "Single bit in a byte offset (0-7)."},
  "SINT":    {"size_bits": 8,  "endian": "N/A",        "category": "Integer", "c_type": "int8_t",         "description": "Signed 8-bit integer."},
  "USINT":   {"size_bits": 8,  "endian": "N/A",        "category": "Integer", "c_type": "uint8_t",        "description": "Unsigned 8-bit integer."},
  "BYTE":    {"size_bits": 8,  "endian": "N/A",        "category": "Integer", "c_type": "uint8_t",        "description": "Unsigned 8-bit bitstring/integer."},
  "CHAR":    {"size_bits": 8,  "endian": "N/A",        "category": "String",  "c_type": "char",           "description": "Single ASCII character."},
  "WORD":    {"size_bits": 16, "endian": "Big-Endian", "category": "Binary",  "c_type": "uint16_t",       "description": "16-bit bitstring or unsigned integer."},
  "INT":     {"size_bits": 16, "endian": "Big-Endian", "category": "Integer", "c_type": "int16_t",        "description": "Signed 16-bit integer."},
  "UINT":    {"size_bits": 16, "endian": "Big-Endian", "category": "Integer", "c_type": "uint16_t",       "description": "Unsigned 16-bit integer."},
  "DWORD":   {"size_bits": 32, "endian": "Big-Endian", "category": "Binary",  "c_type": "uint32_t",       "description": "32-bit bitstring or unsigned integer."},
  "DINT":    {"size_bits": 32, "endian": "Big-Endian", "category": "Integer", "c_type": "int32_t",        "description": "Signed 32-bit integer."},
  "UDINT":   {"size_bits": 32, "endian": "Big-Endian", "category": "Integer", "c_type": "uint32_t",       "description": "Unsigned 32-bit integer."},
  "LWORD":   {"size_bits": 64, "endian": "Big-Endian", "category": "Binary",  "c_type": "uint64_t",       "description": "64-bit bitstring."},
  "LINT":    {"size_bits": 64, "endian": "Big-Endian", "category": "Integer", "c_type": "int64_t",        "description": "Signed 64-bit integer."},
  "ULINT":   {"size_bits": 64, "endian": "Big-Endian", "category": "Integer", "c_type": "uint64_t",       "description": "Unsigned 64-bit integer."},
  "REAL":    {"size_bits": 32, "endian": "Big-Endian", "category": "Float",   "c_type": "float",          "description": "32-bit IEEE 754 Floating point value."},
  "LREAL":   {"size_bits": 64, "endian": "Big-Endian", "category": "Float",   "c_type": "double",         "description": "64-bit Double precision float."},
  "TIME":    {"size_bits": 32, "endian": "Big-Endian", "category": "Time",    "c_type": "int32_t",        "description": "Signed 32-bit duration in milliseconds."},
  "LTIME":   {"size_bits": 64, "endian": "Big-Endian", "category": "Time",    "c_type": "int64_t",        "description": "Signed 64-bit duration in nanoseconds."},
  "S5TIME":  {"size_bits": 16, "endian": "Big-Endian", "category": "Time",    "c_type": "uint16_t",       "description": "BCD-coded duration (Simatic S5 style)."},
  "DATE":    {"size_bits": 16, "endian": "Big-Endian", "category": "Time",    "c_type": "uint16_t",       "description": "Days since 1990-01-01."},
  "TOD":     {"size_bits": 32, "endian": "Big-Endian", "category": "Time",    "c_type": "uint32_t",       "description": "Time of Day (ms since midnight)."},
  "LTOD":    {"size_bits": 64, "endian": "Big-Endian", "category": "Time",    "c_type": "uint64_t",       "description": "Long Time of Day (nanoseconds since midnight)."},
  "DT":      {"size_bits": 64, "endian": "Big-Endian", "category": "Time",    "c_type": "S7DateTime",     "description": "Date and Time (8-byte BCD)."},
  "LDT":     {"size_bits": 64, "endian": "Big-Endian", "category": "Time",    "c_type": "int64_t",        "description": "Date and Time (nanoseconds since 1970)."},
  "DTL":     {"size_bits": 96, "endian": "Big-Endian", "category": "Time",    "c_type": "struct DTL",     "description": "Date and Time Long (12-byte structure)."},
  "STRING":  {"size_bits": "(n+2)*8", "endian": "N/A", "category": "String",  "c_type": "struct S7String", "description": "Pascal-style string. 'n' is the maximum capacity in characters. Size includes 2-byte header."},
  "WSTRING": {"size_bits": "(n*2+4)*8", "endian": "N/A", "category": "String",  "c_type": "struct S7WString", "description": "16-bit Unicode string. 'n' is the maximum capacity in characters. Size includes 4-byte header."},
  "XSTRING":  {"size_bits": "(n+8)*8", "endian": "Big-Endian", "category": "String",  "c_type": "struct SGRNXString", "description": "High-capacity extended string. 'n' is the maximum capacity in characters. Size includes 8-byte header (32-bit Max/Cur)."},
  "XWSTRING": {"size_bits": "(n*2+8)*8", "endian": "Big-Endian", "category": "String",  "c_type": "struct SGRNXWString", "description": "High-capacity extended 16-bit string. 'n' is the maximum capacity in characters. Size includes 8-byte header (32-bit Max/Cur)."},
  "COUNTER": {"size_bits": 16, "endian": "Big-Endian", "category": "Legacy",  "c_type": "uint16_t",       "description": "S7-300 Hardware Counter (BCD)."},
  "TIMER":   {"size_bits": 16, "endian": "Big-Endian", "category": "Legacy",  "c_type": "uint16_t",       "description": "S7-300 Hardware Timer (BCD)."}
})";

} // namespace sgrn::gateway::twin
