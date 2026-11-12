#include <fmt/color.h>
#include <fmt/format.h>
#include <sgrn/s7shell/utils/json-helpers.hpp>
#include <sgrn/utils/json-dom.hpp>
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
    sgrn::json::dom::Document doc;
    if (doc.parse(t_json.c_str()).hasParseError())
        return t_fallback;
    if (doc.isDouble() || doc.is_number())
        return doc.getDouble();
    if (doc.isInt())
        return static_cast<double>(doc.getInt());
    if (doc.isUint())
        return static_cast<double>(doc.getUint());
    if (doc.is_boolean())
        return doc.getBool() ? 1.0 : 0.0;
    return t_fallback;
}

int32_t jsonScalarInt(const std::string& t_json, int32_t t_fallback) {
    sgrn::json::dom::Document doc;
    if (doc.parse(t_json.c_str()).hasParseError())
        return t_fallback;
    if (doc.isInt())
        return doc.getInt();
    if (doc.isUint())
        return static_cast<int32_t>(doc.getUint());
    if (doc.isDouble() || doc.is_number())
        return static_cast<int32_t>(doc.getDouble());
    if (doc.is_boolean())
        return doc.getBool() ? 1 : 0;
    return t_fallback;
}

} // namespace sgrn::s7shell::shell
