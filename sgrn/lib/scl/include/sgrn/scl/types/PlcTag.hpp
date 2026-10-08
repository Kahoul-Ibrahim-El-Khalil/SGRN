#pragma once
// PlcTag — one TIA-style tag-table row: name + type + physical address.
//
// Tags are discrete addressed objects (%I0.0 inputs, %Q0.0 outputs, %M/%MW/%MD
// merkers, DB1.DBX0.0 aliases, …) living in their own memory areas — a
// different beast from DataBlocks, which are whole memory areas. A tag may be
// any scalar S7 type or a schema UDT (size/layout resolve from the UDT).
#include <sgrn/scl/S7K.hpp>
#include <sgrn/scl/types/DataType.hpp>

#include <cstdint>
#include <string>

namespace sgrn::scl
{

// ---------------------------------------------------------------------------
// PLC Address Shorthand  (I0.0, IB3, IW6, ID0, QB4, MW10, MD8, T5, C12, Z4,
// DB1.DBX0.0, DB2.DBD8, …; %-prefixed TIA form accepted and normalized)
// ---------------------------------------------------------------------------

struct PlcAddress {
    int area{S7AreaMK}; ///< S7AreaPE, S7AreaPA, S7AreaMK, S7AreaTM, S7AreaCT, S7AreaDB
    uint16_t db_number{0};
    int byte_offset{0};
    int bit_index{-1};      ///< -1 = no bit; 0-7 = specific bit position
    int word_len{S7WLByte}; ///< S7WLBit, S7WLByte, S7WLWord, S7WLDWord, S7WLTimer, S7WLCounter
    int byte_count{1};      ///< bytes to transfer (1, 2, 4, 8)
    std::string label;      ///< normalised display label
};

/**
 * @brief A single symbolic tag: one row of a (TIA-style) tag table.
 *
 * Bridges symbolic names and physical S7 addresses. The table_name groups
 * rows the way TIA tag-table folders do; the PLC namespace itself is flat
 * (names must be unique PLC-wide).
 */
struct PlcTag {
    std::string name;              ///< Tag name, e.g. "StartButton"
    std::string table_name;        ///< Source table, e.g. TIA <Tagtable name> or SCL #TAG_TABLE name
    std::string type_str;          ///< Original type spelling, e.g. "Bool", "MotorState"
    std::string remark;            ///< Optional remark
    PlcAddress addr;               ///< Resolved physical PLC address
    DataType type{DataType::Bool}; ///< Resolved S7 type (Struct for UDT refs — see udt_name)
    std::string udt_name;          ///< Non-empty when type_str names a schema UDT
};

} // namespace sgrn::scl
