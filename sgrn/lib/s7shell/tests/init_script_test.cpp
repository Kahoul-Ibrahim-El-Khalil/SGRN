// loadInitScript() regression test: a global variable declared AFTER the
// first function definition must still land in repl_module.
//
// Old bug: once a function's closing brace was buffered, fn_buf stayed
// non-empty at depth 0 forever, so every later line (including plain
// global-var declarations) was silently absorbed into fn_buf and never
// compiled into repl_module.

#include <sgrn/s7shell/S7Shell.hpp>

#include <angelscript.h>

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace
{

namespace fs = std::filesystem;

void writeFile(const fs::path& t_path, const std::string& t_text) {
    std::ofstream ofs(t_path, std::ios::binary | std::ios::trunc);
    assert(ofs.is_open());
    ofs << t_text;
    ofs.close();
}

/// Returns the address of the named global in t_mod, or nullptr.
int* findGlobalInt(asIScriptModule* t_mod, const std::string& t_name, bool& t_found) {
    for (asUINT i = 0; i < t_mod->GetGlobalVarCount(); ++i) {
        const char* p_name = nullptr;
        int type_id = 0;
        t_mod->GetGlobalVar(i, &p_name, nullptr, &type_id);
        if (p_name != nullptr && t_name == p_name) {
            t_found = true;
            return static_cast<int*>(t_mod->GetAddressOfGlobalVar(i));
        }
    }
    t_found = false;
    return nullptr;
}

void testGlobalAfterFunction() {
    const fs::path script = fs::temp_directory_path() / "sgrn_init_script_test.as";
    writeFile(script, "int before_var = 7;\n"
                      "\n"
                      "void helper() {\n"
                      "    int x = 1;\n"
                      "    x = x + 1;\n"
                      "}\n"
                      "\n"
                      "int after_var = 42;\n");

    asIScriptEngine* p_engine = asCreateScriptEngine(ANGELSCRIPT_VERSION);
    assert(p_engine != nullptr);
    asIScriptModule* p_mod = p_engine->GetModule("repl", asGM_ALWAYS_CREATE);
    assert(p_mod != nullptr);

    sgrn::s7shell::shell::loadInitScript(p_engine, p_mod, script.string());

    bool found = false;
    int* p_before = findGlobalInt(p_mod, "before_var", found);
    assert(found && p_before != nullptr && *p_before == 7 && "global before the function is missing");

    int* p_after = findGlobalInt(p_mod, "after_var", found);
    assert(found && p_after != nullptr && *p_after == 42 && "global after the function was swallowed by the function buffer");

    // Step 2 must still register the free function in the init_script module.
    asIScriptModule* p_init = p_engine->GetModule("init_script");
    assert(p_init != nullptr && p_init->GetFunctionCount() > 0 && "init_script module lost the function");

    p_engine->ShutDownAndRelease();

    std::error_code ec;
    fs::remove(script, ec);

    std::cout << "[init_script_test] global-after-function: OK\n";
}

} // namespace

int main() {
    testGlobalAfterFunction();
    std::cout << "[init_script_test] ALL OK\n";
    return 0;
}
