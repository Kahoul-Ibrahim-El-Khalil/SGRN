/*
 * Copyright (C) 2026 Kahoul Ibrahim El-Khalil
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later
 */

/**
 * @file  sgrn/codecs/opcua_codec_table.hpp
 * @brief Declarative OPC UA <-> S7 type-dispatch table.
 *
 * This is the bridge between the freestanding s7codec layer and the OPC UA
 * protocol adapter. Each row in `kCodecTable` covers one S7 scalar type and
 * carries:
 *   - Static metadata (name, storage width, UA type index).
 *   - `from_ua` -- extract a `DecodedValue` from a raw UA scalar pointer.
 *   - `to_ua`   -- materialise a `UA_Variant` from a `DecodedValue`.
 *
 * Adding a genuinely new S7 type is exactly one row here; all four OPC UA
 * adapter dispatch sites (write scalar, write array element, read scalar,
 * read array element) pick it up automatically.
 *
 * @note  Enum types are handled by the callers *before* consulting this table:
 *        UA enums are always `UA_Int32` on the wire, so the adapter injects the
 *        enum guard once and then delegates to the underlying integer row.
 */

#pragma once

#include <cstdint>
#include <open62541/types.h>
#include <open62541/types_generated.h>
#include <open62541/types_generated_handling.h>
#include <s7codec/codec.hpp>

namespace sgrn::codecs
{

/**
 * @brief Per-type descriptor for the OPC UA <-> S7 codec bridge.
 */
struct CodecEntry {
    s7codec::Type type;
    const char* name;
    int storage_bytes; ///< 0 for variable-length types (String/WString/...)
    int ua_type_idx;   ///< index into UA_TYPES[] for this type's scalar
    bool is_string;
    bool is_temporal;

    /**
     * @brief Extract a neutral DecodedValue from a raw OPC UA scalar buffer.
     *
     * @param ua_type  The UA_DataType of the incoming value (used for type check).
     * @param ua_ptr   Raw pointer to the UA scalar payload.
     * @param out      Receives the decoded value on success.
     * @return true if the UA type matched this entry and decoding succeeded.
     */
    bool (*from_ua)(const UA_DataType* ua_type, const uint8_t* ua_ptr, s7codec::DecodedValue& out) noexcept;

    /**
     * @brief Materialise a UA_Variant from a neutral DecodedValue.
     *
     * @param dv        The decoded S7 value.
     * @param s7_type   The original S7 DataType (needed for sub-type disambiguation).
     * @param out       Receives the initialised UA_Variant (caller must clear).
     * @return true on success.
     */
    bool (*to_ua)(const s7codec::DecodedValue& dv, s7codec::Type s7_type, UA_Variant& out) noexcept;
};

/**
 * @brief The canonical OPC UA <-> S7 codec table -- one row per scalar S7 type.
 *
 * Rows are ordered to match kTypeTable in sgrn::scl (same 30 types).
 * Do NOT rely on index-based access; use `codecEntryFor()`.
 */
extern const CodecEntry kCodecTable[30];

/**
 * @brief O(N) lookup by S7 type (N = 30, compile-time constant).
 * @return Pointer to the matching entry, or nullptr if unmapped.
 */
const CodecEntry* codecEntryFor(s7codec::Type t_type) noexcept;

} // namespace sgrn::codecs
