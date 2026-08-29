// ─────────────────────────────────────────────────────────────────────────────
// SchemaVM.cpp  –  Schema-driven typed virtual machine implementation
// ─────────────────────────────────────────────────────────────────────────────
#include <sgrn/AngelScriptEngine.hpp>
#include <sgrn/s7shell/DataBlock.hpp>
#include <sgrn/s7shell/SchemaVM.hpp>
#include <sgrn/s7shell/bindings/registration.hpp>
#include <sgrn/s7shell/connection/S7Connection.hpp>
#include <scriptarray/scriptarray.h>

#include <cctype>
#include <cstring>

#include <fmt/color.h>
#include <fmt/format.h>
#include <angelscript.h>

#include <sgrn/scl/schema/PlcSchemaStore.hpp>

namespace sgrn::s7shell::shell
{

using PlcSchemaStore = sgrn::scl::PlcSchemaStore;
namespace scl = ::sgrn::scl;

// ── Global instances ─────────────────────────────────────────────────────────
// UdtArrayMeta is now declared in SchemaVM.hpp alongside SchemaVMRegistry.

SchemaVMRegistry g_schema_registry;
ScriptS7Connection* p_g_active_connection = nullptr;

// ── Generic getters: decode from ScriptDataBlock buffer ──────────────────────

static s7codec::DecodedValue decodeFromLiveMemory(ScriptDataBlock* tp_db, FieldMeta* tp_meta) {
    size_t span = s7codec::typeSpanBytes(tp_meta->s7type, tp_meta->count).value_or(0);
    std::vector<uint8_t> tmp(span, 0);
    tp_db->readFieldFromMemory(tp_meta->abs_offset, tmp.data(), span);
    return s7codec::decodeScalar(tp_meta->s7type, tmp.data(), span, tp_meta->bit_index, tp_meta->count, tp_meta->endian);
}

static void GenericFieldGetter_float(asIScriptGeneric* tp_gen) {
    auto* p_db = static_cast<ScriptDataBlock*>(tp_gen->GetObject());
    auto* p_meta = static_cast<FieldMeta*>(tp_gen->GetAuxiliary());
    tp_gen->SetReturnFloat(static_cast<float>(decodeFromLiveMemory(p_db, p_meta).asDouble()));
}

static void GenericFieldGetter_double(asIScriptGeneric* tp_gen) {
    auto* p_db = static_cast<ScriptDataBlock*>(tp_gen->GetObject());
    auto* p_meta = static_cast<FieldMeta*>(tp_gen->GetAuxiliary());
    tp_gen->SetReturnDouble(decodeFromLiveMemory(p_db, p_meta).asDouble());
}

static void GenericFieldGetter_int(asIScriptGeneric* tp_gen) {
    auto* p_db = static_cast<ScriptDataBlock*>(tp_gen->GetObject());
    auto* p_meta = static_cast<FieldMeta*>(tp_gen->GetAuxiliary());
    auto val = decodeFromLiveMemory(p_db, p_meta).asInt64();
    auto primitive_sz = s7codec::primitiveSize(p_meta->s7type).value_or(4);
    if (primitive_sz == 1) {
        tp_gen->SetReturnByte(static_cast<asBYTE>(val));
    } else if (primitive_sz == 2) {
        tp_gen->SetReturnWord(static_cast<asWORD>(val));
    } else {
        tp_gen->SetReturnDWord(static_cast<asDWORD>(val));
    }
}

static void GenericFieldGetter_int64(asIScriptGeneric* tp_gen) {
    auto* p_db = static_cast<ScriptDataBlock*>(tp_gen->GetObject());
    auto* p_meta = static_cast<FieldMeta*>(tp_gen->GetAuxiliary());
    tp_gen->SetReturnQWord(static_cast<asQWORD>(decodeFromLiveMemory(p_db, p_meta).asInt64()));
}

static void GenericFieldGetter_bool(asIScriptGeneric* tp_gen) {
    auto* p_db = static_cast<ScriptDataBlock*>(tp_gen->GetObject());
    auto* p_meta = static_cast<FieldMeta*>(tp_gen->GetAuxiliary());
    tp_gen->SetReturnByte(decodeFromLiveMemory(p_db, p_meta).asInt64() != 0 ? 1 : 0);
}

static void GenericFieldGetter_string(asIScriptGeneric* tp_gen) {
    auto* p_db = static_cast<ScriptDataBlock*>(tp_gen->GetObject());
    auto* p_meta = static_cast<FieldMeta*>(tp_gen->GetAuxiliary());
    
    auto dv = decodeFromLiveMemory(p_db, p_meta);
    if (p_meta->s7type == s7codec::Type::Char || p_meta->s7type == s7codec::Type::WChar) {
        char c = static_cast<char>(dv.u());
        new (tp_gen->GetAddressOfReturnLocation()) std::string(1, c);
    } else {
        new (tp_gen->GetAddressOfReturnLocation()) std::string(dv.s());
    }
}

// Struct/UDT field → returns a FieldProxy for chaining
static void GenericFieldGetter_proxy(asIScriptGeneric* tp_gen) {
    auto* p_db = static_cast<ScriptDataBlock*>(tp_gen->GetObject());
    auto* p_meta = static_cast<FieldMeta*>(tp_gen->GetAuxiliary());
    auto* p_proxy = new ScriptFieldProxy(p_db, p_meta->path);
    tp_gen->SetReturnAddress(p_proxy);
}

// ── Generic setters: encode into ScriptDataBlock buffer ──────────────────────
// IMPORTANT: encode into a LOCAL stack buffer, NOT into snapshot_buffer_.
// snapshot_buffer_ is the "last-read-from-PLC" baseline used by push() to
// detect dirty regions. If we encode directly into it, the diff is always zero
// and nothing gets written to the PLC.

static void GenericFieldSetter_float(asIScriptGeneric* tp_gen) {
    auto* p_db = static_cast<ScriptDataBlock*>(tp_gen->GetObject());
    auto* p_meta = static_cast<FieldMeta*>(tp_gen->GetAuxiliary());
    float val = tp_gen->GetArgFloat(0);
    uint8_t tmp[16]{};
    auto dv = s7codec::DecodedValue::makeFloat(val);
    s7codec::encodeScalar(dv, p_meta->s7type, tmp, sizeof(tmp), p_meta->bit_index, p_meta->count, p_meta->endian);
    p_db->writeFieldToMemory(
        p_meta->abs_offset, tmp, static_cast<size_t>(s7codec::typeSpanBytes(p_meta->s7type, p_meta->count).value_or(0)));
}

static void GenericFieldSetter_double(asIScriptGeneric* tp_gen) {
    auto* p_db = static_cast<ScriptDataBlock*>(tp_gen->GetObject());
    auto* p_meta = static_cast<FieldMeta*>(tp_gen->GetAuxiliary());
    double val = tp_gen->GetArgDouble(0);
    uint8_t tmp[16]{};
    auto dv = s7codec::DecodedValue::makeDouble(val);
    s7codec::encodeScalar(dv, p_meta->s7type, tmp, sizeof(tmp), p_meta->bit_index, p_meta->count, p_meta->endian);
    p_db->writeFieldToMemory(
        p_meta->abs_offset, tmp, static_cast<size_t>(s7codec::typeSpanBytes(p_meta->s7type, p_meta->count).value_or(0)));
}

static void GenericFieldSetter_int(asIScriptGeneric* tp_gen) {
    auto* p_db = static_cast<ScriptDataBlock*>(tp_gen->GetObject());
    auto* p_meta = static_cast<FieldMeta*>(tp_gen->GetAuxiliary());

    // All integer setters are now declared with 'int' parameter in AngelScript
    // so that hex literals (0x10) work without explicit narrowing casts.
    // Always read as DWORD; encodeScalar will truncate to the correct width.
    int32_t val = static_cast<int32_t>(tp_gen->GetArgDWord(0));

    uint32_t span = static_cast<uint32_t>(s7codec::typeSpanBytes(p_meta->s7type, p_meta->count).value_or(8));
    std::vector<uint8_t> tmp(span, 0);
    auto dv = s7codec::DecodedValue::makeSigned(static_cast<int64_t>(val));
    s7codec::encodeScalar(dv, p_meta->s7type, tmp.data(), tmp.size(), p_meta->bit_index, p_meta->count, p_meta->endian);
    p_db->writeFieldToMemory(p_meta->abs_offset, tmp.data(), tmp.size());
}

static void GenericFieldSetter_int64(asIScriptGeneric* tp_gen) {
    auto* p_db = static_cast<ScriptDataBlock*>(tp_gen->GetObject());
    auto* p_meta = static_cast<FieldMeta*>(tp_gen->GetAuxiliary());
    int64_t val = static_cast<int64_t>(tp_gen->GetArgQWord(0));
    uint32_t span = static_cast<uint32_t>(s7codec::typeSpanBytes(p_meta->s7type, p_meta->count).value_or(8));
    std::vector<uint8_t> tmp(span, 0);
    auto dv = s7codec::DecodedValue::makeSigned(val);
    s7codec::encodeScalar(dv, p_meta->s7type, tmp.data(), tmp.size(), p_meta->bit_index, p_meta->count, p_meta->endian);
    p_db->writeFieldToMemory(p_meta->abs_offset, tmp.data(), tmp.size());
}

static void GenericFieldSetter_bool(asIScriptGeneric* tp_gen) {
    auto* p_db = static_cast<ScriptDataBlock*>(tp_gen->GetObject());
    auto* p_meta = static_cast<FieldMeta*>(tp_gen->GetAuxiliary());
    bool val = tp_gen->GetArgByte(0) != 0;
    // Read the live memory arena byte (not the stale PLC snapshot) so that
    // consecutive bool writes to different bits of the same byte do not
    // clobber each other. snapshot_buffer_ only reflects the last get().
    uint8_t cur = 0;
    p_db->readFieldFromMemory(p_meta->abs_offset, &cur, 1);
    auto dv = s7codec::DecodedValue::makeBool(val);
    s7codec::encodeScalar(dv, p_meta->s7type, &cur, 1, p_meta->bit_index, p_meta->count, p_meta->endian);
    p_db->writeFieldToMemory(p_meta->abs_offset, &cur, 1);
}

static void GenericFieldSetter_string(asIScriptGeneric* tp_gen) {
    auto* p_db = static_cast<ScriptDataBlock*>(tp_gen->GetObject());
    auto* p_meta = static_cast<FieldMeta*>(tp_gen->GetAuxiliary());
    const std::string& val = *static_cast<const std::string*>(tp_gen->GetArgAddress(0));
    uint32_t span = static_cast<uint32_t>(s7codec::typeSpanBytes(p_meta->s7type, p_meta->string_capacity > 0 ? p_meta->string_capacity : p_meta->count).value_or(0));
    std::vector<uint8_t> tmp(static_cast<size_t>(span), 0);
    
    s7codec::DecodedValue dv;
    if (p_meta->s7type == s7codec::Type::Char || p_meta->s7type == s7codec::Type::WChar) {
        uint8_t char_val = val.empty() ? 0 : static_cast<uint8_t>(val[0]);
        dv = s7codec::DecodedValue::makeUnsigned(char_val);
    } else {
        dv = s7codec::DecodedValue::makeString(val);
    }

    // Bug fix: for STRING[N], count==1 (one scalar string element) but the
    // encode needs the CHARACTER capacity N, not 1. Use string_capacity which
    // was stored as N from the schema parser.
    uint32_t max_chars = p_meta->string_capacity > 0 ? p_meta->string_capacity : p_meta->count;
    auto status = s7codec::encodeScalar(dv, p_meta->s7type, tmp.data(), tmp.size(), p_meta->bit_index, max_chars, p_meta->endian);
    if (!status.has_value()) {
        fmt::print(stderr, "[SchemaVM] string encode failed for '{}': {}\n", p_meta->path, s7codec::toString(status.error()));
    }
    p_db->writeFieldToMemory(p_meta->abs_offset, tmp.data(), tmp.size());
}

// ── S7 type → AS type mapping ────────────────────────────────────────────────

static const char* s7TypeToAS(s7codec::Type t_t) {
    using T = s7codec::Type;
    switch (t_t) {
        case T::Bool: return "bool";
        case T::Byte: return "uint8";
        case T::Word: return "uint16";
        case T::DWord: return "uint";
        case T::LWord: return "uint64";
        case T::SInt: return "int8";
        case T::USInt: return "uint8";
        case T::Int: return "int16";
        case T::UInt: return "uint16";
        case T::DInt: return "int";
        case T::UDInt: return "uint";
        case T::LInt: return "int64";
        case T::ULInt: return "uint64";
        case T::Real: return "float";
        case T::LReal: return "double";
        case T::Char: return "string";
        case T::WChar: return "string";
        case T::Counter: return "uint16";
        case T::Timer: return "uint16";
        case T::String:
        case T::WString:
        case T::XString:
        case T::XWString: return "string";
        case T::Time: return "int";
        case T::LTime: return "int64";
        case T::Date: return "uint16";
        case T::TimeOfDay: return "uint";
        case T::LTimeOfDay: return "uint64";
        case T::DTL:
        case T::DateTime: return nullptr;
        default: return nullptr;
    }
}

// ── Generic UDT / FieldProxy Getters and Setters ─────────────────────────────

static std::string getUdtFieldName(asIScriptGeneric* tp_gen) {
    return *static_cast<std::string*>(tp_gen->GetAuxiliary());
}

static void GenericUDTFieldGetter_proxy(asIScriptGeneric* tp_gen) {
    auto* p_proxy = static_cast<ScriptFieldProxy*>(tp_gen->GetObject());
    std::string field_name = getUdtFieldName(tp_gen);
    auto* p_sub_proxy = p_proxy->index(field_name);
    tp_gen->SetReturnAddress(p_sub_proxy);
}

static void GenericUDTFieldGetter_float(asIScriptGeneric* tp_gen) {
    auto* p_proxy = static_cast<ScriptFieldProxy*>(tp_gen->GetObject());
    std::string field_name = getUdtFieldName(tp_gen);
    auto* p_sub_proxy = p_proxy->index(field_name);
    tp_gen->SetReturnFloat(p_sub_proxy->toFloat());
    p_sub_proxy->release();
}

static void GenericUDTFieldSetter_float(asIScriptGeneric* tp_gen) {
    auto* p_proxy = static_cast<ScriptFieldProxy*>(tp_gen->GetObject());
    std::string field_name = getUdtFieldName(tp_gen);
    float val = tp_gen->GetArgFloat(0);
    auto* p_sub_proxy = p_proxy->index(field_name);
    p_sub_proxy->assignFloat(val);
    p_sub_proxy->release();
}

static void GenericUDTFieldGetter_double(asIScriptGeneric* tp_gen) {
    auto* p_proxy = static_cast<ScriptFieldProxy*>(tp_gen->GetObject());
    std::string field_name = getUdtFieldName(tp_gen);
    auto* p_sub_proxy = p_proxy->index(field_name);
    tp_gen->SetReturnDouble(p_sub_proxy->toDouble());
    p_sub_proxy->release();
}

static void GenericUDTFieldSetter_double(asIScriptGeneric* tp_gen) {
    auto* p_proxy = static_cast<ScriptFieldProxy*>(tp_gen->GetObject());
    std::string field_name = getUdtFieldName(tp_gen);
    double val = tp_gen->GetArgDouble(0);
    auto* p_sub_proxy = p_proxy->index(field_name);
    p_sub_proxy->assignDouble(val);
    p_sub_proxy->release();
}

static void GenericUDTFieldGetter_int8(asIScriptGeneric* tp_gen) {
    auto* p_proxy = static_cast<ScriptFieldProxy*>(tp_gen->GetObject());
    std::string field_name = getUdtFieldName(tp_gen);
    auto* p_sub_proxy = p_proxy->index(field_name);
    tp_gen->SetReturnByte(p_sub_proxy->toUInt8());
    p_sub_proxy->release();
}

static void GenericUDTFieldSetter_int8(asIScriptGeneric* tp_gen) {
    auto* p_proxy = static_cast<ScriptFieldProxy*>(tp_gen->GetObject());
    std::string field_name = getUdtFieldName(tp_gen);
    // Setter signature uses 'int' to accept integer literals/hex without AS narrowing errors.
    // Truncate to uint8 here in C++.
    uint8_t val = static_cast<uint8_t>(tp_gen->GetArgDWord(0));
    auto* p_sub_proxy = p_proxy->index(field_name);
    p_sub_proxy->assignUInt8(val);
    p_sub_proxy->release();
}

static void GenericUDTFieldGetter_int16(asIScriptGeneric* tp_gen) {
    auto* p_proxy = static_cast<ScriptFieldProxy*>(tp_gen->GetObject());
    std::string field_name = getUdtFieldName(tp_gen);
    auto* p_sub_proxy = p_proxy->index(field_name);
    tp_gen->SetReturnWord(p_sub_proxy->toUInt16());
    p_sub_proxy->release();
}

static void GenericUDTFieldSetter_int16(asIScriptGeneric* tp_gen) {
    auto* p_proxy = static_cast<ScriptFieldProxy*>(tp_gen->GetObject());
    std::string field_name = getUdtFieldName(tp_gen);
    // Setter signature uses 'int' to accept integer literals/hex without AS narrowing errors.
    // Truncate to uint16 here in C++.
    uint16_t val = static_cast<uint16_t>(tp_gen->GetArgDWord(0));
    auto* p_sub_proxy = p_proxy->index(field_name);
    p_sub_proxy->assignUInt16(val);
    p_sub_proxy->release();
}

static void GenericUDTFieldGetter_int64(asIScriptGeneric* tp_gen) {
    auto* p_proxy = static_cast<ScriptFieldProxy*>(tp_gen->GetObject());
    std::string field_name = getUdtFieldName(tp_gen);
    auto* p_sub_proxy = p_proxy->index(field_name);
    tp_gen->SetReturnQWord(p_sub_proxy->toUInt64());
    p_sub_proxy->release();
}

static void GenericUDTFieldSetter_int64(asIScriptGeneric* tp_gen) {
    auto* p_proxy = static_cast<ScriptFieldProxy*>(tp_gen->GetObject());
    std::string field_name = getUdtFieldName(tp_gen);
    uint64_t val = tp_gen->GetArgQWord(0);
    auto* p_sub_proxy = p_proxy->index(field_name);
    p_sub_proxy->assignUInt64(val);
    p_sub_proxy->release();
}

static void GenericUDTFieldGetter_int32(asIScriptGeneric* tp_gen) {
    auto* p_proxy = static_cast<ScriptFieldProxy*>(tp_gen->GetObject());
    std::string field_name = getUdtFieldName(tp_gen);
    auto* p_sub_proxy = p_proxy->index(field_name);
    tp_gen->SetReturnDWord(p_sub_proxy->toInt());
    p_sub_proxy->release();
}

static void GenericUDTFieldSetter_int32(asIScriptGeneric* tp_gen) {
    auto* p_proxy = static_cast<ScriptFieldProxy*>(tp_gen->GetObject());
    std::string field_name = getUdtFieldName(tp_gen);
    int32_t val = tp_gen->GetArgDWord(0);
    auto* p_sub_proxy = p_proxy->index(field_name);
    p_sub_proxy->assignInt(val);
    p_sub_proxy->release();
}

static void GenericUDTFieldGetter_bool(asIScriptGeneric* tp_gen) {
    auto* p_proxy = static_cast<ScriptFieldProxy*>(tp_gen->GetObject());
    std::string field_name = getUdtFieldName(tp_gen);
    auto* p_sub_proxy = p_proxy->index(field_name);
    tp_gen->SetReturnByte(p_sub_proxy->toBool() ? 1 : 0);
    p_sub_proxy->release();
}

static void GenericUDTFieldSetter_bool(asIScriptGeneric* tp_gen) {
    auto* p_proxy = static_cast<ScriptFieldProxy*>(tp_gen->GetObject());
    std::string field_name = getUdtFieldName(tp_gen);
    bool val = tp_gen->GetArgByte(0) != 0;
    auto* p_sub_proxy = p_proxy->index(field_name);
    p_sub_proxy->assignBool(val);
    p_sub_proxy->release();
}

static void GenericUDTFieldGetter_string(asIScriptGeneric* tp_gen) {
    auto* p_proxy = static_cast<ScriptFieldProxy*>(tp_gen->GetObject());
    std::string field_name = getUdtFieldName(tp_gen);
    auto* p_sub_proxy = p_proxy->index(field_name);
    auto* p_str = new std::string(p_sub_proxy->toString());
    tp_gen->SetReturnAddress(p_str);
    p_sub_proxy->release();
}

static void GenericUDTFieldSetter_string(asIScriptGeneric* tp_gen) {
    auto* p_proxy = static_cast<ScriptFieldProxy*>(tp_gen->GetObject());
    std::string field_name = getUdtFieldName(tp_gen);
    const std::string& val = *static_cast<const std::string*>(tp_gen->GetArgAddress(0));
    auto* p_sub_proxy = p_proxy->index(field_name);
    p_sub_proxy->assignString(val);
    p_sub_proxy->release();
}

static void GenericUDTFieldGetter_dtl(asIScriptGeneric* tp_gen) {
    auto* p_proxy = static_cast<ScriptFieldProxy*>(tp_gen->GetObject());
    std::string field_name = getUdtFieldName(tp_gen);
    auto* p_sub_proxy = p_proxy->index(field_name);
    auto* p_dtl_obj = new ScriptDtl();
    // toString() → val() returns JSON-quoted (e.g. "\"2024-03-18 ...\"" ).
    // Strip the surrounding quotes so the timestamp_str can be fed directly
    // back to encodeScalar's sscanf without a parse failure.
    std::string raw = p_sub_proxy->toString();
    if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
        raw = raw.substr(1, raw.size() - 2);
    p_dtl_obj->timestamp_str_ = std::move(raw);
    tp_gen->SetReturnAddress(p_dtl_obj);
    p_sub_proxy->release();
}

static void GenericUDTFieldSetter_dtl(asIScriptGeneric* tp_gen) {
    auto* p_proxy = static_cast<ScriptFieldProxy*>(tp_gen->GetObject());
    std::string field_name = getUdtFieldName(tp_gen);
    auto* p_dtl_obj = static_cast<ScriptDtl*>(tp_gen->GetArgObject(0));
    auto* p_sub_proxy = p_proxy->index(field_name);
    p_sub_proxy->assignDtl(p_dtl_obj);
    p_sub_proxy->release();
}
static void GenericFieldGetter_primitive_array(asIScriptGeneric* tp_gen) {
    auto* p_db = static_cast<ScriptDataBlock*>(tp_gen->GetObject());
    auto* p_meta = static_cast<FieldMeta*>(tp_gen->GetAuxiliary());
    asIScriptEngine* p_engine = tp_gen->GetEngine();

    const char* as_type = s7TypeToAS(p_meta->s7type);
    if (!as_type) {
        tp_gen->SetReturnAddress(nullptr);
        return;
    }

    // AngelScript declaration, e.g.:
    // array<bool>
    // array<float>
    // array<int>
    std::string decl = fmt::format("array<{}>", as_type);

    asITypeInfo* array_type = p_engine->GetTypeInfoByDecl(decl.c_str());

    if (!array_type) {
        tp_gen->SetReturnAddress(nullptr);
        return;
    }

    CScriptArray* arr = CScriptArray::Create(array_type, p_meta->count);

    for (int i = 0; i < p_meta->count; ++i) {

        FieldMeta element = *p_meta;

        if (p_meta->s7type == s7codec::Type::Bool) {

            size_t bit = static_cast<size_t>(p_meta->bit_index) + static_cast<size_t>(i);

            element.abs_offset = p_meta->abs_offset + bit / 8;

            element.bit_index = static_cast<uint8_t>(bit % 8);

        } else {

            const size_t element_size = s7codec::typeSpanBytes(p_meta->s7type, 1).value_or(0);

            element.abs_offset = p_meta->abs_offset + static_cast<size_t>(i) * element_size;
        }

        element.count = 1;

        auto value = decodeFromLiveMemory(p_db, &element);

        // Set the array element according to its AngelScript type.
        if (std::strcmp(as_type, "bool") == 0 || std::strcmp(as_type, "BOOL") == 0) {

            bool v = value.asInt64() != 0;
            arr->SetValue(i, &v);

        } else if (std::strcmp(as_type, "float") == 0 || std::strcmp(as_type, "REAL") == 0) {

            float v = static_cast<float>(value.asDouble());

            arr->SetValue(i, &v);

        } else if (std::strcmp(as_type, "double") == 0 || std::strcmp(as_type, "LREAL") == 0) {

            double v = value.asDouble();
            arr->SetValue(i, &v);

        } else {

            int32_t v = static_cast<int32_t>(value.asInt64());

            arr->SetValue(i, &v);
        }
    }

    tp_gen->SetReturnAddress(arr);
}
static void GenericFieldGetter_udt_array(asIScriptGeneric* tp_gen) {
    auto* p_db = static_cast<ScriptDataBlock*>(tp_gen->GetObject());
    auto* p_meta = static_cast<UdtArrayMeta*>(tp_gen->GetAuxiliary());
    asIScriptEngine* p_engine = tp_gen->GetEngine();

    std::string decl = fmt::format("array<{}@>", p_meta->udt_name);
    asITypeInfo* p_array_type = p_engine->GetTypeInfoByDecl(decl.c_str());
    if (!p_array_type) {
        tp_gen->SetReturnAddress(nullptr);
        return;
    }

    CScriptArray* p_arr = CScriptArray::Create(p_array_type, p_meta->count);
    for (int i = 0; i < p_meta->count; ++i) {
        std::string elem_path = p_meta->path + "[" + std::to_string(i) + "]";
        auto* p_elem_proxy = new ScriptFieldProxy(p_db, elem_path);
        p_arr->SetValue(i, &p_elem_proxy);
        p_elem_proxy->release();
    }
    tp_gen->SetReturnAddress(p_arr);
}

static void GenericUDTFieldGetter_udt_array(asIScriptGeneric* tp_gen) {
    auto* p_proxy = static_cast<ScriptFieldProxy*>(tp_gen->GetObject());
    auto* p_meta = static_cast<UdtArrayMeta*>(tp_gen->GetAuxiliary());
    asIScriptEngine* p_engine = tp_gen->GetEngine();

    std::string decl = fmt::format("array<{}@>", p_meta->udt_name);
    asITypeInfo* p_array_type = p_engine->GetTypeInfoByDecl(decl.c_str());
    if (!p_array_type) {
        tp_gen->SetReturnAddress(nullptr);
        return;
    }

    CScriptArray* p_arr = CScriptArray::Create(p_array_type, p_meta->count);
    for (int i = 0; i < p_meta->count; ++i) {
        std::string elem_path = p_proxy->getPath() + "[" + std::to_string(i) + "]";
        auto* p_elem_proxy = new ScriptFieldProxy(p_proxy->getDb(), elem_path);
        p_arr->SetValue(i, &p_elem_proxy);
        p_elem_proxy->release();
    }
    tp_gen->SetReturnAddress(p_arr);
}

struct GenericAccessors {
    asSFuncPtr getter;
    asSFuncPtr setter;
};

static GenericAccessors accessorsForASType(const char* tp_as_type) {
    if (!tp_as_type)
        return {asFUNCTION(GenericFieldGetter_proxy), {0}};
    if (std::strcmp(tp_as_type, "float") == 0)
        return {asFUNCTION(GenericFieldGetter_float), asFUNCTION(GenericFieldSetter_float)};
    if (std::strcmp(tp_as_type, "double") == 0)
        return {asFUNCTION(GenericFieldGetter_double), asFUNCTION(GenericFieldSetter_double)};
    if (std::strcmp(tp_as_type, "bool") == 0)
        return {asFUNCTION(GenericFieldGetter_bool), asFUNCTION(GenericFieldSetter_bool)};
    if (std::strcmp(tp_as_type, "int64") == 0 || std::strcmp(tp_as_type, "uint64") == 0)
        return {asFUNCTION(GenericFieldGetter_int64), asFUNCTION(GenericFieldSetter_int64)};
    if (std::strcmp(tp_as_type, "string") == 0)
        return {asFUNCTION(GenericFieldGetter_string), asFUNCTION(GenericFieldSetter_string)};
    // All remaining numeric types: int/uint/int8/uint8/int16/uint16 all use
    // 32-bit generic get/set. AngelScript's generic calling convention always
    // passes/receives sub-32-bit values as 32-bit DWORDs.
    return {asFUNCTION(GenericFieldGetter_int), asFUNCTION(GenericFieldSetter_int)};
}

// ── Helpers ──────────────────────────────────────────────────────────────────

std::string sanitizeFieldName(const std::string& t_name) {
    std::string out;
    out.reserve(t_name.size());
    for (char c : t_name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_')
            out += c;
        else
            out += '_';
    }
    if (out.empty() || std::isdigit(static_cast<unsigned char>(out[0])))
        out = "_" + out;
    return out;
}
static ScriptFieldProxy* IdentityProxyCast(ScriptFieldProxy* tp_proxy);
// this function register an anonymous type of a struct
static void registerProxyStructType(sgrn::scripting::ScriptHost& t_host, const std::string& t_type_name,
    const std::vector<scl::DbField>& t_fields, SchemaVMRegistry& t_registry) {
    asIScriptEngine* engine = t_host.getEngine();

    if (t_registry.registered_schema_types.count(t_type_name))
        return;

    t_registry.registered_schema_types.insert(t_type_name);

    if (engine->RegisterObjectType(t_type_name.c_str(), 0, asOBJ_REF) < 0)
        return;

    // Inline STRUCTs are represented by ScriptFieldProxy at runtime.
    engine->RegisterObjectBehaviour(t_type_name.c_str(), asBEHAVE_ADDREF, "void f()", asMETHOD(ScriptFieldProxy, addRef), asCALL_THISCALL);

    engine->RegisterObjectBehaviour(
        t_type_name.c_str(), asBEHAVE_RELEASE, "void f()", asMETHOD(ScriptFieldProxy, release), asCALL_THISCALL);

    // STRUCT <-> FieldProxy casts
    engine->RegisterObjectMethod(t_type_name.c_str(), "FieldProxy@ opCast()", asFUNCTION(IdentityProxyCast), asCALL_CDECL_OBJFIRST);

    engine->RegisterObjectMethod(
        "FieldProxy", fmt::format("{}@ opCast()", t_type_name).c_str(), asFUNCTION(IdentityProxyCast), asCALL_CDECL_OBJFIRST);

    // Register the actual members.
    for (const auto& f : t_fields) {
        const std::string safe_name = sanitizeFieldName(f.name);

        // Nested STRUCT / UDT
        if (!f.children.empty() || !f.udt_name.empty()) {
            std::string child_type;

            if (!f.udt_name.empty()) {
                child_type = sanitizeFieldName(f.udt_name);
            } else {
                child_type = t_type_name + "_" + safe_name;

                registerProxyStructType(t_host, child_type, f.children, t_registry);
            }

            auto p_name = std::make_unique<std::string>(f.name);
            std::string* raw_name = p_name.get();
            t_registry.udt_field_names.push_back(std::move(p_name));

            std::string getter_sig = fmt::format("{}@ get_{}() const", child_type, safe_name);

            engine->RegisterObjectMethod(
                t_type_name.c_str(), getter_sig.c_str(), asFUNCTION(GenericUDTFieldGetter_proxy), asCALL_GENERIC, raw_name);

            continue;
        }

        const char* as_field_type = s7TypeToAS(f.type);
        if (!as_field_type)
            continue;

        auto p_name = std::make_unique<std::string>(f.name);
        std::string* raw_name = p_name.get();
        t_registry.udt_field_names.push_back(std::move(p_name));

        asSFuncPtr getter{};
        asSFuncPtr setter{};

        std::string ast = as_field_type;

        if (ast == "float" || ast == "REAL") {
            getter = asFUNCTION(GenericUDTFieldGetter_float);
            setter = asFUNCTION(GenericUDTFieldSetter_float);
        } else if (ast == "double" || ast == "LREAL") {
            getter = asFUNCTION(GenericUDTFieldGetter_double);
            setter = asFUNCTION(GenericUDTFieldSetter_double);
        } else if (ast == "bool" || ast == "BOOL") {
            getter = asFUNCTION(GenericUDTFieldGetter_bool);
            setter = asFUNCTION(GenericUDTFieldSetter_bool);
        } else if (ast == "string") {
            getter = asFUNCTION(GenericUDTFieldGetter_string);
            setter = asFUNCTION(GenericUDTFieldSetter_string);
        } else if (ast == "DTL@") {
            getter = asFUNCTION(GenericUDTFieldGetter_dtl);
            setter = asFUNCTION(GenericUDTFieldSetter_dtl);
        } else if (ast == "int8" || ast == "uint8") {
            getter = asFUNCTION(GenericUDTFieldGetter_int8);
            setter = asFUNCTION(GenericUDTFieldSetter_int8);
        } else if (ast == "int16" || ast == "uint16") {
            getter = asFUNCTION(GenericUDTFieldGetter_int16);
            setter = asFUNCTION(GenericUDTFieldSetter_int16);
        } else if (ast == "int64" || ast == "uint64") {
            getter = asFUNCTION(GenericUDTFieldGetter_int64);
            setter = asFUNCTION(GenericUDTFieldSetter_int64);
        } else {
            getter = asFUNCTION(GenericUDTFieldGetter_int32);
            setter = asFUNCTION(GenericUDTFieldSetter_int32);
        }

        std::string getter_sig = fmt::format("{} get_{}() const", as_field_type, safe_name);

        engine->RegisterObjectMethod(t_type_name.c_str(), getter_sig.c_str(), getter, asCALL_GENERIC, raw_name);

        std::string setter_sig;

        if (ast == "string") {
            setter_sig = fmt::format("void set_{}(const string &in)", safe_name);
        } else if (ast == "int64" || ast == "uint64") {
            setter_sig = fmt::format("void set_{}({} val)", safe_name, as_field_type);
        } else if (ast == "float" || ast == "double" || ast == "bool" || ast == "DTL@") {
            setter_sig = fmt::format("void set_{}({} val)", safe_name, as_field_type);
        } else {
            // Sub-32-bit types (int8, uint8, int16, uint16, int, uint, etc.)
            // Use 'int' as the setter parameter so that integer literals and hex
            // (e.g. 0x10) can be passed without requiring explicit casts in AS.
            // The C++ generic function handles truncation.
            setter_sig = fmt::format("void set_{}(int val)", safe_name);
        }

        engine->RegisterObjectMethod(t_type_name.c_str(), setter_sig.c_str(), setter, asCALL_GENERIC, raw_name);
    }
}
// ── Field property registration ──────────────────────────────────────────────

static void registerFieldProperties(sgrn::scripting::ScriptHost& t_host, const std::string& t_as_type_name,
    const std::vector<scl::DbField>& t_fields, const std::string& t_path_prefix, SchemaVMRegistry& t_registry) {
    for (const auto& f : t_fields) {
        std::string field_path = t_path_prefix.empty() ? f.name : t_path_prefix + "." + f.name;

        std::string safe_name = sanitizeFieldName(f.name);

        bool is_string = f.type == scl::DataType::String || f.type == scl::DataType::WString || f.type == scl::DataType::XString ||
                         f.type == scl::DataType::XWString;

        // ================================================================
        // ARRAY
        // ================================================================
        if (f.count > 1 && !is_string) {

            if (!f.udt_name.empty()) {
                // --------------------------------------------------------
                // UDT array
                // --------------------------------------------------------
                auto p_meta = std::make_unique<UdtArrayMeta>();

                p_meta->path = field_path;
                p_meta->udt_name = sanitizeFieldName(f.udt_name);
                p_meta->count = f.count;

                UdtArrayMeta* raw = p_meta.get();

                t_registry.udt_array_metas.push_back(std::move(p_meta));

                std::string decl = fmt::format("array<{}@>@", sanitizeFieldName(f.udt_name));

                std::string getter_sig = fmt::format("{} get_{}() const", decl, safe_name);

                t_host.getEngine()->RegisterObjectMethod(
                    t_as_type_name.c_str(), getter_sig.c_str(), asFUNCTION(GenericFieldGetter_udt_array), asCALL_GENERIC, raw);
            } else {
                // ------------------------------------------------------------
                // Primitive array -> AngelScript array<T>
                // ------------------------------------------------------------

                const char* as_element_type = s7TypeToAS(f.type);

                if (!as_element_type)
                    continue;

                auto p_meta = std::make_unique<FieldMeta>();

                p_meta->path = field_path;
                p_meta->s7type = f.type;
                p_meta->abs_offset = f.offset;
                p_meta->bit_index = f.bit_index;
                p_meta->count = f.count;
                p_meta->endian = f.endianness;

                FieldMeta* raw = p_meta.get();

                t_registry.field_meta.push_back(std::move(p_meta));

                std::string getter_sig = fmt::format("array<{}>@ get_{}() const", as_element_type, safe_name);

                int r = t_host.getEngine()->RegisterObjectMethod(
                    t_as_type_name.c_str(), getter_sig.c_str(), asFUNCTION(GenericFieldGetter_primitive_array), asCALL_GENERIC, raw);

                if (r < 0) {
                    fmt::print(stderr,
                        "[SchemaVM] Failed to register array getter '{}' "
                        "on '{}': {}\n",
                        getter_sig, t_as_type_name, r);
                }
            }
            continue;
        }

        // ================================================================
        // STRUCT / UDT / DTL / DateTime
        // ================================================================
        if (!f.children.empty() || !f.udt_name.empty() || f.type == scl::DataType::DTL || f.type == scl::DataType::DateTime) {

            std::string ret_type;

            // ------------------------------------------------------------
            // Named UDT
            // ------------------------------------------------------------
            if (!f.udt_name.empty()) {
                ret_type = sanitizeFieldName(f.udt_name);
            }

            // ------------------------------------------------------------
            // Anonymous / inline STRUCT
            //
            // Example:
            //
            // tank : STRUCT
            //     level  : REAL;
            //     volume : REAL;
            // END_STRUCT;
            //
            // Give it a synthetic AngelScript type.
            // ------------------------------------------------------------
            else if (!f.children.empty()) {

                ret_type = t_as_type_name + "_" + safe_name;

                registerProxyStructType(t_host, ret_type, f.children, t_registry);
            }

            // ------------------------------------------------------------
            // DTL / DateTime
            // ------------------------------------------------------------
            else {
                ret_type = "FieldProxy";
            }

            auto p_meta = std::make_unique<FieldMeta>();

            p_meta->path = field_path;
            p_meta->s7type = f.type;
            p_meta->abs_offset = f.offset;
            p_meta->bit_index = f.bit_index;
            p_meta->count = f.count;
            p_meta->endian = f.endianness;

            FieldMeta* raw = p_meta.get();

            t_registry.field_meta.push_back(std::move(p_meta));

            std::string getter_sig = fmt::format("{}@ get_{}() const", ret_type, safe_name);

            int r = t_host.getEngine()->RegisterObjectMethod(
                t_as_type_name.c_str(), getter_sig.c_str(), asFUNCTION(GenericFieldGetter_proxy), asCALL_GENERIC, raw);

            if (r < 0) {
                fmt::print(stderr,
                    "[SchemaVM] Failed to register nested getter '{}' "
                    "on '{}': {}\n",
                    getter_sig, t_as_type_name, r);
            }

            continue;
        }

        // ================================================================
        // PRIMITIVE
        // ================================================================
        const char* as_field_type = s7TypeToAS(f.type);

        if (!as_field_type)
            continue;

        auto p_meta = std::make_unique<FieldMeta>();

        p_meta->path = field_path;
        p_meta->s7type = f.type;
        p_meta->abs_offset = f.offset;
        p_meta->bit_index = f.bit_index;
        p_meta->count = f.count;
        p_meta->endian = f.endianness;
        // For scalar STRING[N] fields f.count holds N (the character capacity).
        // f.string_capacity is non-zero only for array-of-strings; for scalar
        // strings f.count IS the capacity and f.string_capacity stays 0.
        p_meta->string_capacity = (f.string_capacity > 0) ? f.string_capacity : f.count;

        FieldMeta* raw = p_meta.get();

        t_registry.field_meta.push_back(std::move(p_meta));

        // ------------------------------------------------------------
        // IMPORTANT:
        //
        // Primitive fields MUST use typed accessors.
        //
        // BOOL  -> bool
        // REAL  -> float
        // LREAL -> double
        // INT   -> int
        // etc.
        // ------------------------------------------------------------
        auto acc = accessorsForASType(as_field_type);

        std::string getter_sig = fmt::format("{} get_{}() const", as_field_type, safe_name);

        int r1 = t_host.getEngine()->RegisterObjectMethod(t_as_type_name.c_str(), getter_sig.c_str(), acc.getter, asCALL_GENERIC, raw);

        if (r1 < 0) {
            fmt::print(stderr,
                "[SchemaVM] Failed to register getter '{}' "
                "on '{}': {}\n",
                getter_sig, t_as_type_name, r1);
        }

        // ------------------------------------------------------------
        // Setter
        // ------------------------------------------------------------
        std::string setter_sig;
        std::string ast_type = as_field_type;

        if (ast_type == "string") {
            setter_sig = fmt::format("void set_{}(const string &in)", safe_name);
        } else if (ast_type == "int64" || ast_type == "uint64" || ast_type == "float" || ast_type == "double" || ast_type == "bool") {
            setter_sig = fmt::format("void set_{}({} val)", safe_name, as_field_type);
        } else {
            // Sub-32-bit and 32-bit integer types: use 'int' so that integer
            // literals and hex (e.g. 0x10) are accepted without explicit casts.
            // The GenericFieldSetter_int reads GetArgDWord and truncates internally.
            setter_sig = fmt::format("void set_{}(int val)", safe_name);
        }

        int r2 = t_host.getEngine()->RegisterObjectMethod(t_as_type_name.c_str(), setter_sig.c_str(), acc.setter, asCALL_GENERIC, raw);

        if (r2 < 0) {
            fmt::print(stderr,
                "[SchemaVM] Failed to register setter '{}' "
                "on '{}': {}\n",
                setter_sig, t_as_type_name, r2);
        }
    }
}
static void registerUdtFieldProperties(sgrn::scripting::ScriptHost& t_host, const std::string& t_as_type_name,
    const std::vector<scl::DbField>& t_fields, SchemaVMRegistry& t_registry) {

    for (const auto& f : t_fields) {
        std::string safe_name = sanitizeFieldName(f.name);

        auto p_name = std::make_unique<std::string>(f.name);
        std::string* raw_name = p_name.get();
        t_registry.udt_field_names.push_back(std::move(p_name));

        bool is_string = (f.type == scl::DataType::String || f.type == scl::DataType::WString || f.type == scl::DataType::XString ||
                          f.type == scl::DataType::XWString);

        if (f.count > 1 && !is_string) {
            if (!f.udt_name.empty()) {
                // UDT array
                auto p_meta = std::make_unique<UdtArrayMeta>();
                p_meta->path = f.name;
                p_meta->udt_name = sanitizeFieldName(f.udt_name);
                p_meta->count = f.count;
                UdtArrayMeta* raw = p_meta.get();
                t_registry.udt_array_metas.push_back(std::move(p_meta));

                std::string decl = fmt::format("array<{}@>@", sanitizeFieldName(f.udt_name));
                std::string getter_sig = fmt::format("{} get_{}() const", decl, safe_name);
                t_host.getEngine()->RegisterObjectMethod(
                    t_as_type_name.c_str(), getter_sig.c_str(), asFUNCTION(GenericUDTFieldGetter_udt_array), asCALL_GENERIC, raw);
            } else {
                // Primitive array
                std::string getter_sig = fmt::format("FieldProxy@ get_{}() const", safe_name);
                t_host.getEngine()->RegisterObjectMethod(
                    t_as_type_name.c_str(), getter_sig.c_str(), asFUNCTION(GenericUDTFieldGetter_proxy), asCALL_GENERIC, raw_name);
            }
            continue;
        }

        if (!f.children.empty() || f.type == scl::DataType::DTL || f.type == scl::DataType::DateTime) {
            std::string ret_type = "FieldProxy";
            if (!f.udt_name.empty()) {
                ret_type = sanitizeFieldName(f.udt_name);
            }

            std::string getter_sig = fmt::format("{}@ get_{}() const", ret_type, safe_name);
            t_host.getEngine()->RegisterObjectMethod(
                t_as_type_name.c_str(), getter_sig.c_str(), asFUNCTION(GenericUDTFieldGetter_proxy), asCALL_GENERIC, raw_name);
            continue;
        }

        const char* as_field_type = s7TypeToAS(f.type);
        if (!as_field_type)
            continue;

        asSFuncPtr getter = {0};
        asSFuncPtr setter = {0};
        std::string ast = as_field_type;
        if (ast == "float" || ast == "REAL") {
            getter = asFUNCTION(GenericUDTFieldGetter_float);
            setter = asFUNCTION(GenericUDTFieldSetter_float);
        } else if (ast == "double" || ast == "LREAL") {
            getter = asFUNCTION(GenericUDTFieldGetter_double);
            setter = asFUNCTION(GenericUDTFieldSetter_double);
        } else if (ast == "bool" || ast == "BOOL") {
            getter = asFUNCTION(GenericUDTFieldGetter_bool);
            setter = asFUNCTION(GenericUDTFieldSetter_bool);
        } else if (ast == "string") {
            getter = asFUNCTION(GenericUDTFieldGetter_string);
            setter = asFUNCTION(GenericUDTFieldSetter_string);
        } else if (ast == "DTL@") {
            getter = asFUNCTION(GenericUDTFieldGetter_dtl);
            setter = asFUNCTION(GenericUDTFieldSetter_dtl);
        } else if (ast == "int8" || ast == "uint8") {
            getter = asFUNCTION(GenericUDTFieldGetter_int8);
            setter = asFUNCTION(GenericUDTFieldSetter_int8);
        } else if (ast == "int16" || ast == "uint16") {
            getter = asFUNCTION(GenericUDTFieldGetter_int16);
            setter = asFUNCTION(GenericUDTFieldSetter_int16);
        } else if (ast == "int64" || ast == "uint64") {
            getter = asFUNCTION(GenericUDTFieldGetter_int64);
            setter = asFUNCTION(GenericUDTFieldSetter_int64);
        } else {
            getter = asFUNCTION(GenericUDTFieldGetter_int32);
            setter = asFUNCTION(GenericUDTFieldSetter_int32);
        }

        std::string getter_sig = fmt::format("{} get_{}() const", as_field_type, safe_name);
        int r1 = t_host.getEngine()->RegisterObjectMethod(t_as_type_name.c_str(), getter_sig.c_str(), getter, asCALL_GENERIC, raw_name);
        if (r1 < 0) {
            fmt::print(stderr, "[Error] Failed to register getter '{}' for type {}: code {}\n", getter_sig, t_as_type_name, r1);
        }

        std::string setter_sig;
        std::string ast_sig = as_field_type;
        if (ast_sig == "string") {
            setter_sig = fmt::format("void set_{}(const string &in)", safe_name);
        } else if (ast_sig == "int64" || ast_sig == "uint64" || ast_sig == "float" || ast_sig == "double" || ast_sig == "bool" || ast_sig == "DTL@") {
            setter_sig = fmt::format("void set_{}({} val)", safe_name, as_field_type);
        } else {
            // Use 'int' for sub-32-bit/32-bit integer types so hex literals work without casts.
            setter_sig = fmt::format("void set_{}(int val)", safe_name);
        }
        int r2 = t_host.getEngine()->RegisterObjectMethod(t_as_type_name.c_str(), setter_sig.c_str(), setter, asCALL_GENERIC, raw_name);
        if (r2 < 0) {
            fmt::print(stderr, "[Error] Failed to register setter '{}' for type {}: code {}\n", setter_sig, t_as_type_name, r2);
        }
    }
}

// ── Type registration ────────────────────────────────────────────────────────

#define AS_M(cls, method) asMETHOD(cls, method), asCALL_THISCALL

static ScriptDataBlock* IdentityCast(ScriptDataBlock* tp_db) {
    if (tp_db)
        tp_db->addRef();
    return tp_db;
}

static void SchemaDbCastGeneric(asIScriptGeneric* tp_gen) {
    auto* p_db = static_cast<ScriptDataBlock*>(tp_gen->GetObject());
    auto expected_db_num = reinterpret_cast<uintptr_t>(tp_gen->GetAuxiliary());

    if (p_db && p_db->getDbNumber() == expected_db_num) {
        p_db->addRef();
        tp_gen->SetReturnAddress(p_db);
    } else {
        tp_gen->SetReturnAddress(nullptr);
    }
}

static void SchemaDbFactoryGeneric(asIScriptGeneric* tp_gen) {
    auto db_num = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(tp_gen->GetAuxiliary()));
    if (!p_g_active_connection) {
        tp_gen->SetReturnAddress(nullptr);
        if (auto* p_ctx = asGetActiveContext())
            p_ctx->SetException("No active S7Client scope for schema DB constructor");
        return;
    }
    tp_gen->SetReturnAddress(new ScriptDataBlock(p_g_active_connection, db_num));
}

static ScriptFieldProxy* IdentityProxyCast(ScriptFieldProxy* tp_proxy) {
    if (tp_proxy)
        tp_proxy->addRef();
    return tp_proxy;
}

void registerSchemaTypes(sgrn::scripting::ScriptHost& t_host, const PlcSchemaStore& t_store, SchemaVMRegistry& t_registry) {
    // 1. Register all UDT types first
    for (const auto& udt : t_store.udts()) {
        std::string tn = sanitizeFieldName(udt.name);
        if (t_registry.registered_schema_types.count(tn))
            continue;
        t_registry.registered_schema_types.insert(tn);

        if (t_host.getEngine()->RegisterObjectType(tn.c_str(), 0, asOBJ_REF) < 0)
            continue;

        t_host.getEngine()->RegisterObjectBehaviour(
            tn.c_str(), asBEHAVE_ADDREF, "void f()", asMETHOD(ScriptFieldProxy, addRef), asCALL_THISCALL);
        t_host.getEngine()->RegisterObjectBehaviour(
            tn.c_str(), asBEHAVE_RELEASE, "void f()", asMETHOD(ScriptFieldProxy, release), asCALL_THISCALL);

        // Cast to/from FieldProxy
        t_host.getEngine()->RegisterObjectMethod(tn.c_str(), "FieldProxy@ opCast()", asFUNCTION(IdentityProxyCast), asCALL_CDECL_OBJFIRST);
        t_host.getEngine()->RegisterObjectMethod(
            tn.c_str(), "const FieldProxy@ opCast() const", asFUNCTION(IdentityProxyCast), asCALL_CDECL_OBJFIRST);

        std::string cast_sig = fmt::format("{}@ opCast()", tn);
        t_host.getEngine()->RegisterObjectMethod("FieldProxy", cast_sig.c_str(), asFUNCTION(IdentityProxyCast), asCALL_CDECL_OBJFIRST);
        std::string const_cast_sig = fmt::format("const {}@ opCast() const", tn);
        t_host.getEngine()->RegisterObjectMethod(
            "FieldProxy", const_cast_sig.c_str(), asFUNCTION(IdentityProxyCast), asCALL_CDECL_OBJFIRST);
    }

    // 2. Register fields for all UDT types
    for (const auto& udt : t_store.udts()) {
        std::string tn = sanitizeFieldName(udt.name);
        if (t_registry.registered_udt_properties.count(tn))
            continue;
        t_registry.registered_udt_properties.insert(tn);
        registerUdtFieldProperties(t_host, tn, udt.fields, t_registry);
    }

    // 3. Register all DB types
    for (const auto& [db_num, tp_db] : t_store.dbs()) {
        std::string tn = sanitizeFieldName(tp_db.db_name.empty() ? fmt::format("DB{}", db_num) : tp_db.db_name);

        if (t_registry.registered_schema_types.count(tn))
            continue;
        t_registry.registered_schema_types.insert(tn);

        asIScriptEngine* p_engine = t_host.getEngine();
        if (p_engine->RegisterObjectType(tn.c_str(), 0, asOBJ_REF) < 0)
            continue;

        const char* t_t = tn.c_str();
        std::string factory_sig = fmt::format("{}@ f()", tn);
        p_engine->RegisterObjectBehaviour(t_t, asBEHAVE_FACTORY, factory_sig.c_str(), asFUNCTION(SchemaDbFactoryGeneric), asCALL_GENERIC,
            reinterpret_cast<void*>(static_cast<uintptr_t>(db_num)));
        p_engine->RegisterObjectBehaviour(t_t, asBEHAVE_ADDREF, "void f()", AS_M(ScriptDataBlock, addRef));
        p_engine->RegisterObjectBehaviour(t_t, asBEHAVE_RELEASE, "void f()", AS_M(ScriptDataBlock, release));

        // S7-semantic operations
        std::string get_sig = fmt::format("{}@ get()", tn);
        p_engine->RegisterObjectMethod(t_t, get_sig.c_str(), asMETHODPR(ScriptDataBlock, get, (void), ScriptDataBlock*), asCALL_THISCALL);
        p_engine->RegisterObjectMethod(t_t, "void put()", asMETHODPR(ScriptDataBlock, put, (void), void), asCALL_THISCALL);
        p_engine->RegisterObjectMethod(t_t, "void put(const string &in, const string &in)",
            asMETHODPR(ScriptDataBlock, put, (const std::string&, const std::string&), void), asCALL_THISCALL);
        p_engine->RegisterObjectMethod(t_t, "void put(const string &in, double)", asMETHOD(ScriptDataBlock, putDouble), asCALL_THISCALL);
        p_engine->RegisterObjectMethod(t_t, "void put(const string &in, int)", asMETHOD(ScriptDataBlock, putInt), asCALL_THISCALL);
        p_engine->RegisterObjectMethod(t_t, "void put(const string &in, bool)", asMETHOD(ScriptDataBlock, putBool), asCALL_THISCALL);
        p_engine->RegisterObjectMethod(t_t, "void put(const string &in, DTL@)", asMETHOD(ScriptDataBlock, putDtl), asCALL_THISCALL);
        p_engine->RegisterObjectMethod(t_t, "void write(const string &in, const string &in)",
            asMETHODPR(ScriptDataBlock, write, (const std::string&, const std::string&), void), asCALL_THISCALL);
        p_engine->RegisterObjectMethod(
            t_t, "void write(const string &in, double)", asMETHOD(ScriptDataBlock, writeDouble), asCALL_THISCALL);
        p_engine->RegisterObjectMethod(t_t, "void write(const string &in, int)", asMETHOD(ScriptDataBlock, writeInt), asCALL_THISCALL);
        p_engine->RegisterObjectMethod(t_t, "void write(const string &in, bool)", asMETHOD(ScriptDataBlock, writeBool), asCALL_THISCALL);
        p_engine->RegisterObjectMethod(t_t, "void write(const string &in, DTL@)", asMETHOD(ScriptDataBlock, writeDtl), asCALL_THISCALL);
        p_engine->RegisterObjectMethod(t_t, "string toJson() const", AS_M(ScriptDataBlock, toJson));
        p_engine->RegisterObjectMethod(t_t, "string diff() const", AS_M(ScriptDataBlock, diff));
        p_engine->RegisterObjectMethod(t_t, "void print() const", AS_M(ScriptDataBlock, print));
        p_engine->RegisterObjectMethod(t_t, "uint16 number() const", AS_M(ScriptDataBlock, getDbNumber));
        p_engine->RegisterObjectMethod(t_t, "string name() const", AS_M(ScriptDataBlock, getDbName));
        p_engine->RegisterObjectMethod(t_t, "FieldProxy@ opIndex(const string &in)", AS_M(ScriptDataBlock, opIndex));
        p_engine->RegisterObjectMethod(t_t, "HexTable@ hex()", asFUNCTION(DataBlockToHexTableCast), asCALL_CDECL_OBJFIRST);

        // HexTable cast
        p_engine->RegisterObjectMethod(t_t, "HexTable@ opCast()", asFUNCTION(DataBlockToHexTableCast), asCALL_CDECL_OBJFIRST);
        p_engine->RegisterObjectMethod(t_t, "const HexTable@ opCast() const", asFUNCTION(DataBlockToHexTableCast), asCALL_CDECL_OBJFIRST);

        // Bi-directional casts between DataBlock and this schema type
        std::string cast_sig = fmt::format("{}@ opCast()", tn);
        p_engine->RegisterObjectMethod("DataBlock", cast_sig.c_str(), asFUNCTION(SchemaDbCastGeneric), asCALL_GENERIC,
            reinterpret_cast<void*>(static_cast<uintptr_t>(db_num)));
        std::string const_cast_sig = fmt::format("const {}@ opCast() const", tn);
        p_engine->RegisterObjectMethod("DataBlock", const_cast_sig.c_str(), asFUNCTION(SchemaDbCastGeneric), asCALL_GENERIC,
            reinterpret_cast<void*>(static_cast<uintptr_t>(db_num)));

        p_engine->RegisterObjectMethod(t_t, "DataBlock@ opCast()", asFUNCTION(IdentityCast), asCALL_CDECL_OBJFIRST);
        p_engine->RegisterObjectMethod(t_t, "const DataBlock@ opCast() const", asFUNCTION(IdentityCast), asCALL_CDECL_OBJFIRST);

        registerFieldProperties(t_host, tn, tp_db.fields, "", t_registry);

        fmt::print(fg(fmt::color::dark_cyan), "  [schema] registered type '{}' (DB{}, {} bytes)\n", tn, db_num, tp_db.size_bytes);
    }
}

static void RuntimeDbPropertyGetter(asIScriptGeneric* tp_gen) {
    auto* p_rt = static_cast<PlcRuntimeWrapper*>(tp_gen->GetObject());
    auto db_num = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(tp_gen->GetAuxiliary()));
    if (!p_rt) {
        tp_gen->SetReturnAddress(nullptr);
        return;
    }
    tp_gen->SetReturnAddress(p_rt->db(db_num));
}

static void ClientDbPropertyGetter(asIScriptGeneric* tp_gen) {
    auto* p_client = static_cast<ScriptS7Client*>(tp_gen->GetObject());
    auto db_num = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(tp_gen->GetAuxiliary()));
    if (!p_client) {
        tp_gen->SetReturnAddress(nullptr);
        return;
    }
    tp_gen->SetReturnAddress(p_client->db(db_num));
}

void registerDbPropertyAccessors(sgrn::scripting::ScriptHost& t_host, const PlcSchemaStore& t_store, SchemaVMRegistry& t_registry) {
    asIScriptEngine* p_engine = t_host.getEngine();
    if (!p_engine)
        return;

    for (const auto& [db_num, tp_db] : t_store.dbs()) {
        std::string type_name = sanitizeFieldName(tp_db.db_name.empty() ? fmt::format("DB{}", db_num) : tp_db.db_name);

        // Ensure the DB type itself was registered (or at least some type with this name, could be UDT)
        if (!t_registry.registered_schema_types.count(type_name)) {
            continue;
        }

        // If the type name collides with a UDT, the DB type registration was skipped.
        // We must return a generic DataBlock@ instead of the specific type, otherwise
        // AngelScript expects a UDT (ScriptFieldProxy*) but we return a ScriptDataBlock*, causing a segfault.
        std::string ret_type = type_name;
        if (t_registry.registered_udt_properties.count(type_name)) {
            ret_type = "DataBlock";
            fmt::print(stderr, fg(fmt::color::yellow),
                "[SchemaVM] Warning: DB name '{}' collides with a UDT. Using generic DataBlock access.\n", type_name);
        }

        // Convert PascalCase DB name to snake_case for the property accessor (just like injectDbRefs)
        // Note: we'll register the PascalCase name as well, since user requested it.
        // E.g., DB name "PrimaryCoolant" becomes property "primary_coolant" and "PrimaryCoolant".

        std::string snake_name = type_name;
        // Basic snake case conversion (simplified version of toSnakeCase in s7shell_lib.cpp)
        std::string out;
        out.reserve(snake_name.size() + 4);
        for (size_t i = 0; i < snake_name.size(); ++i) {
            char c = snake_name[i];
            if (std::isupper(static_cast<unsigned char>(c))) {
                if (i > 0 && !std::isupper(static_cast<unsigned char>(snake_name[i - 1])) && snake_name[i - 1] != '_')
                    out += '_';
                out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            } else if (std::isalnum(static_cast<unsigned char>(c))) {
                out += c;
            } else {
                out += '_';
            }
        }
        std::string clean;
        bool prev_us = true;
        for (char c : out) {
            if (c == '_') {
                if (!prev_us)
                    clean += c;
                prev_us = true;
            } else {
                clean += c;
                prev_us = false;
            }
        }
        while (!clean.empty() && clean.back() == '_')
            clean.pop_back();
        if (clean.empty())
            clean = "db_" + type_name;
        snake_name = clean;

        void* aux = reinterpret_cast<void*>(static_cast<uintptr_t>(db_num));

        sgrn::scripting::g_suppress_errors = true;
        // Try registering original PascalCase name if it doesn't conflict
        std::string getter_orig = fmt::format("{}@ get_{}()", ret_type, type_name);
        p_engine->RegisterObjectMethod("PlcRuntime", getter_orig.c_str(), asFUNCTION(RuntimeDbPropertyGetter), asCALL_GENERIC, aux);
        p_engine->RegisterObjectMethod("S7Client", getter_orig.c_str(), asFUNCTION(ClientDbPropertyGetter), asCALL_GENERIC, aux);

        // Try registering snake_case name
        if (snake_name != type_name) {
            std::string getter_snake = fmt::format("{}@ get_{}()", ret_type, snake_name);
            p_engine->RegisterObjectMethod("PlcRuntime", getter_snake.c_str(), asFUNCTION(RuntimeDbPropertyGetter), asCALL_GENERIC, aux);
            p_engine->RegisterObjectMethod("S7Client", getter_snake.c_str(), asFUNCTION(ClientDbPropertyGetter), asCALL_GENERIC, aux);
        }
        sgrn::scripting::g_suppress_errors = false;
    }
}

#undef AS_M

} // namespace sgrn::s7shell::shell
