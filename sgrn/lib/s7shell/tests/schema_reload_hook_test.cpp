// Schema-reload hook test: registerS7Shell() must arm plcsim's
// g_on_schema_loaded, and a subsequent PlcRuntime::loadSclSchema() must
// re-register the new DB types on the engine through it.
//
// Background: PlcRuntime used to re-register inline (it could see the
// s7shell script layer). After the plcsim extraction the runtime only
// invokes this hook; the s7shell side arms it once next to p_g_as_engine.
// If either half regresses, freshly loaded DB types stay invisible to
// scripts even though the twin itself loaded fine.

#include <sgrn/plcsim/runtime/PlcRuntime.hpp>
#include <sgrn/s7shell/bindings/registration.hpp>

#include <angelscript.h>
#include <scriptarray/scriptarray.h>
#include <scriptdictionary/scriptdictionary.h>
#include <scriptstdstring/scriptstdstring.h>

#include <cassert>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

int main() {
    // Hook starts disarmed (headless use): loading must not crash and must
    // not touch any engine.
    assert(!static_cast<bool>(::sgrn::plcsim::runtime::g_on_schema_loaded));
    {
        ::sgrn::plcsim::runtime::PlcRuntime rt;
        rt.loadSclSchema((fs::path(__FILE__).parent_path() / ".." / "simulations" / "schema.scl").string());
    }

    asIScriptEngine* p_engine = asCreateScriptEngine(ANGELSCRIPT_VERSION);
    assert(p_engine != nullptr);
    // registerS7Shell() declares methods using `string` (e.g.
    // HexTable::toString) and `array<T>`/`dictionary` (FieldProxy structured
    // assignment) — production engines get them from ScriptHost, so a bare
    // test engine must register the addon types first.
    RegisterStdString(p_engine);
    RegisterScriptArray(p_engine, true);
    RegisterScriptDictionary(p_engine);
    auto reg = ::sgrn::s7shell::shell::registerS7Shell(p_engine);
    assert(!reg.hasError());

    // Arming is observable directly on the hook.
    assert(static_cast<bool>(::sgrn::plcsim::runtime::g_on_schema_loaded));

    const auto types_before = p_engine->GetObjectTypeCount();
    {
        ::sgrn::plcsim::runtime::PlcRuntime rt;
        rt.loadSclSchema((fs::path(__FILE__).parent_path() / ".." / "simulations" / "schema.scl").string());
    }
    const auto types_after = p_engine->GetObjectTypeCount();
    assert(types_after > types_before && "schema reload did not register new DB types on the engine");
    (void)types_before; // assert-only use (compiled out with NDEBUG)
    (void)types_after;  // assert-only use (compiled out with NDEBUG)

    p_engine->ShutDownAndRelease();
    std::cout << "[schema_reload_hook_test] ALL OK\n";
    return 0;
}
