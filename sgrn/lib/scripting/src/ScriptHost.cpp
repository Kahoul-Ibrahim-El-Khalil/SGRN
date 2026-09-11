#include <fmt/core.h>
#include <sgrn/scripting/ScriptHost.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <filesystem>
#include <fstream>
#include <scriptarray/scriptarray.h>
#include <scriptbuilder/scriptbuilder.h>
#include <scriptdictionary/scriptdictionary.h>
#include <scripthelper/scripthelper.h>
#include <scriptstdstring/scriptstdstring.h>
namespace sgrn::scripting
{

using sgrn::Result;
namespace fs = std::filesystem;
using sgrn::utils::filesystem::normalizePath;

static void as_message_callback(const asSMessageInfo* tp_msg, void* tp_param) {
    (void)tp_param;
    const char* p_type = "ERR ";
    if (tp_msg->type == asMSGTYPE_WARNING)
        p_type = "WARN";
    else if (tp_msg->type == asMSGTYPE_INFORMATION)
        p_type = "INFO";

    const char* p_section = tp_msg->section ? tp_msg->section : "";
    if (p_section[0] != '\0') {
        fmt::print(stderr, "[sgrn_scripting] {} in '{}' ({} :{}): {}\n", p_type, p_section, tp_msg->row, tp_msg->col, tp_msg->message);
    } else {
        fmt::print(stderr, "[sgrn_scripting] {} ({} :{}): {}\n", p_type, tp_msg->row, tp_msg->col, tp_msg->message);
    }
}

ScriptHost::ScriptHost() {
    p_engine_ = asCreateScriptEngine();
    if (!p_engine_) {
        fmt::print(stderr, "[ScriptHost] ERROR: asCreateScriptEngine() returned nullptr (version mismatch or init failure)\n");
        return;
    }
    owns_engine_ = true;
    p_engine_->SetMessageCallback(asFUNCTION(as_message_callback), this, asCALL_CDECL);
    p_engine_->SetEngineProperty(asEP_PROPERTY_ACCESSOR_MODE, 2);

    // Register basic types
    RegisterStdString(p_engine_);
    RegisterScriptArray(p_engine_, true);
    RegisterScriptDictionary(p_engine_);
}

ScriptHost::ScriptHost(asIScriptEngine* tp_existing_engine) {
    p_engine_ = tp_existing_engine;
    owns_engine_ = false;
}

ScriptHost::~ScriptHost() {
    if (p_engine_ && owns_engine_) {
        p_engine_->ShutDownAndRelease();
    }
}

Result<void> ScriptHost::loadFile(const std::string& t_path) {
    if (!p_engine_) {
        return Result<void>::Error("AngelScript engine is null");
    }
    fs::path path = normalizePath(t_path);
    if (!fs::exists(path)) {
        return Result<void>::Error("Path does not exist: " + path.string());
    }
    if (!fs::is_regular_file(path)) {
        return Result<void>::Error(fmt::format("{} is not a file", path.string()));
    }
    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return Result<void>::Error("Cannot open script: " + path.string());
    }
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

    CScriptBuilder builder;
    if (builder.StartNewModule(p_engine_, "main") < 0) {
        return Result<void>::Error("Failed to create module for script");
    }

    if (builder.AddSectionFromMemory(t_path.c_str(), content.c_str()) < 0) {
        return Result<void>::Error("Failed to add section from script content");
    }

    if (builder.BuildModule() < 0) {
        return Result<void>::Error("Compilation failed");
    }

    loaded_path_ = path.string();
    return {};
}

Result<void> ScriptHost::reload() {
    if (loaded_path_.empty()) {
        return Result<void>::Error("No script loaded to reload");
    }
    return loadFile(loaded_path_);
}

void ScriptHost::registerType(const std::string& t_declaration, int t_flags) {
    if (!p_engine_)
        return;
    p_engine_->RegisterObjectType(t_declaration.c_str(), 0, t_flags);
}

void ScriptHost::registerObjectMethod(
    const std::string& t_obj, const std::string& t_declaration, const asSFuncPtr& t_func_pointer, asDWORD t_call_conv, void* tp_auxiliary) {
    if (!p_engine_)
        return;
    p_engine_->RegisterObjectMethod(t_obj.c_str(), t_declaration.c_str(), t_func_pointer, t_call_conv, tp_auxiliary);
}

void ScriptHost::registerObjectBehaviour(const std::string& t_obj, asEBehaviours t_behaviour, const std::string& t_declaration,
    const asSFuncPtr& t_func_pointer, asDWORD t_call_conv, void* tp_auxiliary) {
    if (!p_engine_)
        return;
    p_engine_->RegisterObjectBehaviour(t_obj.c_str(), t_behaviour, t_declaration.c_str(), t_func_pointer, t_call_conv, tp_auxiliary);
}

void ScriptHost::registerObjectProperty(const std::string& t_obj, const std::string& t_declaration, int t_byte_offset) {
    if (!p_engine_)
        return;
    p_engine_->RegisterObjectProperty(t_obj.c_str(), t_declaration.c_str(), t_byte_offset);
}

asITypeInfo* ScriptHost::getTypeInfoByDecl(const std::string& t_decl) const {
    if (!p_engine_)
        return nullptr;
    return p_engine_->GetTypeInfoByDecl(t_decl.c_str());
}

void ScriptHost::registerGlobalFunction(
    const std::string& t_declaration, const asSFuncPtr& t_func_pointer, asDWORD t_call_conv, void* tp_obj_for_thiscall) {
    if (!p_engine_)
        return;
    p_engine_->RegisterGlobalFunction(t_declaration.c_str(), t_func_pointer, t_call_conv, tp_obj_for_thiscall);
}

} // namespace sgrn::scripting
