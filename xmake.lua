-- SGRN root xmake.lua (replaces the CMake build entirely)
--
--   xmake f -p linux -m release -y && xmake          native static release
--   xmake f -k shared -m debug                       dev build (shared libs, $ORIGIN rpath)
--   xmake f -p mingw -a x86_64 -m release -y && xmake            win64 cross
--   xmake f -p cross -a arm64 --cross=aarch64-linux-gnu- -m release -y && xmake   arm64 cross
--   xmake f --tests=y && xmake test                  tests
-- See xmake/README.md for the full CMake-preset mapping.

set_xmakever("2.8.5")
set_project("SGRN")
set_version("1.0.0")
set_config("buildir", ".build")

add_moduledirs("xmake/modules")

-- Lock in GNU compilers on Linux (prevents Conda/Micromamba MinGW leakage)
if is_plat("linux") then
    set_toolchains("gcc")
    set_toolset("cc", "gcc")
    set_toolset("cxx", "g++")
    add_ldflags("-B/usr/bin/", {force = true})
    add_shflags("-B/usr/bin/", {force = true})
    if is_arch("x86_64") then
        add_ldflags("-Wl,-rpath-link,/usr/lib/x86_64-linux-gnu", {force = true})
        add_shflags("-Wl,-rpath-link,/usr/lib/x86_64-linux-gnu", {force = true})
    end
end

set_languages("cxx23", "c11")
set_defaultmode("release")
add_rules("mode.release", "mode.debug")
add_rules("plugin.compile_commands.autoupdate", {outputdir = "."})

includes("deps-versions.lua")   -- dependency versions (SGRN_DEPS)
includes("xmake/options.lua")
includes("xmake/rules.lua")
includes("xmake/packages.lua")

add_rules("sgrn.base")

includes("sgrn")
includes("xmake/tasks.lua")
