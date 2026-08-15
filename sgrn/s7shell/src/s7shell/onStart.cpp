#include <sgrn/s7shell/S7Shell.hpp>
#include <vector>

namespace sgrn::s7shell::shell
{

void S7Shell::onStart() {
    fmt::print(fg(fmt::color::cyan), "S7 AngelScript Shell\n");
    fmt::print("Type 'help()' for commands, a path to run a script, or 'exit' to quit.\n");
    fmt::print(fg(fmt::color::gray), "Tip: entering a bare object expression prints it automatically.\n\n");

    p_s_active_for_completion = this;
    rl_attempted_completion_function = &S7Shell::completionDispatch;
    // Keep '.' out of the break set so "plc.db1.te<TAB>" arrives as one token
    static char breakers[] = " \t\n\"\\'`@$><=;|&{(";
    rl_completer_word_break_characters = breakers;
    rl_completion_append_character = '\0';
}
} // namespace sgrn::s7shell::shell
