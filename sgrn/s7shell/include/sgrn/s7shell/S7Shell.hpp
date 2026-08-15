#pragma once

#include <sgrn/AngelScriptEngine.hpp>
#include <sgrn/Result.hpp>
#include <angelscript.h>
#include <chrono>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace sgrn::s7shell::shell
{

/**
 * @brief S7 AngelScript Shell for interactive and scripted PLC access
 *
 * Extends AngelScriptEngine with gateway-facing PLC APIs including:
 * - S7Client: Connection management and symbolic access
 * - DataBlock: Script-facing DB read/write wrapper
 * - TagTable: Tag-based access
 * - S7Memory: Low-level memory operations
 * - S7Diagnostics: PLC diagnostics
 * - S7PlcControl: PLC control operations
 * - And more...
 *
 * Usage:
 *   S7Shell shell;
 *   shell.run();           // Interactive REPL
 *   shell.runScript(file); // Execute script
 *   shell.printHelp();     // Print API documentation
 */

#include <chrono>
#include <sstream>

class S7Shell : public sgrn::scripting::AngelScriptEngine {
public:
    S7Shell();

    void runScript(const std::string& t_filename);
    void runScripts(const std::vector<std::string>& t_filenames);
    void printHelp() const;

    void loadSchema(const std::string& t_schema_path, const std::string& t_output_dir = "");

    /** Override: adds object auto-print to the base REPL evaluation chain. */
    void execute(std::string t_line) override;

protected:
    void onStart() override;
    std::string getPrompt() const override;
    void showHelp() const override;
    bool handleMetaCommand(const std::string& t_line) override;

private:
    void printTiming(const std::chrono::high_resolution_clock::time_point& t_start) const {
        if (!timing_)
            return;
        auto d = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - t_start);
        if (d.count() > 1000)
            fmt::print(fg(fmt::color::gray), "[{:.2f}ms]\n", d.count() / 1000.0);
        else
            fmt::print(fg(fmt::color::gray), "[{}us]\n", d.count());
    }

    /// Auto-generated DataBlock@ handle declarations, prepended to every runScript() compilation.
    /// Accumulated from all schema loads via injectDbRefs/buildDbPreamble.
    std::string db_preamble_;
    static char** completionDispatch(const char* tp_text, int t_start, int t_end);

    int last_shell_exit_code_;
    std::vector<std::string> completeTopLevel(const std::string& t_prefix) const;
    std::vector<std::string> completeMember(const std::string& t_object_chain, const std::string& t_member_prefix) const;
    static S7Shell* p_s_active_for_completion;
    void runShellCommand(const std::string& t_cmd);
};

/**
 * @brief Register all S7-specific APIs with the AngelScript engine
 * @param engine The AngelScript engine to register with
 * @return true if registration succeeded, false otherwise
 */
::sgrn::Result<void, std::string> registerS7Shell(asIScriptEngine* tp_engine);

void loadInitScript(asIScriptEngine* tp_engine, asIScriptModule* tp_repl_mod, const std::string& t_path = "./angelscript.as");

bool tryAutoPrint(asIScriptEngine* tp_engine, asIScriptModule* tp_mod, asIScriptContext* tp_ctx, const std::string& t_expr);

} // namespace sgrn::s7shell::shell
