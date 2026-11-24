-- sgrn/lib/datastore/xmake.lua
-- wire / client / shell are always built; the backend (core, ORM, lib) only
-- with `--datastore=y`.

local function sgrn_test(name, file, opt)
    if not has_config("tests") then return end
    opt = opt or {}
    target(name)
        set_kind("binary")
        set_group("tests")
        add_files(file)
        add_deps(table.unpack(opt.deps or {"sgrn_datastore_lib"}))
        if opt.packages then add_packages(table.unpack(opt.packages)) end
        if opt.support then add_includedirs("$(projectdir)/sgrn/lib/tests/support") end
        if opt.srcdir then add_defines('SGRN_TEST_SOURCE_DIR="$(projectdir)"') end
        add_rules("sgrn.datastore")
        add_tests("default")
    target_end()
end

target("sgrn_datastore_wire")
    set_kind("static")
    add_files("wire/src/wire.cpp")
    add_includedirs("wire/include", {public = true})
    add_deps("sgrn_utils")

target("sgrn_datastore_client")
    set_kind("static")
    add_files("client/src/Client.cpp", "client/src/StorageClient.cpp", "client/src/client-json.cpp")
    add_includedirs("client/include", {public = true})
    add_deps("sgrn_core", "sgrn_utils", "sgrn_datastore_wire")
    add_packages("fmt", "cpp-httplib", {public = true})

target("sgrn_datastore_shell")
    set_kind("static")
    add_files("shell/src/DatastoreShell.cpp", "shell/src/connection.cpp", "shell/src/path.cpp",
              "shell/src/pipeline.cpp", "shell/src/angelscript-bindings.cpp", "shell/src/dispatch.cpp",
              "shell/src/print.cpp", "shell/src/commands/cmd-admin-accounts.cpp",
              "shell/src/commands/cmd-admin-sessions.cpp", "shell/src/commands/cmd-auth.cpp",
              "shell/src/commands/cmd-filesystem.cpp", "shell/src/commands/cmd-session.cpp",
              "shell/src/commands/cmd-storage.cpp", "shell/src/commands/cmd-upload.cpp",
              "shell/src/commands/cmd-versions.cpp")
    add_includedirs("shell/include", {public = true})
    add_deps("sgrn_core", "sgrn_datastore_client", "sgrn_utils")
    add_packages("fmt", {public = true})
    if not is_plat("windows", "mingw") then
        add_packages("readline", {public = true})
    end

if has_config("tests") then
    target("wire_decode_benchmark")
        set_kind("binary")
        set_group("tests")
        add_files("client/test/wire-decode-benchmark.cpp")
        add_deps("sgrn_datastore_wire")
        add_packages("fmt")
        add_tests("default")
    target_end()
end

if has_config("datastore") then
    target("sgrn_datastore_orm")
        set_kind("static")
        add_files("src/orm/models/**.cc", {cxxflags = "-w"})
        add_includedirs("src", {public = true})
        add_deps("sgrn_core", "sgrn_utils")
        add_packages("drogon", "jsoncpp", "fmt", "openssl", "zstd", "libpq", {public = true})
        add_rules("sgrn.datastore")
        before_build(function (target)
            if has_config("auto_codegen") then
                import("sgrn.codegen").auto()
            end
        end)

    target("sgrn_datastore_lib")
        set_kind("static")
        add_files("src/session/**.cpp", "src/audit/**.cpp", "src/plugins/**.cpp", "src/services/**.cpp",
                  "src/filters/**.cpp", "src/handlers/**.cpp", "src/utils/**.cpp", "src/bootstrap/**.cpp",
                  "src/crypto/**.cpp", "src/query/*.cpp")
        add_includedirs("src", "include", {public = true})
        add_includedirs("$(projectdir)/$(buildir)/gen/datastore/sql",
                        "$(projectdir)/$(buildir)/gen/datastore/config", {public = true})
        add_deps("sgrn_core", "sgrn_utils", "sgrn_datastore_orm", "sgrn_datastore_wire")
        add_packages("drogon", "libpq", "jsoncpp", "fmt", "openssl", "zstd",
                     "c-ares", "libuuid", "sqlite3", {public = true})
        add_rules("sgrn.datastore")
        before_build(function (target)
            import("sgrn.assets")
            assets.datastore_sql()
            assets.datastore_config()
        end)

    sgrn_test("crud_binds_test", "tests/crud-binds-test.cpp")
    sgrn_test("rate_limit_test", "tests/rate-limit-test.cpp")
    sgrn_test("system_config_test", "tests/system-config-test.cpp")
    sgrn_test("pg_auth_functions_test", "tests/pg-auth-functions-test.cpp", {packages = {"reproc"}, support = true})
    sgrn_test("backup_test", "tests/backup-test.cpp")
    sgrn_test("verify_test", "tests/verify-test.cpp", {packages = {"openssl"}})
    sgrn_test("asan_scratch_test", "tests/asan-scratch-test.cpp")
    sgrn_test("cross_org_auth_test", "tests/cross-org-auth-test.cpp")
    sgrn_test("admin_support_schema_test", "tests/admin-support-schema-test.cpp", {packages = {"reproc"}, support = true})
    sgrn_test("scoped_admins_test", "tests/scoped-admins-test.cpp", {packages = {"reproc"}, support = true, srcdir = true})
    sgrn_test("authz_matrix_test", "tests/authz-matrix-test.cpp", {packages = {"reproc"}, support = true, srcdir = true})
    sgrn_test("audit_tamper_test", "tests/audit-tamper-test.cpp", {packages = {"reproc", "openssl"}, support = true, srcdir = true})
    sgrn_test("encryption_test", "tests/encryption-test.cpp", {packages = {"reproc", "openssl"}, support = true, srcdir = true})
    sgrn_test("crud_admin_lockdown_test", "tests/crud-admin-lockdown-test.cpp", {srcdir = true})
end
