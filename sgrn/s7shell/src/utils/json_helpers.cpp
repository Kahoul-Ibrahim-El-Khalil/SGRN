#include <fmt/color.h>
#include <fmt/format.h>
#include <sgrn/s7shell/utils/json_helpers.hpp>
#include <angelscript.h>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <scriptarray/scriptarray.h>
#include <scriptdictionary/scriptdictionary.h>

namespace sgrn::s7shell::shell
{

void logError(const ::sgrn::scl::Error& t_err) {
    fmt::print(stderr, fg(fmt::color::red), "Error: {}\n", t_err.string());
}

void logError(const ::sgrn::scl::Error& t_err, std::string_view t_ctx) {
    fmt::print(stderr, fg(fmt::color::red), "Error in {}: {}\n", t_ctx, t_err.string());
}

void logError(const ::sgrn::gateway::wrappers::s7::S7Error& t_err) {
    fmt::print(stderr, fg(fmt::color::red), "Error: {}\n", t_err.string());
}

void logError(const ::sgrn::gateway::wrappers::s7::S7Error& t_err, std::string_view t_ctx) {
    fmt::print(stderr, fg(fmt::color::red), "Error in {}: {}\n", t_ctx, t_err.string());
}

bool ok(sgrn::Result<void, ::sgrn::scl::Error>&& t_res, std::string_view t_ctx) {
    if (t_res.hasError()) {
        if (t_ctx.empty())
            logError(t_res.error());
        else
            logError(t_res.error(), t_ctx);
        return false;
    }
    return true;
}

bool ok(sgrn::Result<void, ::sgrn::gateway::wrappers::s7::S7Error>&& t_res, std::string_view t_ctx) {
    if (t_res.hasError()) {
        if (t_ctx.empty())
            logError(t_res.error());
        else
            logError(t_res.error(), t_ctx);
        return false;
    }
    return true;
}

double jsonScalarDouble(const std::string& t_json, double t_fallback) {
    rapidjson::Document doc;
    if (doc.Parse(t_json.c_str()).HasParseError())
        return t_fallback;
    if (doc.IsDouble() || doc.IsNumber())
        return doc.GetDouble();
    if (doc.IsInt())
        return static_cast<double>(doc.GetInt());
    if (doc.IsUint())
        return static_cast<double>(doc.GetUint());
    if (doc.IsBool())
        return doc.GetBool() ? 1.0 : 0.0;
    return t_fallback;
}

int32_t jsonScalarInt(const std::string& t_json, int32_t t_fallback) {
    rapidjson::Document doc;
    if (doc.Parse(t_json.c_str()).HasParseError())
        return t_fallback;
    if (doc.IsInt())
        return doc.GetInt();
    if (doc.IsUint())
        return static_cast<int32_t>(doc.GetUint());
    if (doc.IsDouble() || doc.IsNumber())
        return static_cast<int32_t>(doc.GetDouble());
    if (doc.IsBool())
        return doc.GetBool() ? 1 : 0;
    return t_fallback;
}

static void valueToJson(
    asIScriptEngine* tp_engine, const void* tp_value, int t_type_id, rapidjson::Writer<rapidjson::StringBuffer>& t_writer);

static void dictToJson(asIScriptEngine* tp_engine, const CScriptDictionary* tp_dict, rapidjson::Writer<rapidjson::StringBuffer>& t_writer) {
    t_writer.StartObject();
    for (auto it = tp_dict->begin(); it != tp_dict->end(); ++it) {
        t_writer.Key(it.GetKey().c_str());
        valueToJson(tp_engine, it.GetAddressOfValue(), it.GetTypeId(), t_writer);
    }
    t_writer.EndObject();
}

static void arrayToJson(asIScriptEngine* tp_engine, const CScriptArray* tp_arr, rapidjson::Writer<rapidjson::StringBuffer>& t_writer) {
    t_writer.StartArray();
    int t_type_id = tp_arr->GetElementTypeId();
    for (asUINT i = 0; i < tp_arr->GetSize(); ++i) {
        valueToJson(tp_engine, tp_arr->At(i), t_type_id, t_writer);
    }
    t_writer.EndArray();
}

static void valueToJson(
    asIScriptEngine* tp_engine, const void* tp_value, int t_type_id, rapidjson::Writer<rapidjson::StringBuffer>& t_writer) {
    if (!tp_value) {
        t_writer.Null();
        return;
    }

    if (t_type_id & asTYPEID_OBJHANDLE) {
        void* p_obj = *(void**)tp_value;
        int sub_type_id = t_type_id & ~asTYPEID_OBJHANDLE;
        valueToJson(tp_engine, p_obj, sub_type_id, t_writer);
        return;
    }

    if (t_type_id == asTYPEID_BOOL) {
        t_writer.Bool(*(const bool*)tp_value);
    } else if (t_type_id == asTYPEID_INT8 || t_type_id == asTYPEID_INT16 || t_type_id == asTYPEID_INT32) {
        t_writer.Int(*(const int*)tp_value);
    } else if (t_type_id == asTYPEID_UINT8 || t_type_id == asTYPEID_UINT16 || t_type_id == asTYPEID_UINT32) {
        t_writer.Uint(*(const unsigned int*)tp_value);
    } else if (t_type_id == asTYPEID_INT64) {
        t_writer.Int64(*(const int64_t*)tp_value);
    } else if (t_type_id == asTYPEID_UINT64) {
        t_writer.Uint64(*(const uint64_t*)tp_value);
    } else if (t_type_id == asTYPEID_FLOAT) {
        t_writer.Double(*(const float*)tp_value);
    } else if (t_type_id == asTYPEID_DOUBLE) {
        t_writer.Double(*(const double*)tp_value);
    } else {
        asITypeInfo* p_type = tp_engine->GetTypeInfoById(t_type_id);
        if (p_type) {
            std::string type_name = p_type->GetName();
            if (type_name == "string") {
                t_writer.String(((const std::string*)tp_value)->c_str());
            } else if (type_name == "dictionary") {
                dictToJson(tp_engine, (const CScriptDictionary*)tp_value, t_writer);
            } else if (type_name == "array") {
                arrayToJson(tp_engine, (const CScriptArray*)tp_value, t_writer);
            } else {
                t_writer.Null();
            }
        } else {
            t_writer.Null();
        }
    }
}

std::string convertDictToJson(CScriptDictionary* tp_dict) {
    if (!tp_dict)
        return "null";
    asIScriptContext* p_ctx = asGetActiveContext();
    if (!p_ctx)
        return "null";
    asIScriptEngine* p_engine = p_ctx->GetEngine();
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);
    dictToJson(p_engine, tp_dict, t_writer);
    return sb.GetString();
}

std::string convertArrayToJson(CScriptArray* tp_arr) {
    if (!tp_arr)
        return "null";
    asIScriptContext* p_ctx = asGetActiveContext();
    if (!p_ctx)
        return "null";
    asIScriptEngine* p_engine = p_ctx->GetEngine();
    rapidjson::StringBuffer sb;
    rapidjson::Writer<rapidjson::StringBuffer> t_writer(sb);
    arrayToJson(p_engine, tp_arr, t_writer);
    return sb.GetString();
}

} // namespace sgrn::s7shell::shell
