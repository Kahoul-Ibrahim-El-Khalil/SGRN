#pragma once
#include <sgrn/Result.hpp>
#include <angelscript.h>
#include <memory>
#include <string>
#include <type_traits>

namespace sgrn::scripting
{

class ScriptHost {
public:
    ScriptHost();
    explicit ScriptHost(asIScriptEngine* tp_existing_engine);
    ~ScriptHost();

    // Disable copy/move
    ScriptHost(const ScriptHost&) = delete;
    ScriptHost& operator=(const ScriptHost&) = delete;

    /// Compile a file into a module named "main".
    /// Reports line/column on error, never partially applies a broken script.
    sgrn::Result<void> loadFile(const std::string& t_path);

    /// Reload the last successfully loaded file.
    sgrn::Result<void> reload();

    /// Register a type
    void registerType(const std::string& t_declaration, int t_flags = asOBJ_REF | asOBJ_NOCOUNT);

    /// Register object methods, behaviours, and properties
    void registerObjectMethod(const std::string& t_obj, const std::string& t_declaration, const asSFuncPtr& t_func_pointer,
        asDWORD t_call_conv, void* tp_auxiliary = nullptr);
    void registerObjectBehaviour(const std::string& t_obj, asEBehaviours t_behaviour, const std::string& t_declaration,
        const asSFuncPtr& t_func_pointer, asDWORD t_call_conv, void* tp_auxiliary = nullptr);
    void registerObjectProperty(const std::string& t_obj, const std::string& t_declaration, int t_byte_offset);

    /// Type reflection
    asITypeInfo* getTypeInfoByDecl(const std::string& t_decl) const;

    /// Register a global function
    void registerGlobalFunction(
        const std::string& t_declaration, const asSFuncPtr& t_func_pointer, asDWORD t_call_conv, void* tp_obj_for_thiscall = nullptr);

    /// Get underlying engine
    asIScriptEngine* getEngine() const {
        return p_engine_;
    }

    /// Invoke a global function in the "main" module
    template <typename T, typename... Args>
    sgrn::Result<T> call(const std::string& t_function_name, Args... t_args) {
        if (!p_engine_)
            return sgrn::Result<T>::Error("Engine not initialized");
        asIScriptModule* p_mod = p_engine_->GetModule("main");
        if (!p_mod)
            return sgrn::Result<T>::Error("No module loaded");

        // Construct declaration, assuming void if we don't care, but angelscript needs exact match.
        // Actually, getting function by name alone works if no overloads:
        asIScriptFunction* p_func = p_mod->GetFunctionByName(t_function_name.c_str());
        if (!p_func)
            return sgrn::Result<T>::Error("Function not found: " + t_function_name);

        asIScriptContext* p_ctx = p_engine_->CreateContext();
        if (!p_ctx)
            return sgrn::Result<T>::Error("Failed to create context");

        p_ctx->Prepare(p_func);

        // Set args
        int arg_index = 0;
        (
            [&] {
                if constexpr (std::is_pointer_v<Args>) {
                    p_ctx->SetArgObject(arg_index++, (void*)t_args);
                } else if constexpr (std::is_same_v<Args, int> || std::is_same_v<Args, bool>) {
                    p_ctx->SetArgDWord(arg_index++, t_args);
                } else if constexpr (std::is_same_v<Args, double>) {
                    p_ctx->SetArgDouble(arg_index++, t_args);
                } else if constexpr (std::is_same_v<Args, float>) {
                    p_ctx->SetArgFloat(arg_index++, t_args);
                } else {
                    p_ctx->SetArgObject(arg_index++, (void*)&t_args);
                }
            }(),
            ...);

        // Execute with timeout (e.g., 5 seconds) could be added via line callback, but we keep it simple for now.
        int r = p_ctx->Execute();
        if (r != asEXECUTION_FINISHED) {
            std::string err = "Execution failed";
            if (r == asEXECUTION_EXCEPTION)
                err = "Exception: " + std::string(p_ctx->GetExceptionString());
            p_ctx->Release();
            return sgrn::Result<T>::Error(err);
        }

        if constexpr (std::is_same_v<T, void>) {
            p_ctx->Release();
            return sgrn::Result<T>::Ok();
        } else if constexpr (std::is_pointer_v<T>) {
            T ret = (T)p_ctx->GetReturnObject();
            p_ctx->Release();
            return sgrn::Result<T>::Ok(ret);
        } else if constexpr (std::is_same_v<T, int> || std::is_same_v<T, bool>) {
            T ret = (T)p_ctx->GetReturnDWord();
            p_ctx->Release();
            return sgrn::Result<T>::Ok(ret);
        } else if constexpr (std::is_same_v<T, double>) {
            T ret = (T)p_ctx->GetReturnDouble();
            p_ctx->Release();
            return sgrn::Result<T>::Ok(ret);
        } else if constexpr (std::is_same_v<T, float>) {
            T ret = (T)p_ctx->GetReturnFloat();
            p_ctx->Release();
            return sgrn::Result<T>::Ok(ret);
        } else {
            // Default object return
            T ret = *(T*)p_ctx->GetReturnObject();
            p_ctx->Release();
            return sgrn::Result<T>::Ok(ret);
        }
    }

private:
    asIScriptEngine* p_engine_{nullptr};
    bool owns_engine_{true};
    std::string loaded_path_;
};

} // namespace sgrn::scripting
