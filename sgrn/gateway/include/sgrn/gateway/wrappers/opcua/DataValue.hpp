#pragma once

#include <open62541/types.h>
#include <open62541/types_generated.h>
#include <open62541/types_generated_handling.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace sgrn::gateway::wrappers::opcua
{

// ── Time helper ─────────────────────────────────────────────────────────────

/// Convert milliseconds since Unix epoch to UA_DateTime
/// (100-nanosecond intervals since 1601-01-01).
inline UA_DateTime millisToUaDateTime(uint64_t t_ms) noexcept {
    return static_cast<UA_DateTime>(t_ms) * UA_DATETIME_MSEC + UA_DATETIME_UNIX_EPOCH;
}

// ── DataValue ────────────────────────────────────────────────────────────────

/// RAII move-only wrapper over UA_DataValue.
/// All factory methods set hasValue = true.
class DataValue {
public:
    DataValue() noexcept {
        UA_DataValue_init(&dv_);
    }

    ~DataValue() noexcept {
        UA_DataValue_clear(&dv_);
    }

    // Move-only
    DataValue(DataValue&& t_other) noexcept
        : dv_(t_other.dv_) {
        UA_DataValue_init(&t_other.dv_);
    }
    DataValue& operator=(DataValue&& t_other) noexcept {
        if (this != &t_other) {
            UA_DataValue_clear(&dv_);
            dv_ = t_other.dv_;
            UA_DataValue_init(&t_other.dv_);
        }
        return *this;
    }
    DataValue(const DataValue&) = delete;
    DataValue& operator=(const DataValue&) = delete;

    // ── Scalar factories ─────────────────────────────────────────────────────

    static DataValue fromBool(bool t_v) {
        DataValue d;
        UA_Boolean b = t_v;
        UA_Variant_setScalarCopy(&d.dv_.value, &b, &UA_TYPES[UA_TYPES_BOOLEAN]);
        d.dv_.hasValue = true;
        return d;
    }

    static DataValue fromInt16(int16_t t_v) {
        DataValue d;
        UA_Int16 val = t_v;
        UA_Variant_setScalarCopy(&d.dv_.value, &val, &UA_TYPES[UA_TYPES_INT16]);
        d.dv_.hasValue = true;
        return d;
    }

    static DataValue fromUInt16(uint16_t t_v) {
        DataValue d;
        UA_UInt16 val = t_v;
        UA_Variant_setScalarCopy(&d.dv_.value, &val, &UA_TYPES[UA_TYPES_UINT16]);
        d.dv_.hasValue = true;
        return d;
    }

    static DataValue fromInt32(int32_t t_v) {
        DataValue d;
        UA_Int32 val = t_v;
        UA_Variant_setScalarCopy(&d.dv_.value, &val, &UA_TYPES[UA_TYPES_INT32]);
        d.dv_.hasValue = true;
        return d;
    }

    static DataValue fromUInt32(uint32_t t_v) {
        DataValue d;
        UA_UInt32 val = t_v;
        UA_Variant_setScalarCopy(&d.dv_.value, &val, &UA_TYPES[UA_TYPES_UINT32]);
        d.dv_.hasValue = true;
        return d;
    }

    static DataValue fromInt64(int64_t t_v) {
        DataValue d;
        UA_Int64 val = t_v;
        UA_Variant_setScalarCopy(&d.dv_.value, &val, &UA_TYPES[UA_TYPES_INT64]);
        d.dv_.hasValue = true;
        return d;
    }

    static DataValue fromUInt64(uint64_t t_v) {
        DataValue d;
        UA_UInt64 val = t_v;
        UA_Variant_setScalarCopy(&d.dv_.value, &val, &UA_TYPES[UA_TYPES_UINT64]);
        d.dv_.hasValue = true;
        return d;
    }

    static DataValue fromFloat(float t_v) {
        DataValue d;
        UA_Float val = t_v;
        UA_Variant_setScalarCopy(&d.dv_.value, &val, &UA_TYPES[UA_TYPES_FLOAT]);
        d.dv_.hasValue = true;
        return d;
    }

    static DataValue fromDouble(double t_v) {
        DataValue d;
        UA_Double val = t_v;
        UA_Variant_setScalarCopy(&d.dv_.value, &val, &UA_TYPES[UA_TYPES_DOUBLE]);
        d.dv_.hasValue = true;
        return d;
    }

    static DataValue fromString(std::string_view t_v) {
        DataValue d;
        UA_String s = UA_STRING_ALLOC(t_v.data());
        UA_Variant_setScalarCopy(&d.dv_.value, &s, &UA_TYPES[UA_TYPES_STRING]);
        UA_String_clear(&s);
        d.dv_.hasValue = true;
        return d;
    }

    // ── Array factories ──────────────────────────────────────────────────────

    static DataValue fromBoolArray(std::span<const bool> t_v) {
        return makeArray<UA_Boolean, bool>(t_v, UA_TYPES_BOOLEAN, [](UA_Boolean& t_dst, bool t_src) { t_dst = t_src; });
    }

    static DataValue fromInt16Array(std::span<const int16_t> t_v) {
        return makeArrayDirect<UA_Int16>(t_v, UA_TYPES_INT16);
    }

    static DataValue fromUInt16Array(std::span<const uint16_t> t_v) {
        return makeArrayDirect<UA_UInt16>(t_v, UA_TYPES_UINT16);
    }

    static DataValue fromInt32Array(std::span<const int32_t> t_v) {
        return makeArrayDirect<UA_Int32>(t_v, UA_TYPES_INT32);
    }

    static DataValue fromUInt32Array(std::span<const uint32_t> t_v) {
        return makeArrayDirect<UA_UInt32>(t_v, UA_TYPES_UINT32);
    }

    static DataValue fromInt64Array(std::span<const int64_t> t_v) {
        return makeArrayDirect<UA_Int64>(t_v, UA_TYPES_INT64);
    }

    static DataValue fromUInt64Array(std::span<const uint64_t> t_v) {
        return makeArrayDirect<UA_UInt64>(t_v, UA_TYPES_UINT64);
    }

    static DataValue fromFloatArray(std::span<const float> t_v) {
        return makeArrayDirect<UA_Float>(t_v, UA_TYPES_FLOAT);
    }

    static DataValue fromDoubleArray(std::span<const double> t_v) {
        return makeArrayDirect<UA_Double>(t_v, UA_TYPES_DOUBLE);
    }

    // ── Timestamp setters (fluent, mutate in place) ──────────────────────────

    DataValue& withSourceTimestamp(uint64_t t_unix_ms) noexcept {
        dv_.hasSourceTimestamp = true;
        dv_.sourceTimestamp = millisToUaDateTime(t_unix_ms);
        return *this;
    }

    DataValue& withServerTimestamp() noexcept {
        dv_.hasServerTimestamp = true;
        dv_.serverTimestamp = UA_DateTime_now();
        return *this;
    }

    /// Take ownership of a stack-allocated UA_DataValue (e.g. from json_to_ua).
    static DataValue adopt(UA_DataValue&& t_raw) {
        DataValue d;
        UA_DataValue_clear(&d.dv_);
        d.dv_ = t_raw;
        UA_DataValue_init(&t_raw);
        return d;
    }

    // ── INTERNAL: escape hatch for the adapter layer only ───────────────────
    const UA_DataValue& get() const noexcept {
        return dv_;
    }
    UA_DataValue& get() noexcept {
        return dv_;
    }

private:
    // Generic array helper for types where UA_* == C++ type (same size/layout)
    template <typename UaT, typename CppT = UaT>
    static DataValue makeArrayDirect(std::span<const CppT> t_v, int t_ua_type_idx) {
        DataValue d;
        const size_t n = t_v.size();
        auto* p_arr = static_cast<UaT*>(UA_Array_new(n, &UA_TYPES[t_ua_type_idx]));
        for (size_t i = 0; i < n; ++i)
            p_arr[i] = static_cast<UaT>(t_v[i]);
        UA_Variant_setArray(&d.dv_.value, p_arr, n, &UA_TYPES[t_ua_type_idx]);
        d.dv_.value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
        d.dv_.value.arrayDimensions[0] = static_cast<UA_UInt32>(n);
        d.dv_.value.arrayDimensionsSize = 1;
        d.dv_.hasValue = true;
        return d;
    }

    // Generic array helper with per-element conversion lambda
    template <typename UaT, typename CppT, typename Conv>
    static DataValue makeArray(std::span<const CppT> t_v, int t_ua_type_idx, Conv t_conv) {
        DataValue d;
        const size_t n = t_v.size();
        auto* p_arr = static_cast<UaT*>(UA_Array_new(n, &UA_TYPES[t_ua_type_idx]));
        for (size_t i = 0; i < n; ++i)
            t_conv(p_arr[i], t_v[i]);
        UA_Variant_setArray(&d.dv_.value, p_arr, n, &UA_TYPES[t_ua_type_idx]);
        d.dv_.value.arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(1, &UA_TYPES[UA_TYPES_UINT32]));
        d.dv_.value.arrayDimensions[0] = static_cast<UA_UInt32>(n);
        d.dv_.value.arrayDimensionsSize = 1;
        d.dv_.hasValue = true;
        return d;
    }

    UA_DataValue dv_{};
};

// ── DataValueView ────────────────────────────────────────────────────────────

/// Non-owning, read-only view over a UA_DataValue*.
/// Used by the client monitored-item trampoline so that callbacks never
/// receive raw UA_* types. Lifetime is tied to the underlying UA_DataValue.
class DataValueView {
public:
    explicit DataValueView(const UA_DataValue* tp_dv) noexcept
        : dv_(tp_dv) {
    }

    bool hasValue() const noexcept {
        return dv_ && dv_->hasValue;
    }
    bool hasSourceTimestamp() const noexcept {
        return dv_ && dv_->hasSourceTimestamp;
    }
    bool hasServerTimestamp() const noexcept {
        return dv_ && dv_->hasServerTimestamp;
    }

    uint64_t sourceTimestampMs() const noexcept {
        if (!dv_ || !dv_->hasSourceTimestamp)
            return 0;
        return static_cast<uint64_t>((dv_->sourceTimestamp - UA_DATETIME_UNIX_EPOCH) / UA_DATETIME_MSEC);
    }

    /// Access the underlying variant for type inspection.
    /// INTERNAL — prefer typed accessors below in production code.
    const UA_Variant* variant() const noexcept {
        return dv_ ? &dv_->value : nullptr;
    }

    // ── INTERNAL: raw access for the adapter layer only ─────────────────────
    const UA_DataValue* t_raw() const noexcept {
        return dv_;
    }

private:
    const UA_DataValue* dv_{nullptr};
};

} // namespace sgrn::gateway::wrappers::opcua
