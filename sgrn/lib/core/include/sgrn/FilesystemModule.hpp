#pragma once
// FilesystemModule.hpp — AngelScript filesystem bindings for shell environments
// Provides professional-grade file I/O, directory operations, and shell
// execution capabilities to AngelScript scripts.
// =============================================================================

#include <fmt/core.h>
#include <sgrn/debug.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <angelscript.h>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace sgrn::scripting
{
namespace fs = std::filesystem;
using namespace sgrn::utils::filesystem;
// ---------------------------------------------------------------------------
// File object — provides persistent file handle management
// ---------------------------------------------------------------------------
class FileHandle {
public:
    enum mode_ { Read = 0, Write = 1, Append = 2 };

    FileHandle(const std::string& t_path, int t_mode)
        : path_(t_path)
        , mode_(t_mode) {
        open();
    }

    ~FileHandle() {
        close();
    }

    bool is_open() const {
        return file_.is_open();
    }

    std::string readLine() {
        std::string t_line;
        if (file_.is_open() && std::getline(file_, t_line)) {
            return t_line;
        }
        return std::string("");
    }

    std::string readAll() {
        if (!file_.is_open()) {
            return std::string("");
        }
        std::stringstream buffer;
        buffer << file_.rdbuf();
        return buffer.str();
    }

    void writeLine(const std::string& t_line) {
        if (file_.is_open()) {
            file_ << t_line << '\n';
            file_.flush();
        }
    }

    void write(const std::string& t_data) {
        if (file_.is_open()) {
            file_ << t_data;
            file_.flush();
        }
    }

    void close() {
        if (file_.is_open()) {
            file_.close();
        }
    }

    const std::string& path() const {
        return path_;
    }

private:
    void open() {
        switch (mode_) {
            case Read:
                file_.open(path_, std::ios::in);
                break;
            case Write:
                file_.open(path_, std::ios::out | std::ios::trunc);
                break;
            case Append:
                file_.open(path_, std::ios::out | std::ios::app);
                break;
        }
    }

    std::string path_;
    int mode_;
    std::fstream file_;
};

// ---------------------------------------------------------------------------
// FilesystemModule — AngelScript API functions
// ---------------------------------------------------------------------------
namespace fs_impl
{

// File I/O
inline std::string readFile(const std::string& t_path) {
    std::ifstream file_(expandUserPath(t_path));
    if (!file_.is_open())
        return "";
    std::stringstream buffer;
    buffer << file_.rdbuf();
    return buffer.str();
}

inline bool writeFile(const std::string& t_path, const std::string& t_content) {
    std::ofstream file_(expandUserPath(t_path), std::ios::out | std::ios::trunc);
    if (!file_.is_open())
        return false;
    file_ << t_content;
    file_.close();
    return true;
}

inline bool appendFile(const std::string& t_path, const std::string& t_content) {
    std::ofstream file_(expandUserPath(t_path), std::ios::out | std::ios::app);
    if (!file_.is_open())
        return false;
    file_ << t_content;
    file_.close();
    return true;
}

inline FileHandle* openFile(const std::string& t_path, const std::string& t_mode) {
    int fmode = FileHandle::Read;
    std::string expanded = expandUserPath(t_path);
    if (t_mode == "r")
        fmode = FileHandle::Read;
    else if (t_mode == "w")
        fmode = FileHandle::Write;
    else if (t_mode == "a")
        fmode = FileHandle::Append;

    auto* p_handle = new FileHandle(expanded, fmode);
    if (!p_handle->is_open()) {
        delete p_handle;
        return nullptr;
    }
    return p_handle;
}

// Directory operations
inline bool exists(const std::string& t_path) {
    return fs::exists(expandUserPath(t_path));
}

inline bool isFile(const std::string& t_path) {
    return fs::is_regular_file(expandUserPath(t_path));
}

inline bool is_directory(const std::string& t_path) {
    return fs::is_directory(expandUserPath(t_path));
}

inline bool mkdir(const std::string& t_path) {
    try {
        return fs::create_directories(expandUserPath(t_path));
    } catch (const std::exception& e) {
        SGRN_WARN_LOG("Filesystem", "mkdir failed for {}: {}", t_path, e.what());
        return false;
    }
}

inline bool remove(const std::string& t_path) {
    try {
        return fs::remove(expandUserPath(t_path)) > 0;
    } catch (const std::exception& e) {
        SGRN_WARN_LOG("Filesystem", "remove failed for {}: {}", t_path, e.what());
        return false;
    }
}

inline bool remove_all(const std::string& t_path) {
    try {
        return fs::remove_all(expandUserPath(t_path)) > 0;
    } catch (const std::exception& e) {
        SGRN_WARN_LOG("Filesystem", "remove_all failed for {}: {}", t_path, e.what());
        return false;
    }
}

inline std::string absolutePath(const std::string& t_path) {
    try {
        return fs::absolute(expandUserPath(t_path)).string();
    } catch (const std::exception& e) {
        SGRN_WARN_LOG("Filesystem", "absolutePath failed for {}: {}", t_path, e.what());
        return t_path;
    }
}

inline std::string current_path() {
    try {
        return fs::current_path().string();
    } catch (const std::exception& e) {
        SGRN_WARN_LOG("Filesystem", "current_path failed: {}", e.what());
        return "";
    }
}

inline bool changeDirectory(const std::string& t_path) {
    try {
        fs::current_path(expandUserPath(t_path));
        return true;
    } catch (const std::exception& e) {
        SGRN_WARN_LOG("Filesystem", "changeDirectory failed for {}: {}", t_path, e.what());
        return false;
    }
}

// Environment variables
inline std::string getenv(const std::string& t_name) {
    const char* p_val = std::getenv(t_name.c_str());
    return p_val ? std::string(p_val) : "";
}

inline bool setenv(const std::string& t_name, const std::string& t_value) {
#ifdef _WIN32
    return _putenv_s(t_name.c_str(), t_value.c_str()) == 0;
#else
    return ::setenv(t_name.c_str(), t_value.c_str(), 1) == 0;
#endif
}

// Shell execution (with output capture)
inline std::string exec(const std::string& t_command) {
    std::array<char, 128> buffer;
    std::string result;
#ifdef _WIN32
    FILE* p_pipe = _popen(t_command.c_str(), "r");
#else
    FILE* p_pipe = popen(t_command.c_str(), "r");
#endif
    if (!p_pipe)
        return "";
    while (fgets(buffer.data(), buffer.size(), p_pipe) != nullptr) {
        result += buffer.data();
    }
#ifdef _WIN32
    _pclose(p_pipe);
#else
    pclose(p_pipe);
#endif
    return result;
}

// Glob pattern matching
inline std::vector<std::string> glob(const std::string& t_directory, const std::string& t_pattern) {
    std::vector<std::string> results;
    try {
        std::string expanded = expandUserPath(t_directory);
        if (!fs::exists(expanded))
            return results;

        for (const auto& entry : fs::directory_iterator(expanded)) {
            if (t_pattern == "*" || entry.path().filename().string().find(t_pattern) != std::string::npos) {
                results.push_back(entry.path().string());
            }
        }
    } catch (const std::exception& e) {
        SGRN_WARN_LOG("Filesystem", "glob failed for {}: {}", t_directory, e.what());
    }
    return results;
}

inline std::string ls(const std::string& t_path) {
    std::string out;
    try {
        std::string expanded = expandUserPath(t_path);
        fs::path p = expanded.empty() ? fs::current_path() : fs::path(expanded);
        if (!fs::exists(p))
            return "SchemaError: Path does not exist\n";

        for (const auto& entry : fs::directory_iterator(p)) {
            const char kind = entry.is_directory() ? 'd' : '-';
            out += fmt::format("{} {}\n", kind, entry.path().filename().string());
        }
    } catch (const std::exception& e) {
        out = fmt::format("SchemaError: {}\n", e.what());
    }
    return out;
}

inline void printLs(const std::string& t_path) {
    fmt::print("{}", ls(t_path));
}
inline void printPwd() {
    fmt::print("{}\n", current_path());
}
inline void printAbs(const std::string& t_path) {
    fmt::print("{}\n", absolutePath(t_path));
}
inline void printReadFile(const std::string& t_path) {
    fmt::print("{}", readFile(t_path));
}

} // namespace fs_impl

// ---------------------------------------------------------------------------
// AngelScript Registration
// ---------------------------------------------------------------------------

/**
 * @brief Register filesystem functions with AngelScript engine
 * @param engine The AngelScript engine to register with
 */
inline void registerFilesystemModule(asIScriptEngine* tp_engine) {
    // File I/O functions

    tp_engine->RegisterGlobalFunction("bool writeFile(const string &in, const string &in)", asFUNCTION(fs_impl::writeFile), asCALL_CDECL);

    tp_engine->RegisterGlobalFunction("bool appendFile(const string &in, const string &in)", asFUNCTION(fs_impl::appendFile), asCALL_CDECL);

    // Directory operations
    tp_engine->RegisterGlobalFunction("bool exists(const string &in)", asFUNCTION(fs_impl::exists), asCALL_CDECL);
    tp_engine->RegisterGlobalFunction("bool isfile(const string &in)", asFUNCTION(fs_impl::isFile), asCALL_CDECL);
    tp_engine->RegisterGlobalFunction("bool isdir(const string &in)", asFUNCTION(fs_impl::is_directory), asCALL_CDECL);
    tp_engine->RegisterGlobalFunction("bool mkdir(const string &in)", asFUNCTION(fs_impl::mkdir), asCALL_CDECL);
    tp_engine->RegisterGlobalFunction("bool remove(const string &in)", asFUNCTION(fs_impl::remove), asCALL_CDECL);
    tp_engine->RegisterGlobalFunction("bool remove_all(const string &in)", asFUNCTION(fs_impl::remove_all), asCALL_CDECL);

    tp_engine->RegisterGlobalFunction("string absStr(const string &in)", asFUNCTION(fs_impl::absolutePath), asCALL_CDECL);
    tp_engine->RegisterGlobalFunction("void abs(const string &in)", asFUNCTION(fs_impl::printAbs), asCALL_CDECL);

    tp_engine->RegisterGlobalFunction("string pwdStr()", asFUNCTION(fs_impl::current_path), asCALL_CDECL);
    tp_engine->RegisterGlobalFunction("void pwd()", asFUNCTION(fs_impl::printPwd), asCALL_CDECL);

    tp_engine->RegisterGlobalFunction("bool cd(const string &in)", asFUNCTION(fs_impl::changeDirectory), asCALL_CDECL);

    tp_engine->RegisterGlobalFunction("string lsStr(const string &in = \"\")", asFUNCTION(fs_impl::ls), asCALL_CDECL);
    tp_engine->RegisterGlobalFunction("void ls(const string &in = \"\")", asFUNCTION(fs_impl::printLs), asCALL_CDECL);

    tp_engine->RegisterGlobalFunction("string readFileStr(const string &in)", asFUNCTION(fs_impl::readFile), asCALL_CDECL);
    tp_engine->RegisterGlobalFunction("void readFile(const string &in)", asFUNCTION(fs_impl::printReadFile), asCALL_CDECL);
}

} // namespace sgrn::scripting
