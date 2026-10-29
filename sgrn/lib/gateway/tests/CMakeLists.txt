# sgrn/lib/gateway/tests — gateway test targets.
# ─────────────────────────────────────────────────────────────────────────────
# Extracted from the gateway CMakeLists.txt so the library target list stays
# readable. Defined as a subdirectory: sgrn_add_component_executable,
# sgrn_target and the global options are visible from the parent scope (see
# cmake/global.cmake, included before this directory is added).
#
# NOTE: `${CMAKE_CURRENT_SOURCE_DIR}` here is the tests/ directory itself —
# gateway include paths are therefore spelled with ${CMAKE_SOURCE_DIR}.
# =============================================================================
if(SGRN_BUILD_TESTS)
    # Header-only RuntimeSync framing test. This deliberately has no Crow or
    # network dependency so malformed SGRW frames are cheap to regression-test.
    sgrn_add_component_executable(
        TARGET runtime_sync_protocol_test
        SOURCES runtime_sync_protocol_test.cpp
        ENABLE_ORIGIN_RPATH
    )
    target_include_directories(runtime_sync_protocol_test PRIVATE
        ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
    )
    add_test(NAME runtime_sync_protocol_test COMMAND runtime_sync_protocol_test)
    sgrn_target(runtime_sync_protocol_test)

    sgrn_add_component_executable(
        TARGET opcua_simulation_enum_projection_test
        SOURCES opcua_simulation_enum_projection_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_gateway_opcua
            sgrn_scl
    )
    target_include_directories(opcua_simulation_enum_projection_test PRIVATE
        ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
        ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
    )
    add_test(NAME opcua_simulation_enum_projection_test COMMAND opcua_simulation_enum_projection_test)
    sgrn_target(opcua_simulation_enum_projection_test)

    # Adapter port decoupling test: drives the s7/modbus/ethernetip adapters
    # against fake sgrn::common ports. Deliberately links NO twin/security
    # library — a successful link proves the adapters are decoupled.
    sgrn_add_component_executable(
        TARGET adapter_ports_test
        SOURCES adapter_ports_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_gateway_s7
            sgrn_gateway_modbus
            sgrn_gateway_ethernetip
            sgrn::common
            sgrn::open62541
            fmt::fmt
    )
    target_include_directories(adapter_ports_test PRIVATE
        ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
        ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
    )
    add_test(NAME adapter_ports_test COMMAND adapter_ports_test)
    sgrn_target(adapter_ports_test)

    # Twin port glue test: TwinMemoryPort against a real PlcMemory, plus
    # full classify() coverage. Links the twin on purpose (tests the glue).
    sgrn_add_component_executable(
        TARGET twin_ports_test
        SOURCES twin_ports_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_gateway_ports
            sgrn_gateway_twin
            sgrn::common
    )
    target_include_directories(twin_ports_test PRIVATE
        ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
        ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
    )
    add_test(NAME twin_ports_test COMMAND twin_ports_test)
    sgrn_target(twin_ports_test)

    # Wire-level tests for the twin encoder/decoder layer migrated onto the
    # s7codec span/options API. Pure functions over DbField + byte buffers —
    # no network, no live PLC. Beats encodeFieldAt/applyJsonPatchToFields
    # against exact wire bytes, renders through the S7SerializationUtils
    # decode hop, and pins the pre-existing count/string_capacity and
    # enum-label conflation semantics so they cannot drift further.
    sgrn_add_component_executable(
        TARGET twin_encoding_wire_test
        SOURCES twin_encoding_wire_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_gateway_twin
            sgrn::common
            sgrn_scl
    )
    target_include_directories(twin_encoding_wire_test PRIVATE
        ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
        ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
        ${CMAKE_SOURCE_DIR}/sgrn/lib/common/include
    )
    add_test(NAME twin_encoding_wire_test COMMAND twin_encoding_wire_test)
    sgrn_target(twin_encoding_wire_test)

    # Store-bound coverage: `:=` init seeding through the fully public load
    # path (PlcSchemaStore -> PlcMemory::loadRegistry), verified via Tier-2
    # readDbMemory. Pins Bool/numeric/string/enum literals, skipped array
    # initializers, and the documented Bool last-wins + struct@0 quirks.
    sgrn_add_component_executable(
        TARGET plc_memory_init_test
        SOURCES plc_memory_init_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_gateway_twin
            sgrn::common
            sgrn_scl
    )
    target_include_directories(plc_memory_init_test PRIVATE
        ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
        ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
        ${CMAKE_SOURCE_DIR}/sgrn/lib/common/include
    )
    add_test(NAME plc_memory_init_test COMMAND plc_memory_init_test)
    sgrn_target(plc_memory_init_test)

    # Lost-wakeup regression test: producer threads hammer writeDbMemory()
    # while the heavy_pool dirty callback drains; every burst's final write
    # must be observed (no orphaned dirty batch). Links the twin on purpose.
    sgrn_add_component_executable(
        TARGET dirty_wakeup_test
        SOURCES dirty_wakeup_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_gateway_twin
    )
    target_include_directories(dirty_wakeup_test PRIVATE
        ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
        ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
    )
    target_compile_definitions(dirty_wakeup_test PRIVATE ASIO_STANDALONE)
    add_test(NAME dirty_wakeup_test COMMAND dirty_wakeup_test)
    sgrn_target(dirty_wakeup_test)

    # Binary WAL regression test: multi-DB DeltaSnapshot events (db == 0,
    # dirty_dbs set) must produce anchor/delta/keyframe frames, and the
    # archive must round-trip through recoverStateFromArchives().
    sgrn_add_component_executable(
        TARGET persistence_binary_test
        SOURCES persistence_binary_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_gateway
            sgrn_gateway_twin
    )
    target_include_directories(persistence_binary_test PRIVATE
        ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
        ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
    )
    target_compile_definitions(persistence_binary_test PRIVATE ASIO_STANDALONE)
    add_test(NAME persistence_binary_test COMMAND persistence_binary_test)
    sgrn_target(persistence_binary_test)

    # Deterministic DB-write-ordering test: one processCommands() call over
    # scrambled DBs must invoke the batch callback in ascending DB order.
    sgrn_add_component_executable(
        TARGET process_commands_order_test
        SOURCES process_commands_order_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_gateway_twin
    )
    target_include_directories(process_commands_order_test PRIVATE
        ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
        ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
    )
    target_compile_definitions(process_commands_order_test PRIVATE ASIO_STANDALONE)
    add_test(NAME process_commands_order_test COMMAND process_commands_order_test)
    sgrn_target(process_commands_order_test)

    # OPC-UA projection regression for scalar-alias SCL types (assert-based
    # harness, was an unregistered orphan source).
    sgrn_add_component_executable(
        TARGET opcua_projection_test
        SOURCES opcua_projection_test.cpp
        ENABLE_ORIGIN_RPATH
        PRIVATE_LIBS
            sgrn_gateway_opcua
            sgrn_scl
            sgrn::wrappers::opcua
            sgrn::open62541
    )
    target_include_directories(opcua_projection_test PRIVATE
        ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
        ${CMAKE_SOURCE_DIR}/sgrn/lib/scl/include
    )
    add_test(NAME opcua_projection_test COMMAND opcua_projection_test)
    sgrn_target(opcua_projection_test)

    # The integration tests below use gateway_fixture.hpp / test_util.hpp, which
    # rely on POSIX process spawning (fork/waitpid/execvp) and sockets.
    # Exclude them when cross-compiling for Windows.
    if(NOT WIN32 AND NOT MINGW AND NOT SGRN_CROSS_WIN64)
        # Live-gateway OPC-UA discovery (ported from tests/gateway/opcua_discovery.py):
        sgrn_add_component_executable(
            TARGET opcua_discovery_test
            SOURCES opcua_discovery_test.cpp
            ENABLE_ORIGIN_RPATH
            PRIVATE_LIBS
                sgrn::wrappers::opcua
                sgrn::open62541
                sgrn::jsoncpp
        )
        target_include_directories(opcua_discovery_test PRIVATE
            ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
            ${CMAKE_SOURCE_DIR}/sgrn/lib/tests/support
            ${CMAKE_SOURCE_DIR}/.prefix/include/httplib
        )
        add_test(NAME opcua_discovery_test COMMAND opcua_discovery_test)
        sgrn_target(opcua_discovery_test)

        # Live-gateway #ENUM round-trip (ported from tests/gateway/enum_tests.py):
        sgrn_add_component_executable(
            TARGET opcua_enum_test
            SOURCES opcua_enum_test.cpp
            ENABLE_ORIGIN_RPATH
            PRIVATE_LIBS
                sgrn::wrappers::opcua
                sgrn::open62541
                sgrn::jsoncpp
        )
        target_include_directories(opcua_enum_test PRIVATE
            ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
            ${CMAKE_SOURCE_DIR}/sgrn/lib/tests/support
            ${CMAKE_SOURCE_DIR}/.prefix/include/httplib
        )
        add_test(NAME opcua_enum_test COMMAND opcua_enum_test)
        sgrn_target(opcua_enum_test)

        # Live-gateway REST API test (ported from tests/gateway/rest_api.py & tests/ts/tests/data.test.ts):
        sgrn_add_component_executable(
            TARGET gateway_rest_api_test
            SOURCES gateway_rest_api_test.cpp
            ENABLE_ORIGIN_RPATH
            PRIVATE_LIBS
                sgrn::jsoncpp
        )
        target_include_directories(gateway_rest_api_test PRIVATE
            ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
            ${CMAKE_SOURCE_DIR}/sgrn/lib/tests/support
            ${CMAKE_SOURCE_DIR}/.prefix/include/httplib
        )
        add_test(NAME gateway_rest_api_test COMMAND gateway_rest_api_test)
        sgrn_target(gateway_rest_api_test)

        # Live-gateway array support test (ported from tests/gateway/array_support.py):
        sgrn_add_component_executable(
            TARGET gateway_array_support_test
            SOURCES gateway_array_support_test.cpp
            ENABLE_ORIGIN_RPATH
            PRIVATE_LIBS
                sgrn::jsoncpp
        )
        target_include_directories(gateway_array_support_test PRIVATE
            ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
            ${CMAKE_SOURCE_DIR}/sgrn/lib/tests/support
            ${CMAKE_SOURCE_DIR}/.prefix/include/httplib
        )
        add_test(NAME gateway_array_support_test COMMAND gateway_array_support_test)
        sgrn_target(gateway_array_support_test)

        # Live-gateway policy & registry test (ported from tests/ts/tests/policy.test.ts & registry.test.ts):
        sgrn_add_component_executable(
            TARGET gateway_policy_test
            SOURCES gateway_policy_test.cpp
            ENABLE_ORIGIN_RPATH
            PRIVATE_LIBS
                sgrn::jsoncpp
        )
        target_include_directories(gateway_policy_test PRIVATE
            ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
            ${CMAKE_SOURCE_DIR}/sgrn/lib/tests/support
            ${CMAKE_SOURCE_DIR}/.prefix/include/httplib
        )
        add_test(NAME gateway_policy_test COMMAND gateway_policy_test)
        sgrn_target(gateway_policy_test)

        # Live-gateway Modbus TCP wire test:
        sgrn_add_component_executable(
            TARGET modbus_registers_test
            SOURCES modbus_registers_test.cpp
            ENABLE_ORIGIN_RPATH
            PRIVATE_LIBS
                sgrn::wrappers::modbus
                sgrn::jsoncpp
        )
        target_include_directories(modbus_registers_test PRIVATE
            ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
            ${CMAKE_SOURCE_DIR}/sgrn/lib/tests/support
            ${CMAKE_SOURCE_DIR}/.prefix/include/httplib
        )
        add_test(NAME modbus_registers_test COMMAND modbus_registers_test)
        sgrn_target(modbus_registers_test)

        # Live-gateway S7comm wire test:
        sgrn_add_component_executable(
            TARGET s7_southbound_test
            SOURCES s7_southbound_test.cpp
            ENABLE_ORIGIN_RPATH
            PRIVATE_LIBS
                sgrn::wrappers::s7
                sgrn::jsoncpp
        )
        target_include_directories(s7_southbound_test PRIVATE
            ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
            ${CMAKE_SOURCE_DIR}/sgrn/lib/tests/support
            ${CMAKE_SOURCE_DIR}/.prefix/include/httplib
        )
        add_test(NAME s7_southbound_test COMMAND s7_southbound_test)
        sgrn_target(s7_southbound_test)

        # WebSocket JSON/binary parity test:
        sgrn_add_component_executable(
            TARGET binary_live_snapshot_test
            SOURCES binary_live_snapshot_test.cpp
            ENABLE_ORIGIN_RPATH
            PRIVATE_LIBS
                ixwebsocket::ixwebsocket
                sgrn::zlib
                sgrn::openssl
                sgrn::jsoncpp
        )
        target_include_directories(binary_live_snapshot_test PRIVATE
            ${CMAKE_SOURCE_DIR}/sgrn/lib/gateway/include
            ${CMAKE_SOURCE_DIR}/sgrn/lib/tests/support
            ${CMAKE_SOURCE_DIR}/.prefix/include/httplib
        )
        add_test(NAME binary_live_snapshot_test COMMAND binary_live_snapshot_test)
        sgrn_target(binary_live_snapshot_test)
    endif()
endif()

