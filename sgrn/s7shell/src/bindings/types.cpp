#include <fmt/color.h>
#include <fmt/format.h>
#include <sgrn/s7shell/SchemaVM.hpp>
#include <sgrn/s7shell/bindings/registration.hpp>
#include <sgrn/s7shell/connection/S7Connection.hpp>
#include <sgrn/s7shell/facades/ScriptAsync.hpp>
#include <sgrn/s7shell/facades/ScriptBlocks.hpp>
#include <sgrn/s7shell/facades/ScriptConnectionProxy.hpp>
#include <sgrn/s7shell/facades/ScriptDiagnostics.hpp>
#include <sgrn/s7shell/facades/ScriptMemory.hpp>
#include <sgrn/s7shell/facades/ScriptPlcControl.hpp>
#include <sgrn/s7shell/script/ScriptDataBlock.hpp>
#include <sgrn/s7shell/script/ScriptFieldProxy.hpp>
#include <sgrn/s7shell/script/ScriptHexTable.hpp>
#include <sgrn/s7shell/script/ScriptPathBatch.hpp>
#include <sgrn/s7shell/script/ScriptSchemaStore.hpp>
#include <sgrn/s7shell/script/ScriptTagTable.hpp>
#include <sgrn/s7shell/utils/PlcSimClock.hpp>
#include <angelscript.h>
#include <ctime>
#include <scriptarray/scriptarray.h>
#include <scriptdictionary/scriptdictionary.h>
#include <snap7.h>

namespace sgrn::s7shell::shell
{

extern ScriptHexTable* DataBlockToHexTableCast(ScriptDataBlock* tp_db);

::sgrn::Result<void, std::string> registerS7Types(asIScriptEngine* tp_engine) {
    int r = 0;

    // Siemens-native type aliases (Comprehensive)
    SGRN_AS_REG(tp_engine->RegisterTypedef("BOOL", "bool"));
    SGRN_AS_REG(tp_engine->RegisterTypedef("SINT", "int8"));
    SGRN_AS_REG(tp_engine->RegisterTypedef("USINT", "uint8"));
    SGRN_AS_REG(tp_engine->RegisterTypedef("BYTE", "uint8"));
    SGRN_AS_REG(tp_engine->RegisterTypedef("INT", "int16"));
    SGRN_AS_REG(tp_engine->RegisterTypedef("UINT", "uint16"));
    SGRN_AS_REG(tp_engine->RegisterTypedef("WORD", "uint16"));
    SGRN_AS_REG(tp_engine->RegisterTypedef("DINT", "int"));
    SGRN_AS_REG(tp_engine->RegisterTypedef("UDINT", "uint"));
    SGRN_AS_REG(tp_engine->RegisterTypedef("DWORD", "uint"));
    SGRN_AS_REG(tp_engine->RegisterTypedef("LINT", "int64"));
    SGRN_AS_REG(tp_engine->RegisterTypedef("ULINT", "uint64"));
    SGRN_AS_REG(tp_engine->RegisterTypedef("LWORD", "uint64"));
    SGRN_AS_REG(tp_engine->RegisterTypedef("REAL", "float"));
    SGRN_AS_REG(tp_engine->RegisterTypedef("LREAL", "double"));

    // Temporal aliases (mapped to underlying numeric storage for now)
    SGRN_AS_REG(tp_engine->RegisterTypedef("TIME", "int"));    // ms
    SGRN_AS_REG(tp_engine->RegisterTypedef("LTIME", "int64")); // ns
    SGRN_AS_REG(tp_engine->RegisterTypedef("DATE", "uint16")); // days
    SGRN_AS_REG(tp_engine->RegisterTypedef("TOD", "uint"));    // ms
    SGRN_AS_REG(tp_engine->RegisterTypedef("LTOD", "uint64")); // ns

    // Extra print overloads for ergonomics
    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("void print(float)", asFUNCTION(printFloat), asCALL_CDECL));
    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("void print(double)", asFUNCTION(printDouble), asCALL_CDECL));
    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("void print(int)", asFUNCTION(printInt), asCALL_CDECL));
    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("void print(uint)", asFUNCTION(printUInt), asCALL_CDECL));
    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("void print(bool)", asFUNCTION(printBool), asCALL_CDECL));
    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("void print(int64)", asFUNCTION(printInt64), asCALL_CDECL));
    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("void print(uint64)", asFUNCTION(printUInt64), asCALL_CDECL));

    // Forward-register auxiliary types for S7Client method signatures
    SGRN_AS_TYPE(tp_engine, "S7Diagnostics");
    SGRN_AS_TYPE(tp_engine, "S7PlcControl");
    SGRN_AS_TYPE(tp_engine, "S7Memory");
    SGRN_AS_TYPE(tp_engine, "S7Blocks");
    SGRN_AS_TYPE(tp_engine, "S7SchemaStore");
    SGRN_AS_REFCOUNTED(tp_engine, "S7SchemaStore", ScriptSchemaStore);
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7SchemaStore", "void print()", asMETHOD(ScriptSchemaStore, print), asCALL_THISCALL));
    SGRN_AS_TYPE(tp_engine, "S7Connection");
    SGRN_AS_TYPE(tp_engine, "S7Async");

    // ── HexTable Type Registration ───────────────────────────────────────
    SGRN_AS_TYPE(tp_engine, "HexTable");
    SGRN_AS_REFCOUNTED(tp_engine, "HexTable", ScriptHexTable);
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("HexTable", "void print() const", asMETHOD(ScriptHexTable, print), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("HexTable", "string toString() const", asMETHOD(ScriptHexTable, toString), asCALL_THISCALL));

    SGRN_AS_TYPE(tp_engine, "S7PathBatch");
    SGRN_AS_REFCOUNTED(tp_engine, "S7PathBatch", S7PathBatch);
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7PathBatch", "S7PathBatch@ write(const string &in)",
        asMETHODPR(S7PathBatch, write, (const std::string&), S7PathBatch*), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7PathBatch", "S7PathBatch@ write(double)", asMETHOD(S7PathBatch, writeDouble), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7PathBatch", "S7PathBatch@ write(int)", asMETHOD(S7PathBatch, writeInt), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7PathBatch", "S7PathBatch@ write(bool)", asMETHOD(S7PathBatch, writeBool), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7PathBatch", "S7PathBatch@ write(dictionary@)", asMETHOD(S7PathBatch, writeDict), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7PathBatch", "S7PathBatch@ write(array<int>@)", asMETHOD(S7PathBatch, writeArray), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7PathBatch", "S7PathBatch@ write(array<double>@)", asMETHOD(S7PathBatch, writeArray), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7PathBatch", "S7PathBatch@ write(array<bool>@)", asMETHOD(S7PathBatch, writeArray), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7PathBatch", "S7PathBatch@ write(array<dictionary@>@)", asMETHOD(S7PathBatch, writeArray), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7PathBatch", "string read() const", asMETHOD(S7PathBatch, read), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7PathBatch", "void put()", asMETHOD(S7PathBatch, put), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7PathBatch", "void get()", asMETHOD(S7PathBatch, get), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7PathBatch", "string toJson() const", asMETHOD(S7PathBatch, toJson), asCALL_THISCALL));

    // ── DTL type registration (before FieldProxy signatures) ───────────
    SGRN_AS_TYPE(tp_engine, "DTL");
    SGRN_AS_REFCOUNTED(tp_engine, "DTL", ScriptDtl);
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DTL", "DTL& opAssign(const DTL &in)", asMETHODPR(ScriptDtl, operator=, (const ScriptDtl&), ScriptDtl&), asCALL_THISCALL));

    // ── FieldProxy type registration ───────────
    SGRN_AS_TYPE(tp_engine, "FieldProxy");
    SGRN_AS_REFCOUNTED(tp_engine, "FieldProxy", ScriptFieldProxy);

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "FieldProxy", "FieldProxy& opAssign(float)", asMETHOD(ScriptFieldProxy, assignFloat), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "FieldProxy", "FieldProxy& opAssign(double)", asMETHOD(ScriptFieldProxy, assignDouble), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("FieldProxy", "FieldProxy& opAssign(int)", asMETHOD(ScriptFieldProxy, assignInt), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "FieldProxy", "FieldProxy& opAssign(uint)", asMETHOD(ScriptFieldProxy, assignUInt), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "FieldProxy", "FieldProxy& opAssign(int64)", asMETHOD(ScriptFieldProxy, assignInt64), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "FieldProxy", "FieldProxy& opAssign(uint64)", asMETHOD(ScriptFieldProxy, assignUInt64), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "FieldProxy", "FieldProxy& opAssign(bool)", asMETHOD(ScriptFieldProxy, assignBool), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "FieldProxy", "FieldProxy& opAssign(const string &in)", asMETHOD(ScriptFieldProxy, assignString), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "FieldProxy", "FieldProxy& opAssign(DTL@)", asMETHOD(ScriptFieldProxy, assignDtl), asCALL_THISCALL));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("FieldProxy", "float opCast() const", asMETHOD(ScriptFieldProxy, toFloat), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("FieldProxy", "double opCast() const", asMETHOD(ScriptFieldProxy, toDouble), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("FieldProxy", "int opCast() const", asMETHOD(ScriptFieldProxy, toInt), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("FieldProxy", "uint opCast() const", asMETHOD(ScriptFieldProxy, toUInt), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("FieldProxy", "int64 opCast() const", asMETHOD(ScriptFieldProxy, toInt64), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("FieldProxy", "uint64 opCast() const", asMETHOD(ScriptFieldProxy, toUInt64), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("FieldProxy", "bool opCast() const", asMETHOD(ScriptFieldProxy, toBool), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("FieldProxy", "string opCast() const", asMETHOD(ScriptFieldProxy, toString), asCALL_THISCALL));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("FieldProxy", "float opImplCast() const", asMETHOD(ScriptFieldProxy, toFloat), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("FieldProxy", "double opImplCast() const", asMETHOD(ScriptFieldProxy, toDouble), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("FieldProxy", "int opImplCast() const", asMETHOD(ScriptFieldProxy, toInt), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("FieldProxy", "uint opImplCast() const", asMETHOD(ScriptFieldProxy, toUInt), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("FieldProxy", "int64 opImplCast() const", asMETHOD(ScriptFieldProxy, toInt64), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("FieldProxy", "uint64 opImplCast() const", asMETHOD(ScriptFieldProxy, toUInt64), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("FieldProxy", "bool opImplCast() const", asMETHOD(ScriptFieldProxy, toBool), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("FieldProxy", "string opImplCast() const", asMETHOD(ScriptFieldProxy, toString), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("FieldProxy", "void print() const", asMETHOD(ScriptFieldProxy, print), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "FieldProxy", "FieldProxy@ opIndex(const string &in)", asMETHOD(ScriptFieldProxy, index), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("FieldProxy", "FieldProxy@ opIndex(int)", asMETHOD(ScriptFieldProxy, indexInt), asCALL_THISCALL));

    // ── DataBlock type registration ───────────
    SGRN_AS_TYPE(tp_engine, "DataBlock");
    SGRN_AS_REFCOUNTED(tp_engine, "DataBlock", ScriptDataBlock);

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("DataBlock", "string val(const string &in)", asMETHOD(ScriptDataBlock, val), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("DataBlock", "string read(const string &in)", asMETHOD(ScriptDataBlock, val), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("DataBlock", "HexTable@ hex()", asFUNCTION(DataBlockToHexTableCast), asCALL_CDECL_OBJFIRST));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "void setVal(const string &in, const string &in)", asMETHOD(ScriptDataBlock, setVal), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "string get(const string &in)", asMETHODPR(ScriptDataBlock, get, (const std::string&), std::string), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "double getReal(const string &in)", asMETHOD(ScriptDataBlock, getReal), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("DataBlock", "int getInt(const string &in)", asMETHOD(ScriptDataBlock, getInt), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "bool getBool(const string &in)", asMETHOD(ScriptDataBlock, getBool), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("DataBlock", "void write(const string &in, const string &in)",
        asMETHODPR(ScriptDataBlock, write, (const std::string&, const std::string&), void), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "void write(const string &in, double)", asMETHOD(ScriptDataBlock, writeDouble), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "void write(const string &in, int)", asMETHOD(ScriptDataBlock, writeInt), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "void write(const string &in, bool)", asMETHOD(ScriptDataBlock, writeBool), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "void write(const string &in, dictionary@)", asMETHOD(ScriptDataBlock, writeDict), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "void write(const string &in, array<int>@)", asMETHOD(ScriptDataBlock, writeArray), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "void write(const string &in, DTL@)", asMETHOD(ScriptDataBlock, writeDtl), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("DataBlock", "void put()", asMETHODPR(ScriptDataBlock, put, (), void), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("DataBlock", "void put(const string &in, const string &in)",
        asMETHODPR(ScriptDataBlock, put, (const std::string&, const std::string&), void), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "void put(const string &in, double)", asMETHOD(ScriptDataBlock, putDouble), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "void put(const string &in, int)", asMETHOD(ScriptDataBlock, putInt), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "void put(const string &in, bool)", asMETHOD(ScriptDataBlock, putBool), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "void put(const string &in, DTL@)", asMETHOD(ScriptDataBlock, putDtl), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "DataBlock@ get()", asMETHODPR(ScriptDataBlock, get, (), ScriptDataBlock*), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "DataBlock@ get(uint)", asMETHODPR(ScriptDataBlock, get, (size_t), ScriptDataBlock*), asCALL_THISCALL));
    // void push() removed — use put() which calls push() internally
    // ── Error introspection ───────────────────────────────────────────────
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("DataBlock", "bool lastOpOk() const", asMETHOD(ScriptDataBlock, getLastOpOk), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "string lastOpError() const", asMETHOD(ScriptDataBlock, getlastOpError), asCALL_THISCALL));

    // ── Retry variants ────────────────────────────────────────────────────
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "string getRetry(const string &in, int maxRetries = 3)", asMETHOD(ScriptDataBlock, getRetry), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("DataBlock", "bool putRetry(const string &in, const string &in, int maxRetries = 3)",
        asMETHOD(ScriptDataBlock, putRetry), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("DataBlock", "bool putRetry(const string &in, double, int maxRetries = 3)",
        asMETHOD(ScriptDataBlock, putRetryDouble), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "bool putRetry(const string &in, int, int maxRetries = 3)", asMETHOD(ScriptDataBlock, putRetryInt), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("DataBlock", "bool putRetry(const string &in, bool, int maxRetries = 3)",
        asMETHOD(ScriptDataBlock, putRetryBool), asCALL_THISCALL));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("DataBlock", "uint16 number() const", asMETHOD(ScriptDataBlock, getDbNumber), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("DataBlock", "string name() const", asMETHOD(ScriptDataBlock, getDbName), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("DataBlock", "string toJson() const", asMETHOD(ScriptDataBlock, toJson), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("DataBlock", "string diff() const", asMETHOD(ScriptDataBlock, diff), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("DataBlock", "void print() const", asMETHOD(ScriptDataBlock, print), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "FieldProxy@ opIndex(const string &in)", asMETHOD(ScriptDataBlock, opIndex), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "DataBlock", "S7PathBatch@ path(const string &in)", asMETHOD(ScriptDataBlock, getPath), asCALL_THISCALL));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("DataBlock", "void registerSize(uint)", asMETHOD(ScriptDataBlock, registerSize), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("DataBlock", "void addField(const string &in, const string &in, uint, uint16 = 1)",
        asMETHOD(ScriptDataBlock, addField), asCALL_THISCALL));

    // ── TagTable type registration ───────────
    SGRN_AS_TYPE(tp_engine, "TagTable");
    SGRN_AS_REFCOUNTED(tp_engine, "TagTable", ScriptTagTable);

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("TagTable", "string val(const string &in)", asMETHOD(ScriptTagTable, getVal), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("TagTable", "string read(const string &in)", asMETHOD(ScriptTagTable, getVal), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "TagTable", "void setVal(const string &in, const string &in)", asMETHOD(ScriptTagTable, setVal), asCALL_THISCALL));

    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("TagTable", "string get(const string &in)", asMETHOD(ScriptTagTable, get), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "TagTable", "double getReal(const string &in)", asMETHOD(ScriptTagTable, getReal), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("TagTable", "int getInt(const string &in)", asMETHOD(ScriptTagTable, getInt), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("TagTable", "bool getBool(const string &in)", asMETHOD(ScriptTagTable, getBool), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("TagTable", "void put()", asMETHODPR(ScriptTagTable, put, (), void), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("TagTable", "void put(const string &in, const string &in)",
        asMETHODPR(ScriptTagTable, put, (const std::string&, const std::string&), void), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("TagTable", "void put(const string &in, double)",
        asMETHODPR(ScriptTagTable, put, (const std::string&, double), void), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("TagTable", "void put(const string &in, int)",
        asMETHODPR(ScriptTagTable, put, (const std::string&, int32_t), void), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("TagTable", "void put(const string &in, bool)",
        asMETHODPR(ScriptTagTable, put, (const std::string&, bool), void), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("TagTable", "void write(const string &in, const string &in)",
        asMETHODPR(ScriptTagTable, write, (const std::string&, const std::string&), void), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("TagTable", "void write(const string &in, double)",
        asMETHODPR(ScriptTagTable, write, (const std::string&, double), void), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("TagTable", "void write(const string &in, int)",
        asMETHODPR(ScriptTagTable, write, (const std::string&, int32_t), void), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("TagTable", "void write(const string &in, bool)",
        asMETHODPR(ScriptTagTable, write, (const std::string&, bool), void), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("TagTable", "void write(const string &in, dictionary@)",
        asMETHODPR(ScriptTagTable, write, (const std::string&, CScriptDictionary*), void), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("TagTable", "void write(const string &in, array<int>@)",
        asMETHODPR(ScriptTagTable, write, (const std::string&, CScriptArray*), void), asCALL_THISCALL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("TagTable", "void get()", asMETHOD(ScriptTagTable, get), asCALL_THISCALL));
    // void push() removed — use put() which calls push() internally
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "TagTable", "S7PathBatch@ path(const string &in)", asMETHOD(ScriptTagTable, getPath), asCALL_THISCALL));

    // ── Error introspection ───────────────────────────────────────────────
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("TagTable", "bool lastOpOk() const", asMETHOD(ScriptTagTable, getLastOpOk), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("TagTable", "string lastOpError() const", asMETHOD(ScriptTagTable, lastOpError), asCALL_THISCALL));

    // ── Retry variants ───────────────────────────────────────────────────
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "TagTable", "string getRetry(const string &in, int = 3)", asMETHOD(ScriptTagTable, getRetry), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "TagTable", "bool putRetry(const string &in, const string &in, int = 3)", asMETHOD(ScriptTagTable, putRetry), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "TagTable", "bool putRetry(const string &in, double, int = 3)", asMETHOD(ScriptTagTable, putRetryDouble), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "TagTable", "bool putRetry(const string &in, int, int = 3)", asMETHOD(ScriptTagTable, putRetryInt), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "TagTable", "bool putRetry(const string &in, bool, int = 3)", asMETHOD(ScriptTagTable, putRetryBool), asCALL_THISCALL));

    return {};
}
} // namespace sgrn::s7shell::shell
