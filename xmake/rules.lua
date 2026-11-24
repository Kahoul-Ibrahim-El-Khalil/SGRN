-- xmake/rules.lua
-- Project-wide rules replacing cmake/global.cmake, cmake/build_hardening.cmake,
-- cmake/pch.cmake, and cmake/staging_sgrn.cmake.

local SANITIZER_FLAGS = {
    ["asan"]       = {"-fsanitize=address", "-fno-omit-frame-pointer"},
    ["ubsan"]      = {"-fsanitize=undefined", "-fno-omit-frame-pointer"},
    ["tsan"]       = {"-fsanitize=thread"},
    ["asan,ubsan"] = {"-fsanitize=address,undefined", "-fno-omit-frame-pointer"},
    ["ubsan,asan"] = {"-fsanitize=address,undefined", "-fno-omit-frame-pointer"},
}

local function _apply_msvc_flags(target)
    target:add("cxflags", "/W3", "/utf-8", {force = true})
    if has_config("native") and is_mode("release") then
        target:add("cxflags", "/arch:AVX2", {force = true})
    end
end

local function _apply_gnu_diagnostics(target, win)
    target:add("cxflags", "-Wall", "-Wextra", "-Wpedantic", "-Wshadow",
               "-Wno-missing-field-initializers", "-Wno-missing-braces")
    target:add("cxxflags", "-Wnon-virtual-dtor")
    if target:has_tool("cxx", "clang", "clangxx") then
        target:add("cxxflags", "-Wno-deprecated-literal-operator")
    end
    if not win then
        target:add("cxflags", "-fPIC")
        target:add("rpathdirs", "$ORIGIN")
    end
end

local function _apply_hardening_flags(target, win)
    local kind = target:kind()
    target:add("cxflags", "-fstack-protector-strong", "-Wformat", "-Wformat-security")
    if not win then
        target:add("cxflags", "-fstack-clash-protection")
        if kind == "binary" or kind == "shared" then
            target:add("ldflags", "-Wl,-z,relro,-z,now", {force = true})
            if kind == "shared" then
                target:add("shflags", "-Wl,-z,relro,-z,now", {force = true})
            end
        end
    end
    if target:is_arch("x86_64", "x64", "i386", "x86") then
        target:add("cxflags", "-fcf-protection")
    end
    if is_mode("release") then
        if not win then target:add("defines", "_FORTIFY_SOURCE=3") end
        if has_config("native") then target:add("cxflags", "-march=native") end
    end
end

local function _apply_sanitizer_flags(target)
    local san = get_config("sanitize")
    if type(san) ~= "string" or san == "" then return end
    san = san:lower():gsub("%s", ""):gsub("%+", ",")
    local flags = SANITIZER_FLAGS[san]
    if not flags then
        raise("sanitize='%s': expected asan, ubsan, tsan or asan,ubsan (TSan is exclusive with ASan/UBSan)", san)
    end
    target:add("cxflags", flags, {force = true})
    target:add("ldflags", flags, {force = true})
    target:add("shflags", flags, {force = true})
end

local function _stage_target(target)
    import("sgrn.platform")
    local kind = target:kind()
    local file = target:targetfile()
    if not file or not os.isfile(file) then return end
    local root = os.projectdir()
    if kind == "static" or kind == "shared" then
        local dir = path.join(root, ".prefix", platform.dirname(), "lib")
        os.mkdir(dir)
        os.cp(file, dir)
        import("sgrn.stage").headers()
    elseif kind == "binary" then
        local dir = path.join(root, ".dist", platform.dirname(), "bin")
        os.mkdir(dir)
        local dest_bin = path.join(dir, path.filename(file))
        -- Unlink first so Linux does not fail with ETXTBSY if the staged binary is currently running.
        os.tryrm(dest_bin)
        os.cp(file, dest_bin)
        import("sgrn.harvester").harvest_binary(dest_bin, dir)
    end
end

-- ── sgrn.base: applied to every SGRN target ──────────────────────────────────
rule("sgrn.base")
    on_load(function (target)
        local root = os.projectdir()
        local root_inc = path.join(root, "include")
        if os.isdir(root_inc) then
            target:add("includedirs", root_inc, {public = true})
        end
        if is_plat("windows") then
            target:add("defines", "_WIN32_WINNT=0x0A00", "WIN32_LEAN_AND_MEAN", "NOMINMAX", {public = true})
        end
        if is_plat("mingw") then
            target:add("defines", "_WIN32_WINNT=0x0A00", "WIN32_LEAN_AND_MEAN", "NOMINMAX", "_UCRT", {public = true})
            target:add("syslinks", "ucrt")
        end
        if target:kind() == "shared" then
            target:add("defines", "SGRN_BUILDING_SHARED")
        end
    end)

    on_config(function (target)
        local win = is_plat("windows", "mingw")
        local msvc = target:has_tool("cxx", "cl", "clang_cl")
        if msvc then
            _apply_msvc_flags(target)
            return
        end
        if win and is_mode("release") then
            -- Mingw: silence third-party header noise; keep warnings on Linux.
            target:add("cxflags", "-w", {force = true})
        end
        if not target:has_tool("cxx", "gcc", "gxx", "clang", "clangxx") then return end

        _apply_gnu_diagnostics(target, win)
        _apply_hardening_flags(target, win)
        _apply_sanitizer_flags(target)
    end)

    after_build(_stage_target)

-- ── sgrn.pch: opt-in precompiled headers (`xmake f --pch=y`) ────────────────
-- Usage: add_rules("sgrn.pch"); set_values("sgrn.pch", "s7")   -- std|third|s7|net
rule("sgrn.pch")
    on_load(function (target)
        if not has_config("pch") then return end
        local tier = target:values("sgrn.pch")
        if type(tier) == "table" then tier = tier[1] end
        tier = tier or "std"
        local third = {"fmt", "jsoncpp", "openssl", "zstd", "cxxopts", "sqlite_modern_cpp", "glaze",
                       "cpp-httplib", "crow", "angelscript", "ixwebsocket", "asio"}
        local pkgs = {}
        if tier == "third" or tier == "s7" or tier == "net" then pkgs = table.clone(third) end
        if tier == "s7" then table.join2(pkgs, {"snap7", "open62541", "xml_h"}) end
        if tier == "net" then table.insert(pkgs, "drogon") end
        for _, p in ipairs(pkgs) do target:add("packages", p) end
        target:set("pcxxheader", path.join(os.projectdir(), "xmake", "pch", "sgrn_pch_" .. tier .. ".hpp"))
    end)

-- ── sgrn.datastore: flags the former sgrn_datastore_core interface carried ──
rule("sgrn.datastore")
    on_config(function (target)
        if target:has_tool("cxx", "gxx", "clangxx") then
            target:add("cxflags", "-Wno-unused-parameter", "-Wno-unused-variable")
            if target:has_tool("cxx", "clangxx") and target:has_cxxflags("-fcoro-frame-pointer") then
                target:add("cxflags", "-fcoro-frame-pointer") -- LLVM #56493 mitigation
            end
        end
        local dbg = get_config("datastore_debug")
        if type(dbg) == "string" and dbg ~= "" then
            for flag in dbg:gmatch("[^,%s]+") do
                target:add("defines", flag .. "=1")
            end
        end
        if has_config("datastore_release") then
            target:add("cxflags", "-O3", "-DNDEBUG", "-flto", {force = true})
            target:add("ldflags", "-flto", "-fuse-ld=lld", {force = true})
        end
    end)
