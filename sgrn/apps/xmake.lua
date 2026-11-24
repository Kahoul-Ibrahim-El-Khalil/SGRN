-- sgrn/apps/xmake.lua : SGRN executables (target names match the former CMake ones)

local winlibs = {"ws2_32", "winmm", "rpcrt4", "crypt32", "advapi32", "iphlpapi",
                 "secur32", "wldap32", "bcrypt", "userenv", "mswsock"}

local function sgrn_app(name)
    target(name)
        set_kind("binary")
        if is_plat("windows", "mingw") then
            add_syslinks(table.unpack(winlibs))
        else
            add_syslinks("pthread")
        end
end

-- ── gateway group ───────────────────────────────────────────────────────────
sgrn_app("gateway")
    add_files("gateway/main.cpp")
    add_includedirs("../lib/gateway/include", "../lib/scl/include")
    add_deps("sgrn_gateway", "sgrn_gateway_modbus", "sgrn_gateway_ethernetip", "sgrn_utils")
    add_packages("cxxopts", "zstd", "openssl", "opener")
    add_rules("sgrn.pch")
    set_values("sgrn.pch", "s7")

sgrn_app("mbproxy")
    add_files("mbproxy/main.cpp")
    add_includedirs("../lib/gateway/include", "../lib/scl/include")
    add_deps("sgrn_gateway", "sgrn_wrappers_modbus", "sgrn_utils")
    add_packages("asio", "fmt")
    add_defines("ASIO_STANDALONE")
    add_rules("sgrn.pch")
    set_values("sgrn.pch", "s7")

sgrn_app("sgrn_wal_dump")
    add_files("wal_dump/main.cpp")
    add_includedirs("../lib/gateway/include", "../lib/scl/include")
    add_deps("sgrn_gateway_twin", "sgrn_utils")

sgrn_app("sgrn_dataset")
    add_files("dataset/main.cpp",
              "../lib/gateway/src/tools/sgrn-dataset.cpp",
              "../lib/gateway/src/tools/sgrn-dataset-transcode.cpp",
              "../lib/gateway/src/tools/sgrn-dataset-merge.cpp")
    add_includedirs("../lib/gateway/include", "../lib/scl/include", "../lib/ml/include")
    add_deps("sgrn_ml", "sgrn_gateway_twin", "sgrn_scl", "sgrn_utils", "sgrn_common")
    add_packages("asio", "zstd", "cxxopts", "fmt", "jsoncpp")
    add_defines("ASIO_STANDALONE")

sgrn_app("sgrn_replay")
    add_files("replay/main.cpp",
              "../lib/gateway/src/tools/sgrn-replay.cpp",
              "../lib/gateway/src/gateway.cpp")
    add_includedirs("../lib/gateway/include", "../lib/scl/include", "../lib/ml/include")
    add_deps("sgrn_ml", "sgrn_gateway", "sgrn_gateway_twin", "sgrn_scl", "sgrn_utils")
    add_packages("zstd", "cxxopts", "fmt", "jsoncpp")

sgrn_app("sclc")
    add_files("sclc/main.cpp")
    add_includedirs("../lib/scl/include")
    add_deps("sgrn_scl", "sgrn_core", "sgrn_utils")
    add_packages("cxxopts")

-- ── s7shell ─────────────────────────────────────────────────────────────────
sgrn_app("s7shell")
    add_files("s7shell/main.cpp")
    add_includedirs("$(projectdir)", "../lib/scl/include", "../lib/gateway/include")
    add_deps("sgrn_core", "sgrn_s7shell", "sgrn_utils")
    add_packages("cxxopts")
    if not is_plat("windows", "mingw") then
        add_packages("readline")
    end

-- ── ML ──────────────────────────────────────────────────────────────────────
sgrn_app("sgrn_ml_eval")
    add_files("ml_eval/main.cpp")
    add_deps("sgrn_ml", "sgrn_utils", "sgrn_core")
    add_packages("cxxopts", "fmt")
    if has_config("torch") then
        add_deps("sgrn_ml_torch")
        add_packages("sgrn_torch")
        add_defines("SGRN_HAS_TORCH=1")
    end

if has_config("torch") then
    sgrn_app("sgrn_train")
        add_files("train/main.cpp")
        add_deps("sgrn_ml_torch", "sgrn_ml", "sgrn_utils", "sgrn_core")
        add_packages("cxxopts", "fmt", "sgrn_torch")
end

-- ── datastore ───────────────────────────────────────────────────────────────
if has_config("datastore") then
    sgrn_app("sgrn_datastore")
        add_files("datastore/main.cpp")
        add_includedirs("$(projectdir)/$(buildir)/gen/datastore/dashboard",
                        "$(projectdir)/$(buildir)/gen/datastore/sql",
                        "$(projectdir)/$(buildir)/gen/datastore/config")
        add_deps("sgrn_datastore_lib")
        -- keep Drogon controller/plugin static initializers (former --whole-archive)
        add_linkgroups("sgrn_datastore_lib", {whole = true})
        add_rules("sgrn.datastore", "sgrn.pch")
        set_values("sgrn.pch", "net")
        before_build(function (target)
            import("sgrn.assets")
            assets.datastore_dashboard()
            assets.datastore_sql()
            assets.datastore_config()
        end)

    sgrn_app("datastore_shell")
        add_files("datastore_shell/main.cpp")
        add_deps("sgrn_datastore_shell")
        add_rules("sgrn.datastore")
        if not is_plat("windows", "mingw") then
            add_packages("readline")
        end
end
