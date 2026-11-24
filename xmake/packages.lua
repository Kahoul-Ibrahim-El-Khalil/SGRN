-- xmake/packages.lua
-- Package recipes (niche libraries) and dependency declarations.
-- Replaces cmake/deps*.cmake, cmake/deps/*.cmake and deps/CMakeLists.txt.

includes("packages/snap7.lua")
includes("packages/angelscript.lua")
includes("packages/opener.lua")
includes("packages/sqlite_modern_cpp.lua")
includes("packages/open62541.lua")
includes("packages/drogon.lua")
includes("packages/libmodbus.lua")
includes("packages/xml_h.lua")
includes("packages/reproc.lua")
includes("packages/sgrn_torch.lua")
includes("packages/libpq.lua")
includes("packages/readline.lua")

-- Versions come from deps-versions.lua ("latest" = no version constraint).
local versions = SGRN_DEPS or {}
local function req(name, extra)
    local v = versions[name]
    local spec = name
    if type(v) == "string" and v ~= "" and v ~= "latest" then
        spec = name .. " " .. v
    end
    add_requires(spec, extra)
end

local static_cfg = {shared = false}
-- Allow prebuilt system C libraries on native Linux (saves compiling OpenSSL,
-- zlib, zstd, sqlite3, c-ares, libuuid from source), but force target-arch
-- static builds from xmake-repo when cross-compiling (mingw/windows/arm64).
local native_linux = is_host("linux") and is_plat("linux") and is_arch(os.arch())
local sys_c_opts = native_linux and {system = true} or {configs = static_cfg, system = false}
local fallback_c_opts = native_linux and nil or {configs = static_cfg, system = false}

-- xmake-repo (C++ / header-only: pinned versions & sanitizer-safe)
req("fmt", {configs = static_cfg})
req("jsoncpp", {configs = static_cfg})
req("cxxopts", {configs = static_cfg})
req("unordered_dense", {configs = static_cfg})
req("glaze", {configs = static_cfg})
req("crow", {configs = static_cfg})
req("asio")
req("cpp-httplib", {configs = static_cfg})
req("ixwebsocket", {configs = static_cfg})

-- xmake-repo (C libraries: prebuilt system packages on native Linux)
req("openssl", sys_c_opts)
req("zlib", sys_c_opts)
req("zstd", fallback_c_opts)
req("sqlite3", sys_c_opts)

-- recipes above
req("libmodbus", {configs = static_cfg})
req("snap7", {configs = static_cfg})
req("angelscript", {configs = static_cfg})
req("opener", {configs = static_cfg})
req("sqlite_modern_cpp", {configs = static_cfg})
req("open62541", {configs = static_cfg})
req("xml_h")
if not is_plat("windows", "mingw") then
    req("readline")
end

if has_config("tests") and not is_plat("mingw", "windows") then
    req("reproc")
end

if has_config("torch") then
    add_requires("sgrn_torch", {system = true})
end

if has_config("datastore") then
    req("drogon", {configs = static_cfg})
    req("libpq", {configs = static_cfg})
    req("c-ares", fallback_c_opts)
    req("libuuid", sys_c_opts)
end
