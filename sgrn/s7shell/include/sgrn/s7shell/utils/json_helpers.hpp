#pragma once

#include <fmt/format.h>
#include <sgrn/Result.hpp>
#include <sgrn/gateway/wrappers/s7/S7Client.hpp>
#include <sgrn/gateway/wrappers/s7/error.hpp>
#include <sgrn/scl/types.hpp>
#include <sgrn/utils/encoding.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Forward declarations
class asIScriptEngine;
class CScriptDictionary;
class CScriptArray;

namespace sgrn::s7shell::shell
{

void logError(scl::SclError t_err);
void logError(scl::SclError t_err, std::string_view t_ctx);

void logError(::sgrn::gateway::wrappers::s7::S7Error t_err);
void logError(::sgrn::gateway::wrappers::s7::S7Error t_err, std::string_view t_ctx);

template <typename T>
T valueOr(sgrn::Result<T, sgrn::scl::SclError>&& t_res, T t_fallback, std::string_view t_ctx = {}) {
    if (t_res.hasError()) {
        if (t_ctx.empty())
            logError(t_res.error());
        else
            logError(t_res.error(), t_ctx);
        return t_fallback;
    }
    return std::move(t_res.value());
}

template <typename T>
T valueOr(sgrn::Result<T, ::sgrn::gateway::wrappers::s7::S7Error>&& t_res, T t_fallback, std::string_view t_ctx = {}) {
    if (t_res.hasError()) {
        if (t_ctx.empty())
            logError(t_res.error());
        else
            logError(t_res.error(), t_ctx);
        return t_fallback;
    }
    return std::move(t_res.value());
}

bool ok(sgrn::Result<void, sgrn::scl::SclError>&& t_res, std::string_view t_ctx = {});
bool ok(sgrn::Result<void, ::sgrn::gateway::wrappers::s7::S7Error>&& t_res, std::string_view t_ctx = {});

double jsonScalarDouble(const std::string& t_json, double t_fallback = 0.0);
int32_t jsonScalarInt(const std::string& t_json, int32_t t_fallback = 0);

std::string convertDictToJson(CScriptDictionary* tp_dict);
std::string convertArrayToJson(CScriptArray* tp_arr);

inline const char* plcStatusText(::sgrn::gateway::wrappers::s7::PlcStatus t_s) {
    switch (t_s) {
        case ::sgrn::gateway::wrappers::s7::PlcStatus::Run:
            return "Run";
        case ::sgrn::gateway::wrappers::s7::PlcStatus::Stop:
            return "Stop";
        default:
            return "Unknown";
    }
}

inline const char* connectionTypeName(uint16_t t_t) {
    if (t_t == CONNTYPE_PG)
        return "PG";
    if (t_t == CONNTYPE_OP)
        return "OP";
    if (t_t == CONNTYPE_BASIC)
        return "BASIC";
    return "unknown";
}

inline std::string formatBlockInfo(const ::sgrn::gateway::wrappers::s7::S7BlockInfo& t_b) {
    return fmt::format("type=0x{:02X} number={} lang={} flags=0x{:02X} mc7={} load={} local={} chk=0x{:04X} ver={} code={} intf={}\n",
        t_b.BlkType, t_b.BlkNumber, t_b.BlkLang, t_b.BlkFlags, t_b.MC7Size, t_b.LoadSize, t_b.SBBLength, t_b.CheckSum, t_b.Version,
        t_b.CodeDate, t_b.IntfDate);
}

inline std::string bytesToHex(const std::vector<uint8_t>& t_bytes) {
    return sgrn::utils::encoding::toHex(t_bytes.data(), t_bytes.size());
}

inline bool writeBinaryFile(const std::string& t_path, const std::vector<uint8_t>& t_data) {
    const std::string normalized = sgrn::utils::filesystem::normalizePath(t_path);
    std::ofstream out(normalized, std::ios::binary);
    if (!out.is_open())
        return false;
    if (!t_data.empty())
        out.write(reinterpret_cast<const char*>(t_data.data()), static_cast<std::streamsize>(t_data.size()));
    return out.good();
}

inline bool readBinaryFile(const std::string& t_path, std::vector<uint8_t>& t_data) {
    const std::string normalized = sgrn::utils::filesystem::normalizePath(t_path);
    std::ifstream in(normalized, std::ios::binary);
    if (!in.is_open())
        return false;
    t_data.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return true;
}

inline bool writeHexFile(const std::string& t_path, const std::string& t_hex) {
    return sgrn::utils::filesystem::writeStringToFile(sgrn::utils::filesystem::normalizePath(t_path), t_hex);
}

inline std::optional<std::string> readHexFile(const std::string& t_path) {
    std::string content;
    if (!sgrn::utils::filesystem::readStringFromFile(sgrn::utils::filesystem::normalizePath(t_path), content))
        return std::nullopt;
    return content;
}

} // namespace sgrn::s7shell::shell
