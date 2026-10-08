#include <fmt/color.h>
#include <fmt/format.h>
#include <sgrn/s7shell/utils/json_helpers.hpp>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <stdexcept>
#include <string>

namespace sgrn::s7shell::shell
{
using sgrn::scl::SclError;
using sgrn::wrappers::s7::S7Error;
void logError(SclError t_err) {
    fmt::print(stderr, fg(fmt::color::red), "SclError: {}\n", toString(t_err));
}

void logError(SclError t_err, std::string_view t_ctx) {
    fmt::print(stderr, fg(fmt::color::red), "SclError in {}: {}\n", t_ctx, toString(t_err));
}

void logError(S7Error t_err) {
    fmt::print(stderr, fg(fmt::color::red), "SclError: {}\n", toString(t_err));
}

void logError(::sgrn::wrappers::s7::S7Error t_err, std::string_view t_ctx) {
    fmt::print(stderr, fg(fmt::color::red), "SclError in {}: {}\n", t_ctx, toString(t_err));
}

bool ok(sgrn::Result<void, SclError>&& t_res, std::string_view t_ctx) {
    if (t_res.hasError()) {
        if (t_ctx.empty())
            logError(t_res.error());
        else
            logError(t_res.error(), t_ctx);
        return false;
    }
    return true;
}

bool ok(sgrn::Result<void, ::sgrn::wrappers::s7::S7Error>&& t_res, std::string_view t_ctx) {
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

} // namespace sgrn::s7shell::shell
