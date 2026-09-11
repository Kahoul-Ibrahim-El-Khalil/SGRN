// datastore_shell — AngelScript-powered administration shell & CLI for the SGRN datastore API.
//
// Dual mode:
// 1. REPL / Interactive: `datastore_shell`
// 2. Script execution: `datastore_shell script.as`
// 3. One-shot command: `datastore_shell --url URL --token T ls /`

#include <sgrn/datastore/shell/DatastoreShell.hpp>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace
{

std::string envOr(const char* t_name) {
    const char* v = std::getenv(t_name);
    return v ? std::string(v) : std::string{};
}

} // namespace

int main(int t_argc, char** tp_argv) {
    sgrn::datastore_shell::ConnDefaults def;
    def.url = envOr("SGRN_DATASTORE_URL");
    def.token = envOr("SGRN_DATASTORE_TOKEN");
    def.secret = envOr("SGRN_DATASTORE_SECRET");
    def.email = envOr("SGRN_DATASTORE_EMAIL");
    def.password = envOr("SGRN_DATASTORE_PASSWORD");
    def.session_token = envOr("SGRN_DATASTORE_SESSION_TOKEN");

    std::vector<std::string> words;
    std::string script_file;

    for (int i = 1; i < t_argc; ++i) {
        const std::string a = tp_argv[i];
        auto take_value = [&](const char* t_flag, std::string& t_out, size_t t_len) {
            if (a == t_flag && i + 1 < t_argc) {
                t_out = tp_argv[++i];
                return true;
            }
            if (a.rfind(t_flag, 0) == 0 && a.size() > t_len && a[t_len] == '=') {
                t_out = a.substr(t_len + 1);
                return true;
            }
            return false;
        };

        if (a == "-h" || a == "--help") {
            sgrn::datastore_shell::DatastoreShell sh;
            sh.showHelp();
            return 0;
        } else if (take_value("--url", def.url, 5) || take_value("--token", def.token, 7) || take_value("--secret", def.secret, 8) ||
                   take_value("--email", def.email, 7) || take_value("--password", def.password, 10) ||
                   take_value("--session-token", def.session_token, 15)) {
            continue;
        } else {
            if (script_file.empty() && a.size() > 3 && a.substr(a.size() - 3) == ".as") {
                script_file = a;
            } else {
                words.push_back(a);
            }
        }
    }

    sgrn::datastore_shell::DatastoreShell shell;
    shell.setDefaults(def);

    if (!script_file.empty()) {
        shell.autoConnect();
        shell.runScript(script_file);
        return 0;
    }

    if (!words.empty()) {
        return shell.runOneShot(words);
    }

    // Interactive mode
    shell.autoConnect();
    shell.run();
    return 0;
}
