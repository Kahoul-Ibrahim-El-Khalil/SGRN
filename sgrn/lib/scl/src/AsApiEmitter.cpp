// =============================================================================
// AsApiEmitter.cpp — Generates declaration-only AngelScript .as files
// =============================================================================

#include <sgrn/scl/AsApiEmitter.hpp>
#include <sgrn/utils/strings.hpp>

#include <fmt/format.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace sgrn::scl
{

namespace fs = std::filesystem;

static const char* SHELL_API_TEMPLATE = R"(// s7shell_api.as — Built-in S7Shell API declarations for IDE / linting.
// Tooling-only: DO NOT load into AngelScript engine at runtime.

class PlcRuntime {
    PlcRuntime();
    PlcRuntime(const string &in schemaPath);
    void loadSclSchema(const string &in path);
    void loadJsonSchema(const string &in path);
    bool set(uint16 db, const string &in path, const string &in value);
    string get(uint16 db, const string &in path);
    void sync();
    void syncDb(uint16 db);
    string dirtySnapshot() const;
    bool hasDirty(uint16 db) const;
    bool defineTag(const string &in name, const string &in type, const string &in addr);
    string tagList() const;
    string tagList(const string &in table) const;
    string tagTables() const;
    string tagInfo(const string &in name) const;
    bool hasDirtyTags() const;
}

class S7Client {
    S7Client(const string &in ip, int rack = 0, int slot = 1, uint16 port = 102);
    S7Client(const string &in ip, int rack, int slot, uint16 port, PlcRuntime@ rt);
    bool isConnected() const;
    bool ping();
    void disconnect();
    bool reconnect();
    string lastError() const;
    int lastErrorCode() const;
    bool lastOpOk() const;
    void clearLastError();
    string read(const string &in address);
    bool write(const string &in address, const string &in hex);
}

class S7Server {
    S7Server(PlcRuntime@ rt, const string &in bindIp = "0.0.0.0", uint16 port = 102);
    void start();
    void stop();
    bool isRunning() const;
    int clientsCount() const;
    int getCpuStatus() const;
}

class Gateway {
    Gateway(PlcRuntime@ rt);
    void start(const string &in ip = "0.0.0.0", uint16 port = 8080);
    void stop();
    bool isRunning() const;
    void broadcast(const string &in json);
    void broadcast();
    void setAutoBroadcast(bool enable);
    bool autoBroadcast() const;
    void loadPolicy(const string &in path);
}

class HttpServer {
    HttpServer(PlcRuntime@ rt);
    void start(const string &in ip = "0.0.0.0", uint16 port = 8080);
    void stop();
    bool isRunning() const;
    void loadPolicy(const string &in path);
}

class WebSocketServer {
    WebSocketServer(PlcRuntime@ rt);
    void start(const string &in ip = "0.0.0.0", uint16 port = 9001);
    void stop();
    void broadcast(const string &in json);
    void broadcast();
    void setAutoBroadcast(bool enable);
    bool autoBroadcast() const;
    void loadPolicy(const string &in path);
}

class Persistence {
    Persistence(PlcRuntime@ rt);
    Persistence(PlcRuntime@ rt, const string &in outDir);
    void configure(const string &in outDir);
    void configure(const string &in outDir, const string &in format, const string &in mode);
    void start();
    void flush();
    void stop();
    bool isActive() const;
    string outDir() const;
}

class SimParams {
    uint64 seed;
    uint timestep_ms;
    uint duration_s;
    double noise_level;
    string fault;
}

class SimEngine {
    SimEngine(PlcRuntime@ rt, SimParams@ params);
    void run();
    void step();
}

class WalReplayer {
    WalReplayer(const string &in archivePath);
    void speed(double s);
    bool run();
}

class S7ProxySession {
    S7ProxySession(S7Client@ src, S7Client@ dst);
    void addMapping(uint16 srcDb, uint16 dstDb, uint intervalMs = 100, uint size = 0);
    void start();
    void stop();
}

class GatewayClient {
    GatewayClient(PlcRuntime@ rt);
    void subscribeDb(uint16 db);
    void unsubscribeDb(uint16 db);
    void publishOnDirty(bool enable);
    void useBinary(bool enable);
    bool connect(const string &in url);
    void sync();
    void disconnect();
    bool connected() const;
    string lastError() const;
}

class GatewaySync {
    GatewaySync(PlcRuntime@ rt);
    void subscribeDb(uint16 db);
    void unsubscribeDb(uint16 db);
    void publishOnDirty(bool enable);
    void useBinary(bool enable);
    bool connect(const string &in url);
    void sync();
    void disconnect();
    bool connected() const;
    string lastError() const;
}

class GatewayServer {
    GatewayServer(PlcRuntime@ rt);
    bool start(uint16 port = 8000);
    void stop();
    bool isRunning() const;
    void broadcast();
    string lastError() const;
}

void print(const string &in str);
void sleep(uint ms);
)";

static bool isStringType(DataType t) {
    return t == DataType::String || t == DataType::WString || t == DataType::XString || t == DataType::XWString;
}

static const char* scalarAsType(DataType type) {
    switch (type) {
        case DataType::Bool:
            return "bool";
        case DataType::Byte:
        case DataType::USInt:
            return "uint8";
        case DataType::SInt:
            return "int8";
        case DataType::Word:
        case DataType::UInt:
        case DataType::Date:
        case DataType::Counter:
        case DataType::Timer:
            return "uint16";
        case DataType::Int:
            return "int16";
        case DataType::DWord:
        case DataType::UDInt:
            return "uint";
        case DataType::DInt:
            return "int";
        case DataType::LWord:
        case DataType::ULInt:
            return "uint64";
        case DataType::LInt:
            return "int64";
        case DataType::Real:
            return "float";
        case DataType::LReal:
            return "double";
        case DataType::Char:
        case DataType::WChar:
        case DataType::String:
        case DataType::WString:
        case DataType::XString:
        case DataType::XWString:
            return "string";
        case DataType::Time:
            return "int";
        case DataType::LTime:
            return "int64";
        case DataType::TimeOfDay:
            return "uint";
        case DataType::LTimeOfDay:
            return "uint64";
        default:
            return "";
    }
}

static void emitMembers(std::ostringstream& t_out, const std::vector<DbField>& t_fields, const std::string& t_parent,
    const std::string& t_indent, std::ostringstream& t_nested) {
    for (const auto& f : t_fields) {
        const std::string name = sgrn::utils::strings::sanitizeIdentifier(f.name);
        const bool is_string = isStringType(f.type);
        if (f.count > 1 && !is_string) {
            if (!f.udt_name.empty()) {
                t_out << t_indent << "array<" << sgrn::utils::strings::sanitizeIdentifier(f.udt_name) << "@> " << name << ";\n";
            } else if (const char* elem = scalarAsType(f.type)) {
                t_out << t_indent << "array<" << elem << "> " << name << ";\n";
            } else {
                t_out << t_indent << "// NOTE: " << name << " omitted (runtime skips it too)\n";
            }
            continue;
        }
        if (!f.udt_name.empty() && f.type == DataType::Struct) {
            t_out << t_indent << sgrn::utils::strings::sanitizeIdentifier(f.udt_name) << "@ " << name << ";\n";
            continue;
        }
        if (!f.children.empty()) {
            const std::string nested = t_parent + "_" + name;
            std::ostringstream nested_body;
            emitMembers(nested_body, f.children, nested, "    ", t_nested);
            t_nested << "class " << nested << "\n{\n" << nested_body.str() << "}\n\n";
            t_out << t_indent << nested << "@ " << name << ";\n";
            continue;
        }
        if (!f.udt_name.empty()) {
            if (const char* prim = scalarAsType(f.type)) {
                t_out << t_indent << prim << " " << name << ";\n";
                continue;
            }
        }
        if (f.type == DataType::DTL || f.type == DataType::DateTime) {
            t_out << t_indent << "DTL@ " << name << ";\n";
            continue;
        }
        if (const char* prim = scalarAsType(f.type)) {
            t_out << t_indent << prim << " " << name << ";\n";
        } else {
            t_out << t_indent << "// NOTE: " << name << " omitted (runtime skips it too)\n";
        }
    }
}

static void emitClass(std::ostringstream& t_out, const std::string& t_name, const std::vector<DbField>& t_fields, bool t_db_methods) {
    std::ostringstream nested;
    std::ostringstream body;
    emitMembers(body, t_fields, t_name, "    ", nested);
    t_out << nested.str();
    t_out << "class " << t_name << "\n{\n" << body.str();
    if (t_db_methods) {
        t_out << "    void put();\n";
        t_out << "    " << t_name << "@ get();\n";
        t_out << "    void print() const;\n";
        t_out << "    string toJson() const;\n";
    }
    t_out << "}\n";
}

Result<void, std::string> AsApiEmitter::emit(const PlcSchemaStore& store, const AsEmitterOptions& opts) {
    // include_predefined and include_shell_api both wire the native API
    // surface into the output directory; emitting both would double-declare
    // it (PlcRuntime, Persistence, print, ...) and confuse IDE tooling.
    if (opts.include_predefined && opts.include_shell_api) {
        return Error(
            "--include-predefined and --include-shell-api are mutually exclusive: both declare the native API surface; pass only one.");
    }

    std::error_code ec;
    fs::create_directories(opts.output_dir, ec);
    if (ec) {
        return Error("Failed to create output directory: " + ec.message());
    }

    // Native s7shell API surface (PlcRuntime, S7Client, Persistence, ...).
    std::ostringstream api_out;
    api_out << SHELL_API_TEMPLATE << "\n";

    // Schema UDT declarations (non-scalar-alias types only; scalar aliases
    // resolve to primitives and need no class declarations).
    std::ostringstream udts_out;
    for (const auto& udt : store.udts()) {
        if (udt.is_scalar_alias)
            continue;
        emitClass(udts_out, sgrn::utils::strings::sanitizeIdentifier(udt.name), udt.fields, false);
        udts_out << "\n";
    }

    // DB classes + globals, mirroring exactly what the s7shell runtime
    // registers (SchemaVM) and injects for scripts (injectDbRefs /
    // buildDbPreamble): Db_<Name> class, bare schema-named global, generic db<N> global, and
    // the get_* accessors — the emitter and engine MUST agree on these.
    std::ostringstream dbs_out;
    std::ostringstream globals_out;
    for (const auto& [num, db] : store.dbs()) {
        const std::string name =
            sgrn::utils::strings::sanitizeIdentifier(db.db_name.empty() ? fmt::format("DB{}", db.db_number) : db.db_name);
        const std::string cls = "Db_" + name;
        emitClass(dbs_out, cls, db.fields, true);
        dbs_out << "\n";
        globals_out << cls << "@ " << name << ";\n";
        globals_out << cls << "@ db" << db.db_number << ";\n";
        globals_out << cls << "@ get_" << name << "();\n";
        globals_out << cls << "@ get_db" << db.db_number << "();\n";
    }

    std::string regenerate = "sclc emit-angelscript";
    if (opts.include_predefined)
        regenerate += " --include-predefined";
    else if (opts.include_shell_api)
        regenerate += " --include-shell-api";

    std::ostringstream out;
    out << "// AUTO-GENERATED by " << regenerate << ". Do not edit.\n";
    out << "// Tooling-only: never loaded by the AngelScript runtime.\n\n";

    // --include-predefined writes as.predefined: the single complete ambient
    // header (native API + schema UDT/DB classes + DB globals) that IDE
    // language servers can pick up. Default schema.as carries only the schema
    // surface; the API then lives in s7shell_api.as (legacy --include-shell-api).
    if (opts.include_predefined) {
        out << "// ---- native s7shell API ----\n\n";
        out << api_out.str() << "\n";
    }

    // Schema UDTs.
    if (!udts_out.str().empty()) {
        out << "// ---- schema UDTs ----\n\n";
        out << udts_out.str() << "\n";
    }

    // DB types + globals (bare declarations matching the runtime preamble).
    if (!dbs_out.str().empty()) {
        out << "// ---- DB types and globals ----\n\n";
        out << dbs_out.str() << "\n";
        out << globals_out.str();
    }

    const std::string filename = opts.include_predefined ? "as.predefined" : "schema.as";
    fs::path schema_path = fs::path(opts.output_dir) / filename;
    {
        std::ofstream ofs(schema_path);
        if (!ofs.is_open())
            return Error("Failed to open " + schema_path.string());
        ofs << out.str();
    }

    // Legacy companion: the bare native API surface (matches `s7shell emit-as`).
    if (opts.include_shell_api) {
        fs::path api_path = fs::path(opts.output_dir) / "s7shell_api.as";
        std::ofstream ofs(api_path);
        if (!ofs.is_open())
            return Error("Failed to open " + api_path.string());
        ofs << SHELL_API_TEMPLATE;
    }

    return {};
}

} // namespace sgrn::scl
