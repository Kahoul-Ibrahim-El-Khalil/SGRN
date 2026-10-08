#include <sgrn/s7shell/S7Shell.hpp>
#include <string>
#include <string_view>

#include <fmt/color.h>
#include <fmt/format.h>
#include <sgrn/s7shell/SchemaVM.hpp>
#include <sgrn/s7shell/bindings/registration.hpp>
#include <sgrn/scl/schema/PlcSchemaStore.hpp>
#include <sgrn/utils/filesystem.hpp>
#include <sgrn/utils/strings.hpp>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <rapidjson/document.h>
#include <rapidjson/error/en.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <readline/readline.h>
#include <regex>
#include <scriptbuilder/scriptbuilder.h>
#include <scripthelper/scripthelper.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace sgrn::s7shell::shell
{

void S7Shell::printHelp() const {
    showHelp();
}

std::string S7Shell::getPrompt() const {
    return "s7> ";
}

// ─────────────────────────────────────────────────────────────────────────────
// Auto-print for REPL.
//
// Probe order: toJson() (pretty) → toString() → info() → print()
// Uses simple flat-call syntax (no outer block) to avoid the `{}` collision
// with AngelScript compound-statement braces.
// ─────────────────────────────────────────────────────────────────────────────

bool tryAutoPrint(asIScriptEngine* tp_engine, asIScriptModule* tp_mod, asIScriptContext* tp_ctx, const std::string& t_expr) {
    // Each probe: PLACEHOLDER is replaced with the expression.
    // toJson result is routed through prettyJson() before printing.
    static const std::pair<const char*, const char*> k_probes[] = {
        {"toJson", "print(prettyJson((PLACEHOLDER).toJson()) + \"\\n\");"},
        {"toString", "print((PLACEHOLDER).toString() + \"\\n\");"},
        {"info", "print((PLACEHOLDER).info());"},
        {"print", "(PLACEHOLDER).print();"},
        {"direct", "print(\"\" + (PLACEHOLDER) + \"\\n\");"}, // primitives: int/float/bool/etc.
    };
    for (auto& [label, tpl] : k_probes) {
        std::string stmt = std::string(tpl);
        const std::string ph = "PLACEHOLDER";
        stmt.replace(stmt.find(ph), ph.size(), t_expr);

        sgrn::scripting::g_suppress_errors = true;
        int r = ExecuteString(tp_engine, stmt.c_str(), tp_mod, tp_ctx);
        sgrn::scripting::g_suppress_errors = false;

        if (r == asEXECUTION_FINISHED)
            return true;

        if (r == asEXECUTION_EXCEPTION) {
            fmt::print(stderr, fg(fmt::color::red), "Exception: {}\n", tp_ctx->GetExceptionString());
            return true;
        }
        // Compilation error → try next probe
    }
    return false;
}

static constexpr std::string_view kHelpText =
    "\033[1;36mSiemens Types:\033[0m\n"
    "  BOOL, SINT, USINT, BYTE, INT, UINT, WORD, DINT, UDINT, DWORD\n"
    "  LINT, ULINT, LWORD, REAL, LREAL\n"
    "  TIME (ms), LTIME (ns), DATE (days), TOD (ms), LTOD (ns)\n\n"

    "\033[1;36mVirtual PLC Runtime:\033[0m\n"
    "  PlcRuntime@ rt = PlcRuntime()                       // empty, load schema later\n"
    "  PlcRuntime@ rt = PlcRuntime(\"plant.scl\")            // load SCL schema on creation\n"
    "  PlcRuntime@ rt = PlcRuntime(\"\", \"DB1 pump:BOOL; END_DB\")  // inline SCL text, no file — for debugging\n"
    "  rt.loadSclSchema(path) / rt.loadJsonSchema(path) / rt.loadRegistry(path)\n"
    "  rt.registerDb(num, size, name)  /  rt.registerUdt(name, size)  // manual schema authoring\n"
    "  rt.set(db, \"field.path\", \"value_json\")  // write field by symbolic path\n"
    "  rt.get(db, \"field.path\")                // read field \u2192 JSON string\n"
    "  rt.getJson(db)                          // dump full DB as JSON\n"
    "  rt.setBit(db, byte_offset, bit, bool)  // raw bit write\n"
    "  rt.hasDirty(db)                        // check if any dirty regions\n"
    "  rt.sync() / rt.syncDb(db)              // explicit sync: re-fire dirty observers (dual-mode)\n"
    "  rt.dirtySnapshot()                     // delta JSON for dirty DBs (\"{}\" when clean)\n"
    "\033[90m  Note: rt.loadSclSchema(path) auto-injects DataBlock@ globals (see \"Auto DB\n"
    "  References\" below); the PlcRuntime(\"plant.scl\") constructor form does not \u2014\n"
    "  call rt.loadSclSchema(path) explicitly afterward if you want the globals too.\033[0m\n\n"

    "\033[1;36mS7Server (Virtual PLC Server):\033[0m\n"
    "  S7Server@ srv = S7Server(rt, \"0.0.0.0\")        // bind runtime to S7 server\n"
    "  S7Server@ srv = S7Server(rt, \"0.0.0.0\", 102)   // custom port\n"
    "  srv.start() / srv.stop()                        // lifecycle\n"
    "  srv.isRunning() / srv.clientsCount()\n"
    "  srv.getCpuStatus()\n"
    "  // Pattern: create runtime, attach server, manipulate memory freely\n"
    "  //   PlcRuntime@ rt = PlcRuntime(\"plant.scl\");\n"
    "  //   rt.loadSclSchema(\"plant.scl\");             // repeat, so DataBlock@ globals get injected\n"
    "  //   S7Server@ srv  = S7Server(rt, \"0.0.0.0\");  srv.start();\n"
    "  //   S7Client@ cli  = S7Client(\"127.0.0.1\", 0, 1, 102, rt);\n"
    "  //   rt.set(1, \"valve\", \"true\");  // visible to all S7 clients\n\n"

    "\033[1;36mS7Client (PLC Connection):\033[0m\n"
    "  S7Client@ client = S7Client(ip, rack, slot)\n"
    "  S7Client@ client = S7Client(ip, rack, slot, port, rt)  // attach to shared runtime\n"
    "  client.loadSclSchema(path) / client.loadJsonSchema(path) / client.loadRegistry(path)\n"
    "  client.registerDb(num, size, name) / client.registerUdt(name, size) / client.addUdtField(...)\n"
    "  client.hasSchema() / client.hasRegistry()\n"
    "  client.isConnected() / client.ping() / client.disconnect() / client.reconnect()\n"
    "  client.reconnectOk() / client.reconnectWithRetry(maxAttempts = 3, delayMs = 500)\n"
    "  client.lastError() / client.lastErrorCode() / client.lastOpOk() / client.clearLastError()\n"
    "  client.setConnectionType(t) / client.setPort(p) / client.connectionType() / client.port()\n"
    "  client.read(address) / client.write(address, hex)   // raw, unschematized access\n"
    "  client.listSymbols(filter) / client.searchSymbols(query)\n"
    "  DataBlock@ db = client.db(42) / client.db(\"DBName\")\n"
    "  TagTable@ tags = client.tags()\n"
    "  S7Connection@ conn = client.connection()   // TSAP / low-level connection tuning, see below\n"
    "  S7SchemaStore@ sch = client.schema()       // introspect the loaded schema\n\n"

    "\033[1;36mDataBlock:\033[0m\n"
    "  db.get() / db.put()                       // fetch/flush the whole DB\n"
    "  db.get(path) / db.put(path, val)          // single field: network fetch / shadow + trip\n"
    "  db.getReal(path) / db.getInt(path) / db.getBool(path)\n"
    "  db.getRetry(path, maxRetries = 3) / db.putRetry(path, val, maxRetries = 3)\n"
    "  db.lastOpOk() / db.lastOpError()\n"
    "  db[\"field.path\"]  \u2192 FieldProxy@             // opIndex shorthand\n"
    "  FieldProxy f = db[\"a.b\"] \u2192 f.get() / f.put(v) / f.put()  // field-level net ops\n"
    "  f = {1.0, 2.0} / f = {{\"x\", 1}}               // array / struct-dict assign\n"
    "  db.path(\"field.path\")  \u2192 S7PathBatch@       // see S7PathBatch below\n"
    "  db.toJson() / db.diff() / db.number() / db.name() / db.print()\n"
    "  db.registerSize(bytes) / db.addField(name, type, offset, arrayLen = 1)  // manual schema authoring\n"
    "  HexTable@ hex = cast<HexTable>(db)  \u2192  hex.print()\n"
    "  get() fetches from the network into shadow; put(path, val) writes shadow\n"
    "  + trips; put() flushes dirty to the server; db.field is live + typed\n"
    "  Offline get()/put() serve the twin shadow; on a dropped link (vs never-\n"
    "  connected virtual use) lastOpOk() is false + NotConnected — stale reads\n"
    "  never pass silently; check the flags before trusting the value\n\n"

    "\033[1;36mTagTable (symbol-table access):\033[0m\n"
    "  TagTable@ tags = client.tags();\n"
    "  tags.get(name) / tags.getReal(name) / tags.getInt(name) / tags.getBool(name)\n"
    "  tags.put(name, val)                         // shadow + network trip, immediate\n"
    "  tags.put() / tags.get()                       // flush dirty / refresh all\n"
    "  tags.getRetry(name, maxRetries = 3) / tags.putRetry(name, val, maxRetries = 3)\n"
    "  tags.lastOpOk() / tags.lastOpError()\n"
    "  tags.path(name)  \u2192 S7PathBatch@ (read/get/put/toJson)\n"
    "  client.tagGet(name) / client.tagPut(name, val)  // direct shortcuts\n"
    "  tags.tag_name is live + typed (bool/int/float/double/string)\n\n"

    "\033[1;36mDiscrete Tags (TIA-style #TAG_TABLE):\033[0m\n"
    "  Tags are discrete addressed objects (%I inputs, %Q outputs, %M/%MW/%MD\n"
    "  merkers) — not DataBlocks (DBs stay DATA_BLOCK syntax; DB rows in tag\n"
    "  tables are rejected). Declare them in SCL:\n"
    "    #TAG_TABLE \"Inputs and Outputs\"\n"
    "    [ on : BOOL @ %I0.0, motor : \"Motor\" @ %MB100, ]\n"
    "  rt.defineTag(name, type, addr)                 // manual row (any scalar/UDT)\n"
    "  rt.tagList() / rt.tagList(table) / rt.tagTables() / rt.tagInfo(name)\n"
    "  Typed shell access (scalars live; UDT members via tags.put/get JSON):\n"
    "    TagTable@ tags = client.tags();\n"
    "    tags.start_button = true;  x = tags.line_speed + 1.0;\n"
    "  One shared backing: tags.get/put, tagGet/tagPut, the S7 server (%I/%Q/%M),\n"
    "  HTTP /tags/<name> (GET/POST/PUT), the OPC UA Tags folder (scalars native,\n"
    "  UDTs as structured member nodes) and gateway flat deltas see the same bytes.\n"
    "  Areas %I/%Q/%M auto-size on define (cap 64 KiB); T/C areas, string types\n"
    "  and tag arrays are not backed yet. Mirrored rows needed on sync peers.\n\n"

    "\033[1;36mS7PathBatch (fluent field access):\033[0m\n"
    "  S7PathBatch@ b = db.path(\"field.path\");   // also tags.path(name)\n"
    "  b.get()    // refresh from the PLC\n"
    "  b.read()   // current value \u2192 string\n"
    "  b.put()    // flush staged writes to the PLC\n"
    "  b.toJson()\n\n"

    "\033[1;36mS7Connection (low-level connection tuning):\033[0m\n"
    "  S7Connection@ conn = client.connection();\n"
    "  conn.connectWithTsap(ip, localTsap, remoteTsap) / conn.useTsap(local, remote) / conn.useRackSlot()\n"
    "  conn.usesTsap() / conn.localTsap() / conn.remoteTsap()\n"
    "  conn.getParamInt(id) / conn.setParamInt(id, val) / conn.getParamUInt16(id) / conn.setParamUInt16(id, val)\n"
    "  conn.paramSummary()\n"
    "  Param ids: p_u16_RemotePort, p_i32_PingTimeout, p_i32_SendTimeout, p_i32_RecvTimeout,\n"
    "             p_i32_WorkInterval, p_u16_SrcTSap, p_i32_PDURequest, p_i32_BSendTimeout, p_i32_BRecvTimeout\n"
    "  Connection types: CONNTYPE_PG, CONNTYPE_OP, CONNTYPE_BASIC  (client.setConnectionType(...))\n\n"

    "\033[1;36mDiagnostics:\033[0m\n"
    "  S7Diagnostics@ d = client.diagnostics();\n"
    "  d.connectionInfo() / d.status() / d.info() / d.cpuInfo() / d.pduInfo()\n"
    "  d.isRunning() / d.orderCode() / d.cpInfo() / d.protection()\n"
    "  d.lastError() / d.lastErrorText()\n"
    "  d.diagnosticBuffer(count = 10) / d.szl(id, index)\n"
    "  d.listBlocks() / d.listBlocksOfType(blockType) / d.blockInfo(blockType, blockNumber)\n\n"

    "\033[1;36mPLC Control:\033[0m\n"
    "  S7PlcControl@ c = client.control();\n"
    "  c.hotStart() / c.coldStart() / c.stop()\n"
    "  c.clock() / c.setClock(y,m,d,h,min,s) / c.syncClockToSystem()\n"
    "  c.setPassword(pw) / c.clearPassword()\n"
    "  c.copyRamToRom(timeoutMs) / c.compress(timeoutMs)\n\n"

    "\033[1;36mLow-level Memory:\033[0m\n"
    "  S7Memory@ m = client.memory();\n"
    "  m.readArea(Area_DB,db,start,size,wordLen=WL_Byte) / m.writeArea(...,hex,wordLen=WL_Byte)\n"
    "  m.readAddress(address, size = 0) / m.writeAddress(address, hex)   // e.g. \"DB1.DBX0.0\"\n"
    "  m.readTag(name) / m.writeTag(name, hex) / m.tagInfo(name) / m.decodeTag(name, hex) / m.listTags()\n"
    "  m.readDB(db,start,size) / m.writeDB(db,start,hex)\n"
    "  m.readMB(start,size) / m.writeMB(start,hex)   // merkers\n"
    "  m.readEB(start,size) / m.writeEB(start,hex)   // inputs\n"
    "  m.readAB(start,size) / m.writeAB(start,hex)   // outputs\n"
    "  m.readTM(start,count) / m.writeTM(start,count,hex)   // timers\n"
    "  m.readCT(start,count) / m.writeCT(start,count,hex)   // counters\n"
    "  m.saveHexToFile(path, hex) / m.loadHexFromFile(path)\n"
    "  Areas: Area_DB, Area_MK, Area_PE, Area_PA, Area_TM, Area_CT\n"
    "  WordLen: WL_Bit, WL_Byte, WL_Word, WL_DWord\n\n"

    "\033[1;36mPLC Simulation Time:\033[0m\n"
    "  DTL@ ts = dtl()  /  setPlcTime(y,m,d,h,min,s)\n"
    "  advancePlcTime(ms)  /  resetPlcTime()\n\n"

    "\033[1;36mGateway Sync & Proxy:\033[0m\n"
    "  S7ProxySession@ p = S7ProxySession(srcClient, hubClient);\n"
    "  p.addMapping(srcDB, dstDB, interval_ms, size_bytes = 0) / p.start() / p.stop() / p.running()\n"
    "  GatewaySync@ s = GatewaySync(runtime);\n"
    "  s.subscribeDb(db) / s.unsubscribeDb(db) / s.publishOnDirty(true) / s.useBinary(true)\n"
    "  s.connect(\"ws://...\") / s.disconnect() / s.connected() / s.lastError()\n"
    "  Discrete tags publish uplink as write_area (JSON, acked, HTTP POST /tags\n"
    "  fallback); DB regions use write/RuntimeSync as before.\n\n"

    "\033[1;36mUnified Runtime Gateway:\033[0m\n"
    "  Gateway@ g = Gateway(runtime); g.start(\"0.0.0.0\", 8080)\n"
    "  HTTP REST and WebSocket (/ws) share one listener; g.stop() / g.isRunning()\n"
    "  Dual-mode updates: every markDirty auto-broadcasts a delta while running\n"
    "  (auto-broadcast on by default; g.setAutoBroadcast(false) to go manual).\n"
    "  Deltas are flat leaf-id frames like the full gateway ({\"0\": 100.0});\n"
    "  the {\"type\":\"dictionary\"} decode table is pushed at connect time.\n"
    "  Explicit push: g.broadcast()  // flat delta, else full twin\n"
    "  Out-of-band:   g.broadcast(\"{\\\"alert\\\":...}\")      // manual JSON to all WS clients\n"
    "  Inspect:       rt.dirtySnapshot() (nested) / g.autoBroadcast()\n\n"

    "\033[1;36mAuto-print (REPL):\033[0m\n"
    "  Bare object expressions auto-call:\n"
    "    toJson()    \u2192 DataBlock, TagTable, S7PathBatch\n"
    "    toString()  \u2192 HexTable, DTL, FieldProxy\n"
    "    info()      \u2192 S7Diagnostics\n"
    "    print()     \u2192 everything else\n"
    "  Primitives (int, float, bool) print as-is.\n\n"

    "\033[1;36mTyped Property Accessors:\033[0m\n"
    "  After an explicit call to <var>.loadSclSchema(path) / .loadJsonSchema(path),\n"
    "  all DBs become available as properties on the PlcRuntime or S7Client object:\n"
    "    rt.PrimaryCoolant.get(\"temp_pv\")       (PascalCase, matching schema)\n"
    "    client.primary_coolant.set(\"on\", \"true\") (snake_case is also supported)\n"
    "  Properties return a DataBlock@ reference. You can print them directly in the REPL:\n"
    "    s7> client.PrimaryCoolant\n"
    "\033[90m  Schemas loaded via a constructor argument, e.g. PlcRuntime(\"plant.scl\"),\n"
    "  are also auto-registered globally.\033[0m\n\n"

    "\033[1;36mInit Script (./angelscript.as):\033[0m\n"
    "  Loaded at shell startup. Define shared globals and helpers:\n"
    "    S7Client@ plc = S7Client(\"192.168.1.10\", 0, 1);\n"
    "    void cycle() { ... }\n"
    "  All symbols are available in every REPL line and runScript().\n\n"

    "\033[1;36mShell Variables:\033[0m\n"
    "  $NAME / ${NAME} in any REPL line expands a global of scalar type\n"
    "  (string, bool, int/uint widths, float, double), e.g.:\n"
    "    int retries = 3;  retryWrite(path, val, $retries);\n"
    "    string tag = \"DB1.val\";  db.get($tag);\n"
    "  $$ is a literal dollar; unknown names stay literal.\n\n";

void S7Shell::showHelp() const {
    AngelScriptEngine::showHelp();
    fmt::print("{}", kHelpText);
}
} // namespace sgrn::s7shell::shell
