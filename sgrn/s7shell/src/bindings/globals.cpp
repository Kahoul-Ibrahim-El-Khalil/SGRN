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
using sgrn::Result;
static std::string script_getenv(const std::string& t_name) {
    const char* p_val = std::getenv(t_name.c_str());
    return p_val ? std::string(p_val) : std::string();
}
// registration, alongside get_dtl_now():
Result<void, std::string> registerS7Globals(asIScriptEngine* tp_engine) {
    int r = 0;
    // Global Functions

    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("string getEnv(const string &in)", asFUNCTION(script_getenv), asCALL_CDECL));

    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("string get_dtl_now()", asFUNCTION(get_dtl_now), asCALL_CDECL));

    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("DTL@ dtl()", asFUNCTION(ScriptDtl_now), asCALL_CDECL));

    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("DTL@ dtl(const string &in)", asFUNCTION(ScriptDtl_fromString), asCALL_CDECL));

    // Bug 8 fix: register DTL factory so `DTL myVar;` initializes to the current clock time.
    SGRN_AS_REG(tp_engine->RegisterObjectBehaviour(
        "DTL", asBEHAVE_FACTORY, "DTL@ f()", asFUNCTION(ScriptDtl_factory), asCALL_CDECL));

    SGRN_AS_REG(tp_engine->RegisterObjectMethod("DTL", "string toString() const", asMETHOD(ScriptDtl, toString), asCALL_THISCALL));

    // ── Time / date conversion helpers ───────────────────────────────────────
    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("int64 now_ms()", asFUNCTION(script_now_ms), asCALL_CDECL));
    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("int now_time()", asFUNCTION(script_now_time), asCALL_CDECL));
    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("uint now_tod()", asFUNCTION(script_now_tod), asCALL_CDECL));
    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("string formatTime(int)", asFUNCTION(script_formatTime), asCALL_CDECL));
    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("string formatDate(uint16)", asFUNCTION(script_formatDate), asCALL_CDECL));
    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("string formatTOD(uint)", asFUNCTION(script_formatTOD), asCALL_CDECL));
    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("int dtlToTimeMs(DTL@)", asFUNCTION(script_dtlToTimeMs), asCALL_CDECL));
    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("uint dtlToTodMs(DTL@)", asFUNCTION(script_dtlToTodMs), asCALL_CDECL));

    SGRN_AS_REG(tp_engine->RegisterGlobalFunction(
        "void setPlcTime(int year, int month, int day, int hour, int minute, int second)", asFUNCTION(Script_setPlcTime), asCALL_CDECL));

    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("void advancePlcTime(int delta_ms)", asFUNCTION(Script_advancePlcTime), asCALL_CDECL));

    SGRN_AS_REG(tp_engine->RegisterGlobalFunction("void resetPlcTime()", asFUNCTION(Script_resetPlcTime), asCALL_CDECL));

    SGRN_AS_TYPE(tp_engine, "S7Client");
    SGRN_AS_REFCOUNTED(tp_engine, "S7Client", ScriptS7Client);
    SGRN_AS_REG(tp_engine->RegisterObjectBehaviour(
        "S7Client", asBEHAVE_FACTORY, "S7Client@ f(const string &in, int, int, uint16 = 102)", asFUNCTION(S7Client_factory), asCALL_CDECL));
    SGRN_AS_REG(tp_engine->RegisterObjectBehaviour("S7Client", asBEHAVE_FACTORY,
        "S7Client@ f(const string &in, int, int, uint16, const string &in)", asFUNCTION(S7Client_factoryWithSchema), asCALL_CDECL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Client", "DataBlock@ db(uint16)", asMETHODPR(ScriptS7Client, db, (uint16_t), ScriptDataBlock*), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Client", "DataBlock@ db(const string &in)", asMETHOD(ScriptS7Client, dbByName), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Client", "TagTable@ tags()", asMETHOD(ScriptS7Client, tags), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Client", "string tagGet(const string &in)", asMETHOD(ScriptS7Client, tagGet), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Client", "double tagGetReal(const string &in)", asMETHOD(ScriptS7Client, tagGetReal), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Client", "int tagGetInt(const string &in)", asMETHOD(ScriptS7Client, tagGetInt), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Client", "bool tagGetBool(const string &in)", asMETHOD(ScriptS7Client, tagGetBool), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Client", "void tagPut(const string &in, const string &in)",
        asMETHODPR(ScriptS7Client, tagPut, (const std::string&, const std::string&), void), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Client", "void tagPut(const string &in, double)", asMETHOD(ScriptS7Client, tagPutDouble), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Client", "void tagPut(const string &in, int)", asMETHOD(ScriptS7Client, tagPutInt), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Client", "void tagPut(const string &in, bool)", asMETHOD(ScriptS7Client, tagPutBool), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Client", "void disconnect()", asMETHOD(ScriptS7Client, disconnect), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Client", "bool isConnected() const", asMETHOD(ScriptS7Client, isConnected), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Client", "void loadSclSchema(const string &in)", asMETHOD(ScriptS7Client, loadSclSchema), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Client", "void loadJsonSchema(const string &in)", asMETHOD(ScriptS7Client, loadJsonSchema), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Client", "void registerDb(uint16, uint, const string &in = \"\")", asMETHOD(ScriptS7Client, registerDb), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Client", "void registerUdt(const string &in, uint)", asMETHOD(ScriptS7Client, registerUdt), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Client",
        "void addUdtField(const string &in, const string &in, const string &in, uint, uint16 = 1)", asMETHOD(ScriptS7Client, addUdtField),
        asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Client", "void loadRegistry(const string &in)", asMETHOD(ScriptS7Client, loadRegistry), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Client", "bool hasSchema() const", asMETHOD(ScriptS7Client, hasSchema), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Client", "bool hasRegistry() const", asMETHOD(ScriptS7Client, hasRegistry), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Client", "S7Diagnostics@ diagnostics()", asMETHOD(ScriptS7Client, diagnostics), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Client", "S7PlcControl@ control()", asMETHOD(ScriptS7Client, control), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Client", "S7Memory@ memory()", asMETHOD(ScriptS7Client, memory_), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Client", "S7Blocks@ blocks()", asMETHOD(ScriptS7Client, blocks), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Client", "S7Async@ asyncIo()", asMETHOD(ScriptS7Client, asyncIo), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Client", "void reconnect()", asMETHOD(ScriptS7Client, reconnect), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Client", "bool reconnectOk()", asMETHOD(ScriptS7Client, reconnectOk), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Client", "bool reconnectWithRetry(int maxAttempts = 3, int delayMs = 500)",
        asMETHOD(ScriptS7Client, reconnectWithRetry), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Client", "bool ping()", asMETHOD(ScriptS7Client, ping), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Client", "string lastError() const", asMETHOD(ScriptS7Client, lastError), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Client", "int lastErrorCode() const", asMETHOD(ScriptS7Client, getLastErrorCode), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Client", "bool lastOpOk() const", asMETHOD(ScriptS7Client, lastOpOk), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Client", "void clearLastError()", asMETHOD(ScriptS7Client, clearLastError), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Client", "void setConnectionType(int)", asMETHOD(ScriptS7Client, setConnectionType), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Client", "void setPort(uint16)", asMETHOD(ScriptS7Client, setPort), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Client", "int connectionType() const", asMETHOD(ScriptS7Client, connectionType), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Client", "uint16 port() const", asMETHOD(ScriptS7Client, getPort), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Client", "S7Connection@ connection()", asMETHOD(ScriptS7Client, connection), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Client", "S7SchemaStore@ schema()", asMETHOD(ScriptS7Client, schema_), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Client", "string read(const string &in)", asMETHOD(ScriptS7Client, read), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Client", "void write(const string &in, const string &in)",
        asMETHODPR(ScriptS7Client, write, (const std::string&, const std::string&), void), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Client", "string listSymbols(const string &in)", asMETHOD(ScriptS7Client, listSymbols), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Client", "string searchSymbols(const string &in)", asMETHOD(ScriptS7Client, searchSymbols), asCALL_THISCALL));

    // Snap7 block type constants (PG block directory)
    static int g_block_ob = Block_OB;
    static int g_block_db = Block_DB;
    static int g_block_sdb = Block_SDB;
    static int g_block_fc = Block_FC;
    static int g_block_sfc = Block_SFC;
    static int g_block_fb = Block_FB;
    static int g_block_sfb = Block_SFB;
    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Block_OB", &g_block_ob));
    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Block_DB", &g_block_db));
    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Block_SDB", &g_block_sdb));
    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Block_FC", &g_block_fc));
    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Block_SFC", &g_block_sfc));
    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Block_FB", &g_block_fb));
    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Block_SFB", &g_block_sfb));

    SGRN_AS_REFCOUNTED(tp_engine, "S7Diagnostics", ScriptS7Diagnostics);
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Diagnostics", "string connectionInfo() const", asMETHOD(ScriptS7Diagnostics, connectionInfo), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Diagnostics", "int lastError() const", asMETHOD(ScriptS7Diagnostics, lastError), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Diagnostics", "string lastErrorText() const", asMETHOD(ScriptS7Diagnostics, lastErrorText), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Diagnostics", "string pduInfo() const", asMETHOD(ScriptS7Diagnostics, pduInfo), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Diagnostics", "string status() const", asMETHOD(ScriptS7Diagnostics, status), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Diagnostics", "bool isRunning() const", asMETHOD(ScriptS7Diagnostics, isRunning), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Diagnostics", "string cpuInfo() const", asMETHOD(ScriptS7Diagnostics, cpuInfo), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Diagnostics", "string orderCode() const", asMETHOD(ScriptS7Diagnostics, orderCode), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Diagnostics", "string cpInfo() const", asMETHOD(ScriptS7Diagnostics, cpInfo), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Diagnostics", "string info() const", asMETHOD(ScriptS7Diagnostics, info), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Diagnostics", "string diagnosticBuffer(int count = 10) const",
        asMETHOD(ScriptS7Diagnostics, diagnosticBuffer), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Diagnostics", "string szl(int id, int index) const", asMETHOD(ScriptS7Diagnostics, szl), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Diagnostics", "string listBlocks() const", asMETHOD(ScriptS7Diagnostics, listBlocks), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Diagnostics", "string listBlocksOfType(int block_type) const",
        asMETHOD(ScriptS7Diagnostics, listBlocksOfType), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Diagnostics", "string blockInfo(int block_type, uint16 block_number) const",
        asMETHOD(ScriptS7Diagnostics, blockInfo), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Diagnostics", "string protection() const", asMETHOD(ScriptS7Diagnostics, protection), asCALL_THISCALL));

    SGRN_AS_REFCOUNTED(tp_engine, "S7PlcControl", ScriptS7PlcControl);
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7PlcControl", "void hotStart()", asMETHOD(ScriptS7PlcControl, hotStart), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7PlcControl", "void coldStart()", asMETHOD(ScriptS7PlcControl, coldStart), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7PlcControl", "void stop()", asMETHOD(ScriptS7PlcControl, stop), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7PlcControl", "string clock() const", asMETHOD(ScriptS7PlcControl, clock), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7PlcControl", "void setClock(int,int,int,int,int,int)", asMETHOD(ScriptS7PlcControl, setClock), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7PlcControl", "void syncClockToSystem()", asMETHOD(ScriptS7PlcControl, syncClockToSystem), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7PlcControl", "void setPassword(const string &in)", asMETHOD(ScriptS7PlcControl, setPassword), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7PlcControl", "void clearPassword()", asMETHOD(ScriptS7PlcControl, clearPassword), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7PlcControl", "void copyRamToRom(int timeout_ms)", asMETHOD(ScriptS7PlcControl, copyRamToRom), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7PlcControl", "void compress(int timeout_ms)", asMETHOD(ScriptS7PlcControl, compress), asCALL_THISCALL));

    SGRN_AS_REFCOUNTED(tp_engine, "S7Memory", ScriptS7Memory);
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Memory", "string readArea(int area, uint16 db, int start, int size, int word_len = 2)",
        asMETHOD(ScriptS7Memory, readArea), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Memory", "void writeArea(int area, uint16 db, int start, const string &in, int word_len = 2)",
            asMETHOD(ScriptS7Memory, writeArea), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Memory", "string readAddress(const string &in, int size = 0)", asMETHOD(ScriptS7Memory, readAddress), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Memory", "void writeAddress(const string &in, const string &in)", asMETHOD(ScriptS7Memory, writeAddress), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Memory", "string readTag(const string &in)", asMETHOD(ScriptS7Memory, readTag), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Memory", "void writeTag(const string &in, const string &in)", asMETHOD(ScriptS7Memory, writeTag), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Memory", "string tagInfo(const string &in) const", asMETHOD(ScriptS7Memory, tagInfo), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Memory", "string decodeTag(const string &in, const string &in) const", asMETHOD(ScriptS7Memory, decodeTag), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Memory", "string listTags() const", asMETHOD(ScriptS7Memory, listTags), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Memory", "string readDB(uint16 db, int start, int size)", asMETHOD(ScriptS7Memory, readDB), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Memory", "void writeDB(uint16 db, int start, const string &in)", asMETHOD(ScriptS7Memory, writeDB), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Memory", "string readMB(int start, int size)", asMETHOD(ScriptS7Memory, readMB), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Memory", "void writeMB(int start, const string &in)", asMETHOD(ScriptS7Memory, writeMB), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Memory", "string readEB(int start, int size)", asMETHOD(ScriptS7Memory, readEB), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Memory", "void writeEB(int start, const string &in)", asMETHOD(ScriptS7Memory, writeEB), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Memory", "string readAB(int start, int size)", asMETHOD(ScriptS7Memory, readAB), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Memory", "void writeAB(int start, const string &in)", asMETHOD(ScriptS7Memory, writeAB), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Memory", "string readTM(int start, int count)", asMETHOD(ScriptS7Memory, readTM), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Memory", "void writeTM(int start, int count, const string &in)", asMETHOD(ScriptS7Memory, writeTM), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Memory", "string readCT(int start, int count)", asMETHOD(ScriptS7Memory, readCT), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Memory", "void writeCT(int start, int count, const string &in)", asMETHOD(ScriptS7Memory, writeCT), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Memory", "bool saveHexToFile(const string &in, const string &in) const",
        asMETHOD(ScriptS7Memory, saveHexToFile), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Memory", "string loadHexFromFile(const string &in) const", asMETHOD(ScriptS7Memory, loadHexFromFile), asCALL_THISCALL));

    SGRN_AS_REFCOUNTED(tp_engine, "S7Blocks", ScriptS7Blocks);
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Blocks", "string upload(int block_type, uint16 num, int max_size = 65536)", asMETHOD(ScriptS7Blocks, upload), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Blocks", "string fullUpload(int block_type, uint16 num, int max_size = 65536)",
        asMETHOD(ScriptS7Blocks, fullUpload), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Blocks", "void download(uint16 num, const string &in)", asMETHOD(ScriptS7Blocks, download), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Blocks", "void deleteBlock(int block_type, uint16 num)", asMETHOD(ScriptS7Blocks, deleteBlock), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Blocks", "string dbGet(uint16 db, int max_size = 65536)", asMETHOD(ScriptS7Blocks, dbGet), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Blocks", "void dbFill(uint16 db, int fill_char)", asMETHOD(ScriptS7Blocks, dbFill), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Blocks", "string pgBlockInfo(const string &in) const", asMETHOD(ScriptS7Blocks, pgBlockInfo), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Blocks", "bool saveHex(const string &in, const string &in) const", asMETHOD(ScriptS7Blocks, saveHex), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Blocks", "string loadHex(const string &in) const", asMETHOD(ScriptS7Blocks, loadHex), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Blocks", "bool uploadToFile(int block_type, uint16 num, const string &in, bool full = true)",
            asMETHOD(ScriptS7Blocks, uploadToFile), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Blocks", "bool downloadFromFile(uint16 num, const string &in)", asMETHOD(ScriptS7Blocks, downloadFromFile), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Blocks", "bool dbGetToFile(uint16 db, const string &in)", asMETHOD(ScriptS7Blocks, dbGetToFile), asCALL_THISCALL));

    SGRN_AS_REFCOUNTED(tp_engine, "S7Connection", ScriptS7ConnectionProxy);
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Connection", "bool connectWithTsap(const string &in, uint16 local, uint16 remote)",
        asMETHOD(ScriptS7ConnectionProxy, connectWithTsap), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Connection", "void useTsap(uint16 local, uint16 remote)", asMETHOD(ScriptS7ConnectionProxy, useTsap), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Connection", "void useRackSlot()", asMETHOD(ScriptS7ConnectionProxy, useRackSlot), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Connection", "bool usesTsap() const", asMETHOD(ScriptS7ConnectionProxy, usesTsap), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Connection", "uint16 localTsap() const", asMETHOD(ScriptS7ConnectionProxy, localTsap), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Connection", "uint16 remoteTsap() const", asMETHOD(ScriptS7ConnectionProxy, remoteTsap), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Connection", "int getParamInt(int) const", asMETHOD(ScriptS7ConnectionProxy, getParamInt), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Connection", "void setParamInt(int, int)", asMETHOD(ScriptS7ConnectionProxy, setParamInt), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Connection", "uint16 getParamUInt16(int) const", asMETHOD(ScriptS7ConnectionProxy, getParamUInt16), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Connection", "void setParamUInt16(int, uint16)", asMETHOD(ScriptS7ConnectionProxy, setParamUInt16), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Connection", "string paramSummary() const", asMETHOD(ScriptS7ConnectionProxy, paramSummary), asCALL_THISCALL));

    SGRN_AS_REFCOUNTED(tp_engine, "S7Async", ScriptS7Async);
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Async", "void reset()", asMETHOD(ScriptS7Async, reset), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Async", "bool beginReadArea(int area, uint16 db, int start, int size, int word_len = 2)",
        asMETHOD(ScriptS7Async, beginReadArea), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Async", "bool beginReadDB(uint16 db, int start, int size)", asMETHOD(ScriptS7Async, beginReadDB), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Async", "bool beginFullUpload(int block_type, uint16 num, int max_size = 65536)",
        asMETHOD(ScriptS7Async, beginFullUpload), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod(
        "S7Async", "bool beginDownload(uint16 num, const string &in)", asMETHOD(ScriptS7Async, beginDownload), asCALL_THISCALL));
    SGRN_AS_REG(tp_engine->RegisterObjectMethod("S7Async", "bool isDone()", asMETHOD(ScriptS7Async, isDone), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Async", "bool wait(int timeout_ms = 5000)", asMETHOD(ScriptS7Async, wait), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Async", "int lastOpError() const", asMETHOD(ScriptS7Async, getLastOpError), asCALL_THISCALL));
    SGRN_AS_REG(
        tp_engine->RegisterObjectMethod("S7Async", "string resultHex() const", asMETHOD(ScriptS7Async, resultHex), asCALL_THISCALL));

    static int g_area_db = S7AreaDB;
    static int g_area_mk = S7AreaMK;
    static int g_area_pe = S7AreaPE;
    static int g_area_pa = S7AreaPA;
    static int g_area_tm = S7AreaTM;
    static int g_area_ct = S7AreaCT;
    static int g_wl_bit = S7WLBit;
    static int g_wl_byte = S7WLByte;
    static int g_wl_word = S7WLWord;
    static int g_wl_dword = S7WLDWord;
    [[maybe_unused]] static int g_wl_real = S7WLReal;
    [[maybe_unused]] static int g_wl_counter = S7WLCounter;
    [[maybe_unused]] static int g_wl_timer = S7WLTimer;
    [[maybe_unused]] static int g_blk_ob = Block_OB;
    [[maybe_unused]] static int g_blk_db = Block_DB;
    [[maybe_unused]] static int g_blk_sdb = Block_SDB;
    [[maybe_unused]] static int g_blk_fc = Block_FC;
    [[maybe_unused]] static int g_blk_sfc = Block_SFC;
    [[maybe_unused]] static int g_blk_fb = Block_FB;
    [[maybe_unused]] static int g_blk_sfb = Block_SFB;

    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Area_DB", &g_area_db));
    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Area_MK", &g_area_mk));
    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Area_PE", &g_area_pe));
    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Area_PA", &g_area_pa));
    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Area_TM", &g_area_tm));
    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Area_CT", &g_area_ct));
    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int WL_Bit", &g_wl_bit));
    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int WL_Byte", &g_wl_byte));
    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int WL_Word", &g_wl_word));
    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int WL_DWord", &g_wl_dword));

    static int g_conn_pg = CONNTYPE_PG;
    static int g_conn_op = CONNTYPE_OP;
    static int g_conn_basic = CONNTYPE_BASIC;
    static int g_p_remote_port = p_u16_RemotePort;
    static int g_p_ping_timeout = p_i32_PingTimeout;
    static int g_p_send_timeout = p_i32_SendTimeout;
    static int g_p_recv_timeout = p_i32_RecvTimeout;
    static int g_p_work_interval = p_i32_WorkInterval;
    static int g_p_src_tsap = p_u16_SrcTSap;
    static int g_p_pdu_request = p_i32_PDURequest;
    static int g_p_bsend_timeout = p_i32_BSendTimeout;
    static int g_p_brecv_timeout = p_i32_BRecvTimeout;

    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int ConnType_PG", &g_conn_pg));

    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int ConnType_OP", &g_conn_op));

    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int ConnType_BASIC", &g_conn_basic));

    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Param_RemotePort", &g_p_remote_port));

    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Param_PingTimeout", &g_p_ping_timeout));

    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Param_SendTimeout", &g_p_send_timeout));

    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Param_RecvTimeout", &g_p_recv_timeout));

    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Param_WorkInterval", &g_p_work_interval));

    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Param_SrcTSap", &g_p_src_tsap));

    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Param_PDURequest", &g_p_pdu_request));

    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Param_BSendTimeout", &g_p_bsend_timeout));

    SGRN_AS_REG(tp_engine->RegisterGlobalProperty("const int Param_BRecvTimeout", &g_p_brecv_timeout));

    return {};
}
} // namespace sgrn::s7shell::shell
