-- sgrn/lib/xmake.lua
-- SGRN libraries. Source lists mirror the former per-directory CMakeLists.txt.
-- Libraries that CMake built SHARED in dev mode use set_kind("$(kind)"):
-- static by default, `xmake f -k shared` gives the dev (shared) build.

local winlibs = is_plat("windows", "mingw")

-- test helper: one executable per test, built by default, run by `xmake test`
local function sgrn_test(name, file, opt)
    if not has_config("tests") then return end
    opt = opt or {}
    target(name)
        set_kind("binary")
        set_group("tests")
        add_files(file)
        if opt.deps then add_deps(table.unpack(opt.deps)) end
        if opt.packages then add_packages(table.unpack(opt.packages)) end
        if opt.inc then add_includedirs(table.unpack(opt.inc)) end
        if opt.defines then add_defines(table.unpack(opt.defines)) end
        if opt.flags then add_cxflags(table.unpack(opt.flags), {force = true}) end
        if opt.support then
            add_includedirs("$(projectdir)/sgrn/lib/tests/support")
            add_packages("cpp-httplib")
        end
        if opt.bins then
            on_config(function (target)
                for _, b in ipairs(opt.bins) do
                    local dep = target:dep(b[2])
                    if dep then target:add("defines", format('%s="%s"', b[1], dep:targetfile())) end
                end
            end)
        end
        add_tests("default")
    target_end()
end

-- ── 1. core & codecs ────────────────────────────────────────────────────────
target("sgrn_s7codec")
    set_kind("headeronly")
    add_includedirs("codecs/s7codec/include", {public = true})
    if not has_config("datastore") then
        add_defines("SGRN_NO_DROGON", {public = true})
    end

target("sgrn_core")
    set_kind("headeronly")
    add_includedirs("core/include", {public = true})
    add_deps("sgrn_s7codec")
    add_packages("fmt", "angelscript", "glaze", "jsoncpp", {public = true})
    if not is_plat("windows", "mingw") then
        add_packages("readline", {public = true})
    end
    if not has_config("datastore") then
        add_defines("SGRN_NO_DROGON", {public = true})
    end

target("sgrn_opcua_codec_table")
    set_kind("static")
    add_files("codecs/opcua-codec-table.cpp")
    add_includedirs("codecs", {public = true})
    add_includedirs("scl/include")
    add_deps("sgrn_s7codec")
    add_packages("open62541", {public = true})

-- ── 2. utils ────────────────────────────────────────────────────────────────
target("sgrn_utils")
    set_kind("$(kind)")
    add_files("utils/src/compression.cpp", "utils/src/crypto.cpp", "utils/src/crypto/dek.cpp",
              "utils/src/crypto/key-provider.cpp", "utils/src/crypto/aead.cpp",
              "utils/src/crypto/stream.cpp", "utils/src/file-signature.cpp", "utils/src/jwt.cpp",
              "utils/src/xml.cpp", "utils/src/xml_impl.c", "utils/src/encoding.cpp",
              "utils/src/network.cpp", "utils/src/delta.cpp")
    add_includedirs("utils/include", {public = true})
    add_deps("sgrn_core")
    add_packages("zlib", "zstd", "openssl", "fmt", "glaze", "jsoncpp", "asio", {public = true})
    add_defines("ASIO_STANDALONE", {public = true})
    add_packages("xml_h")
    if winlibs then add_syslinks("ws2_32", "crypt32", "advapi32", {public = true}) end

-- ── 3. scl / scripting ──────────────────────────────────────────────────────
target("sgrn_scl")
    set_kind("$(kind)")
    add_files("scl/src/utils.cpp", "scl/src/schema/PlcSchemaStore.cpp", "scl/src/schema/SclCompiler.cpp",
              "scl/src/schema/SchemaSerializer.cpp", "scl/src/schema/DbSymbolsParser.cpp",
              "scl/src/schema/XmlTagTableParser.cpp", "scl/src/schema/XmlTagTableImplementation.cpp",
              "scl/src/schema/TagTable.cpp", "scl/src/schema/ModbusMap.cpp",
              "scl/src/types/DbField.cpp", "scl/src/types/DbSchema.cpp",
              "scl/src/types/UdtDefinition.cpp", "scl/src/AsApiEmitter.cpp")
    add_includedirs("scl/include", {public = true})
    add_deps("sgrn_core", "sgrn_utils", "sgrn_s7codec")
    add_packages("fmt", "open62541", {public = true})
    add_packages("xml_h")

target("sgrn_scripting")
    set_kind("static")
    add_files("scripting/src/ScriptHost.cpp")
    add_includedirs("scripting/include", {public = true})
    add_deps("sgrn_utils")
    add_packages("angelscript", {public = true})

-- ── 4. protocol wrappers ────────────────────────────────────────────────────
target("sgrn_wrappers_s7")
    set_kind("static")
    add_files("wrappers/src/s7/S7Client.cpp", "wrappers/src/s7/S7Server.cpp")
    add_includedirs("wrappers/include", "scl/include", {public = true})
    add_deps("sgrn_utils", "sgrn_scl")
    add_packages("snap7", "open62541", {public = true})
    add_packages("fmt")
    add_defines("SGRN_HAS_SNAP7", {public = true})

target("sgrn_wrappers_modbus")
    set_kind("static")
    add_files("wrappers/src/modbus/Mapping.cpp", "wrappers/src/modbus/Client.cpp",
              "wrappers/src/modbus/Server.cpp")
    add_includedirs("wrappers/include", {public = true})
    add_deps("sgrn_utils")
    add_packages("libmodbus", "fmt")

target("sgrn_wrappers_ethernetip")
    set_kind("static")
    add_files("wrappers/src/ethernetip/Server.cpp", "wrappers/src/ethernetip/Client.cpp")
    add_includedirs("wrappers/include", {public = true})
    add_deps("sgrn_utils")
    add_packages("opener", "fmt", {public = true})

target("sgrn_wrappers_opcua")
    set_kind("static")
    add_files("wrappers/src/opcua/TypeRegistry.cpp", "wrappers/src/opcua/SessionRegistry.cpp",
              "wrappers/src/opcua/EventSupport.cpp", "wrappers/src/opcua/Server.cpp",
              "wrappers/src/opcua/Client.cpp")
    add_includedirs("wrappers/include", {public = true})
    add_deps("sgrn_utils")
    add_packages("open62541", {public = true})
    add_packages("fmt")

-- ── 5. common + gateway components ──────────────────────────────────────────
target("sgrn_common")
    set_kind("headeronly")
    add_includedirs("common/include", {public = true})
    add_deps("sgrn_core", "sgrn_utils", "sgrn_scl", "sgrn_s7codec")
    add_packages("fmt", {public = true})

target("sgrn_gateway_security")
    set_kind("static")
    add_files("gateway/src/security/SecurityManager.cpp", "gateway/src/security/PolicyEngine.cpp")
    add_includedirs("gateway/include", {public = true})
    add_deps("sgrn_scl", "sgrn_utils", "sgrn_scripting")
    add_packages("open62541", {public = true})

target("sgrn_gateway_twin")
    set_kind("static")
    add_files("gateway/src/core/snapshot.cpp", "gateway/src/core/TreeCacheEngine.cpp",
              "gateway/src/core/RecoveryEngine.cpp", "gateway/src/twin/DbSnapshot.cpp",
              "gateway/src/twin/PlcState.cpp", "gateway/src/twin/DbIOProvider.cpp",
              "gateway/src/twin/PlcMemory.cpp", "gateway/src/twin/PlcMemory-io.cpp",
              "gateway/src/twin/PlcMemory-query.cpp", "gateway/src/twin/encoding.cpp",
              "gateway/src/twin/serialization.cpp", "gateway/src/twin/path.cpp",
              "gateway/src/twin/time-utils.cpp", "gateway/src/twin/PlcCommandProcessor.cpp",
              "gateway/src/twin/field-update.cpp", "gateway/src/twin/SnapshotRegistry.cpp",
              "gateway/src/twin/LeafDictionary.cpp")
    add_includedirs("gateway/include", {public = true})
    add_deps("sgrn_wrappers_s7", "sgrn_scl", "sgrn_utils", "sgrn_common", {public = true})
    add_packages("asio", "fmt", "unordered_dense", {public = true})
    add_packages("open62541")
    add_defines("ASIO_STANDALONE", {public = true})

target("sgrn_gateway_ports")
    set_kind("static")
    add_files("gateway/src/adapters/ports/TwinPorts.cpp")
    add_includedirs("gateway/include", {public = true})
    add_deps("sgrn_common", "sgrn_gateway_twin", "sgrn_gateway_security")

target("sgrn_gateway_modbus")
    set_kind("static")
    add_files("gateway/src/adapters/modbus/ModbusAdapter.cpp", "gateway/src/adapters/modbus/TypeTranslation.cpp")
    add_includedirs("gateway/include", {public = true})
    add_deps("sgrn_common", "sgrn_scl", "sgrn_wrappers_modbus", "sgrn_gateway_twin", {public = true})
    add_packages("fmt")

target("sgrn_gateway_ethernetip")
    set_kind("static")
    add_files("gateway/src/adapters/ethernetip/EipAdapter.cpp", "gateway/src/adapters/ethernetip/CipCodec.cpp",
              "gateway/src/adapters/ethernetip/TypeTranslation.cpp")
    add_includedirs("gateway/include", {public = true})
    add_deps("sgrn_common", "sgrn_scl", "sgrn_wrappers_ethernetip", "sgrn_s7codec", {public = true})
    add_packages("opener", {public = true})
    add_packages("fmt")

target("sgrn_gateway_s7")
    set_kind("static")
    add_files("gateway/src/adapters/s7/S7ProtocolAdapter.cpp", "gateway/src/adapters/s7/TypeTranslation.cpp")
    add_includedirs("gateway/include", {public = true})
    add_deps("sgrn_wrappers_s7", "sgrn_common", "sgrn_scl", {public = true})
    add_packages("fmt")

target("sgrn_gateway_io")
    set_kind("headeronly")
    add_deps("sgrn_wrappers_s7", "sgrn_gateway_s7", "sgrn_gateway_security")
    add_defines("SGRN_HAS_SNAP7", {public = true})

target("sgrn_gateway_opcua")
    set_kind("static")
    add_files("gateway/src/adapters/opcua/OpcUaAdapter.cpp", "gateway/src/adapters/opcua/schema-type-registry.cpp",
              "gateway/src/adapters/opcua/TypeTranslation.cpp", "gateway/src/adapters/opcua/decoders.cpp",
              "gateway/src/adapters/opcua/readValue.cpp", "gateway/src/adapters/opcua/writeValue.cpp",
              "gateway/src/adapters/opcua/delta-push.cpp", "gateway/src/adapters/opcua/node-registration.cpp",
              "gateway/src/adapters/opcua/information-model.cpp", "gateway/src/adapters/opcua/encoders.cpp")
    add_includedirs("gateway/include", {public = true})
    add_includedirs("scl/include")
    add_deps("sgrn_wrappers_opcua", "sgrn_common", "sgrn_gateway_twin", "sgrn_gateway_security",
             "sgrn_scl", {public = true})
    add_deps("sgrn_opcua_codec_table")
    add_packages("fmt")

target("sgrn_gateway")
    set_kind("$(kind)")
    add_files("gateway/src/datastore/DatastoreBridge.cpp", "gateway/src/config/gateway.cpp",
              "gateway/src/config/proxy.cpp", "gateway/src/config/mbproxy.cpp",
              "gateway/src/adapters/northbound/NorthboundServer.cpp", "gateway/src/adapters/http.cpp",
              "gateway/src/adapters/http/types.cpp", "gateway/src/adapters/http/path.cpp",
              "gateway/src/adapters/http/put-handler.cpp", "gateway/src/adapters/http/memory-handlers.cpp",
              "gateway/src/adapters/http/post-handler.cpp", "gateway/src/adapters/http/read-handlers.cpp",
              "gateway/src/adapters/http/tags-handlers.cpp", "gateway/src/adapters/http/assets.cpp",
              "gateway/src/adapters/s7/callbacks.cpp", "gateway/src/core/snapshot.cpp",
              "gateway/src/adapters/websocket/WebSocketAdapter.cpp",
              "gateway/src/adapters/websocket/delta-push.cpp",
              "gateway/src/adapters/websocket/client-messages.cpp",
              "gateway/src/database/GatewayDatabase.cpp", "gateway/src/database/EngineStateStore.cpp",
              "gateway/src/database/PersistenceService.cpp", "gateway/src/gateway.cpp")
    add_includedirs("gateway/include", {public = true})
    add_includedirs("$(projectdir)/$(buildir)/gen/gateway/web",
                    "$(projectdir)/$(buildir)/gen/gateway/doc",
                    "scl/include")
    add_deps("sgrn_utils", "sgrn_datastore_client", "sgrn_gateway_s7", "sgrn_gateway_twin",
             "sgrn_gateway_opcua", "sgrn_gateway_ethernetip", "sgrn_gateway_modbus", "sgrn_scl",
             "sgrn_common", "sgrn_gateway_ports", {public = true})
    add_packages("crow", "asio", "sqlite_modern_cpp", "sqlite3", {public = true})
    add_packages("unordered_dense")
    add_defines("ASIO_STANDALONE", {public = true})
    if winlibs then add_syslinks("mswsock", "ws2_32", {public = true}) end
    before_build(function (target)
        import("sgrn.assets")
        assets.gateway_web()
        assets.gateway_doc()
    end)

-- ── 6. PLC simulation & s7shell ─────────────────────────────────────────────
target("sgrn_plcsim")
    set_kind("static")
    add_files("plcsim/src/PlcTagTable.cpp", "plcsim/src/runtime/PlcRuntime.cpp",
              "plcsim/src/runtime/PersistenceBridge.cpp", "plcsim/src/simulation/SimulationEngine.cpp",
              "plcsim/src/replay/WalReplayer.cpp", "plcsim/src/utils/PlcSimClock.cpp")
    add_includedirs("plcsim/include", {public = true})
    add_deps("sgrn_gateway_twin", "sgrn_gateway", "sgrn_scl", "sgrn_utils")
    add_packages("fmt")

target("sgrn_s7shell")
    set_kind("$(kind)")
    add_files("s7shell/src/s7shell/s7shell.cpp", "s7shell/src/s7shell/print.cpp",
              "s7shell/src/s7shell/autocomplete.cpp", "s7shell/src/s7shell/schema.cpp",
              "s7shell/src/s7shell/execute.cpp", "s7shell/src/s7shell/initScript.cpp",
              "s7shell/src/s7shell/runScript.cpp", "s7shell/src/s7shell/onStart.cpp",
              "s7shell/src/s7shell/runShellCommand.cpp", "s7shell/src/SchemaVM.cpp",
              "s7shell/src/commands/cmd-emit-as.cpp", "s7shell/src/script/ScriptDataBlock.cpp",
              "s7shell/src/script/ScriptFieldProxy.cpp", "s7shell/src/script/ScriptTagTable.cpp",
              "s7shell/src/script/ScriptPathBatch.cpp", "s7shell/src/script/ScriptHexTable.cpp",
              "s7shell/src/script/ScriptSchemaStore.cpp", "s7shell/src/facades/ScriptDiagnostics.cpp",
              "s7shell/src/facades/ScriptPlcControl.cpp", "s7shell/src/facades/ScriptMemory.cpp",
              "s7shell/src/facades/ScriptBlocks.cpp", "s7shell/src/facades/ScriptAsync.cpp",
              "s7shell/src/facades/ScriptConnectionProxy.cpp", "s7shell/src/connection/ScriptS7Connection.cpp",
              "s7shell/src/bindings/registration.cpp", "s7shell/src/bindings/types.cpp",
              "s7shell/src/bindings/globals.cpp", "s7shell/src/bindings/PlcRuntime.cpp",
              "s7shell/src/bindings/Persistence.cpp", "s7shell/src/bindings/Simulation.cpp",
              "s7shell/src/bindings/Replay.cpp", "s7shell/src/bindings/Proxy.cpp",
              "s7shell/src/bindings/GatewaySync.cpp", "s7shell/src/bindings/S7Server.cpp",
              "s7shell/src/bindings/Gateway.cpp", "s7shell/src/bindings/HttpServer.cpp",
              "s7shell/src/bindings/WebSocketServer.cpp", "s7shell/src/connection/ProxySession.cpp",
              "s7shell/src/connection/GatewaySync.cpp", "s7shell/src/connection/S7ShellServer.cpp",
              "s7shell/src/utils/json-helpers.cpp")
    if has_config("opc") then
        add_files("s7shell/src/connection/OpcUaShellServer.cpp", "s7shell/src/bindings/OpcUaServer.cpp")
        add_defines("SGRN_HAS_OPC", {public = true})
        add_deps("sgrn_gateway_opcua")
    end
    add_includedirs("s7shell", "s7shell/include", "gateway/include", {public = true})
    add_includedirs("$(projectdir)", "scl/include")
    add_deps("sgrn_gateway", "sgrn_gateway_twin", "sgrn_gateway_s7", "sgrn_plcsim", "sgrn_scl", "sgrn_scripting")
    add_packages("angelscript", "ixwebsocket", "cxxopts", {public = true})
    add_packages("cpp-httplib")

-- ── 7. ML ───────────────────────────────────────────────────────────────────
target("sgrn_ml")
    set_kind("$(kind)")
    add_files("ml/src/**.cpp")
    add_includedirs("ml/include", {public = true})
    add_deps("sgrn_core", "sgrn_utils", "sgrn_scl", "sgrn_s7codec")
    add_packages("fmt", {public = true})
    add_rules("sgrn.pch")
    set_values("sgrn.pch", "third")

target("sgrn_ml_bridge")
    set_kind("$(kind)")
    add_files("ml_bridge/src/**.cpp")
    add_includedirs("ml_bridge/include", {public = true})
    add_deps("sgrn_ml", "sgrn_core", "sgrn_utils", "sgrn_gateway_twin", "sgrn_s7codec")
    add_packages("fmt", {public = true})
    add_rules("sgrn.pch")
    set_values("sgrn.pch", "third")

if has_config("torch") then
    target("sgrn_ml_torch")
        set_kind("$(kind)")
        add_files("ml_torch/src/**.cpp")
        add_includedirs("ml_torch/include", {public = true})
        add_deps("sgrn_ml", "sgrn_core", "sgrn_utils")
        add_packages("fmt", "sgrn_torch", {public = true})
        if get_config("torch_device") == "gpu" then
            add_defines("TORCH_USE_GPU", {public = true})
        elseif get_config("torch_device") == "cpu" then
            add_defines("TORCH_USE_CPU", {public = true})
        end
        add_rules("sgrn.pch")
        set_values("sgrn.pch", "third")
end

-- datastore wire / client / shell (+ backend when enabled)
includes("datastore")

-- ── tests ───────────────────────────────────────────────────────────────────
sgrn_test("s7codec_example_test", "codecs/s7codec/example-test.cpp", {deps = {"sgrn_s7codec"}, flags = {"-UNDEBUG"}})

sgrn_test("glaze_json_test", "utils/tests/glaze-json-test.cpp", {deps = {"sgrn_utils"}, packages = {"fmt", "glaze"}})
sgrn_test("compression_test", "utils/tests/compression-test.cpp", {deps = {"sgrn_utils"}, packages = {"fmt", "zstd"}})
sgrn_test("delta_test", "utils/tests/delta-test.cpp", {deps = {"sgrn_utils"}, packages = {"fmt", "zstd"}})
sgrn_test("crypto_test", "utils/tests/crypto-test.cpp", {deps = {"sgrn_utils"}, packages = {"fmt", "openssl"}})

sgrn_test("schema_binary_codec_test", "scl/tests/schema-binary-codec-test.cpp", {deps = {"sgrn_scl"}, inc = {"scl/include"}})
sgrn_test("scl_directives_test", "scl/tests/scl-directives-test.cpp", {deps = {"sgrn_scl"}, inc = {"scl/include"}})
sgrn_test("scl_scalar_alias_test", "scl/tests/scl-scalar-alias-tests.cpp", {deps = {"sgrn_scl"}, inc = {"scl/include"}})

sgrn_test("plc_tagtable_test", "plcsim/tests/plc-tagtable-test.cpp",
    {deps = {"sgrn_plcsim", "sgrn_scl", "sgrn_utils", "sgrn_common"}, packages = {"fmt"},
     inc = {"plcsim/include", "scl/include", "common/include"}})

sgrn_test("wal_replayer_test", "s7shell/tests/wal-replayer-test.cpp", {deps = {"sgrn_s7shell", "sgrn_utils"}})
sgrn_test("init_script_test", "s7shell/tests/init-script-test.cpp", {deps = {"sgrn_s7shell"}})
sgrn_test("schema_reload_hook_test", "s7shell/tests/schema-reload-hook-test.cpp", {deps = {"sgrn_s7shell", "sgrn_plcsim"}})
sgrn_test("angel_concurrency_test", "s7shell/tests/angel-concurrency-test.cpp", {deps = {"sgrn_s7shell"}})

for _, t in ipairs({"feature-spec-test", "manifest-test", "extractor-test", "engine-test", "eval-test",
                    "time-grid-resampler-test", "columnar-dataset-test"}) do
    sgrn_test((t:gsub("-", "_")), "ml/tests/" .. t .. ".cpp", {deps = {"sgrn_ml", "sgrn_core", "sgrn_utils"}, packages = {"fmt"}})
end
sgrn_test("twin_bridge_test", "ml_bridge/tests/twin-bridge-test.cpp", {deps = {"sgrn_ml_bridge", "sgrn_core", "sgrn_utils"}, packages = {"fmt"}})
sgrn_test("pipeline_e2e_test", "ml_bridge/tests/pipeline-e2e-test.cpp", {deps = {"sgrn_ml_bridge", "sgrn_core", "sgrn_utils"}, packages = {"fmt"}})

if has_config("torch") then
    for _, t in ipairs({"torch-smoke-test", "mlp-test", "torch-backend-test", "trainer-test"}) do
        sgrn_test((t:gsub("-", "_")), "ml_torch/tests/" .. t .. ".cpp",
                  {deps = {"sgrn_ml_torch", "sgrn_ml", "sgrn_core", "sgrn_utils"}, packages = {"fmt", "sgrn_torch"}})
    end
end

-- gateway tests
local gw = {"gateway/include", "scl/include"}
sgrn_test("runtime_sync_protocol_test", "gateway/tests/runtime-sync-protocol-test.cpp",
    {deps = {"sgrn_utils"}, inc = {"gateway/include", "core/include"}})
sgrn_test("opcua_simulation_enum_projection_test", "gateway/tests/opcua-simulation-enum-projection-test.cpp",
    {deps = {"sgrn_gateway_opcua", "sgrn_scl"}, inc = gw})
sgrn_test("adapter_ports_test", "gateway/tests/adapter-ports-test.cpp",
    {deps = {"sgrn_gateway_s7", "sgrn_gateway_modbus", "sgrn_gateway_ethernetip", "sgrn_common"},
     packages = {"open62541", "fmt"}, inc = gw})
sgrn_test("twin_ports_test", "gateway/tests/twin-ports-test.cpp",
    {deps = {"sgrn_gateway_ports", "sgrn_gateway_twin", "sgrn_common"}, inc = gw})
sgrn_test("twin_encoding_wire_test", "gateway/tests/twin-encoding-wire-test.cpp",
    {deps = {"sgrn_gateway_twin", "sgrn_common", "sgrn_scl"}, inc = {"gateway/include", "scl/include", "common/include"}})
sgrn_test("plc_memory_init_test", "gateway/tests/plc-memory-init-test.cpp",
    {deps = {"sgrn_gateway_twin", "sgrn_common", "sgrn_scl"}, inc = {"gateway/include", "scl/include", "common/include"}})
sgrn_test("dirty_wakeup_test", "gateway/tests/dirty-wakeup-test.cpp",
    {deps = {"sgrn_gateway_twin"}, inc = gw, defines = {"ASIO_STANDALONE"}})
sgrn_test("persistence_binary_test", "gateway/tests/persistence-binary-test.cpp",
    {deps = {"sgrn_gateway", "sgrn_gateway_twin"}, inc = gw, defines = {"ASIO_STANDALONE"}})
sgrn_test("process_commands_order_test", "gateway/tests/process-commands-order-test.cpp",
    {deps = {"sgrn_gateway_twin"}, inc = gw, defines = {"ASIO_STANDALONE"}})
sgrn_test("opcua_projection_test", "gateway/tests/opcua-projection-test.cpp",
    {deps = {"sgrn_gateway_opcua", "sgrn_scl", "sgrn_wrappers_opcua"}, packages = {"open62541"}, inc = gw})

if not winlibs and has_config("tests") then
    local live = {"jsoncpp", "reproc"}
    sgrn_test("opcua_discovery_test", "gateway/tests/opcua-discovery-test.cpp",
        {deps = {"sgrn_wrappers_opcua"}, packages = {"open62541", "jsoncpp", "reproc"}, inc = {"gateway/include"}, support = true})
    sgrn_test("opcua_enum_test", "gateway/tests/opcua-enum-test.cpp",
        {deps = {"sgrn_wrappers_opcua"}, packages = {"open62541", "jsoncpp", "reproc"}, inc = {"gateway/include"}, support = true})
    sgrn_test("gateway_rest_api_test", "gateway/tests/gateway-rest-api-test.cpp",
        {packages = live, inc = {"gateway/include"}, support = true})
    sgrn_test("gateway_array_support_test", "gateway/tests/gateway-array-support-test.cpp",
        {packages = live, inc = {"gateway/include"}, support = true})
    sgrn_test("gateway_policy_test", "gateway/tests/gateway-policy-test.cpp",
        {packages = live, inc = {"gateway/include"}, support = true})
    sgrn_test("modbus_registers_test", "gateway/tests/modbus-registers-test.cpp",
        {deps = {"sgrn_wrappers_modbus"}, packages = live, inc = {"gateway/include"}, support = true})
    sgrn_test("s7_southbound_test", "gateway/tests/s7-southbound-test.cpp",
        {deps = {"sgrn_wrappers_s7"}, packages = live, inc = {"gateway/include"}, support = true})
    sgrn_test("binary_live_snapshot_test", "gateway/tests/binary-live-snapshot-test.cpp",
        {packages = {"ixwebsocket", "zlib", "openssl", "jsoncpp", "reproc"}, inc = {"gateway/include"}, support = true})

    local pipe_deps = {"sgrn_ml", "sgrn_dataset", "sgrn_replay"}
    local pipe_bins = {{"SGRN_DATASET_BIN", "sgrn_dataset"}, {"SGRN_REPLAY_BIN", "sgrn_replay"}}
    if has_config("torch") then
        table.insert(pipe_deps, "sgrn_train")
        table.insert(pipe_bins, {"SGRN_TRAIN_BIN", "sgrn_train"})
    end
    sgrn_test("pipeline_cli_test", "ml/tests/pipeline-cli-test.cpp",
        {deps = pipe_deps, packages = {"fmt", "cpp-httplib", "reproc"}, support = true, bins = pipe_bins})
end
