package("readline")
    set_homepage("https://tiswww.case.edu/php/chet/readline/rltop.html")
    set_description("GNU Readline library")
    set_license("GPL-3.0")

    add_urls("https://ftpmirror.gnu.org/readline/readline-$(version).tar.gz",
             "https://ftp.gnu.org/gnu/readline/readline-$(version).tar.gz",
             "https://mirrors.kernel.org/gnu/readline/readline-$(version).tar.gz")
    add_versions("8.2", "3feb7171f16a84ee82ca18a36d7b9be109a52c04f492a053331d7d1095007c35")

    on_fetch(function (package)
        if package:is_plat("windows", "mingw") then
            return {}
        end

        local result = {}
        -- 1. Check system /usr/include/readline/readline.h
        if os.isfile("/usr/include/readline/readline.h") then
            result.includedirs = {"/usr/include"}
            result.links = {"readline", "history"}
            result.syslinks = {"tinfo"}
            result.linkdirs = {"/usr/lib", "/usr/lib/x86_64-linux-gnu"}
            return result
        end

        -- 2. Check conda prefix if active
        local conda = os.getenv("CONDA_PREFIX")
        if conda and os.isfile(path.join(conda, "include/readline/readline.h")) then
            result.includedirs = {path.join(conda, "include")}
            result.links = {"readline", "history"}
            result.syslinks = {"tinfo"}
            result.linkdirs = {path.join(conda, "lib")}
            return result
        end

        -- 3. Try pkg-config
        import("lib.detect.find_package")
        local pkg = find_package("pkgconfig::readline")
        if pkg then
            return pkg
        end
    end)

    on_install("linux", function (package)
        local configs = {
            "--enable-static",
            "--disable-shared",
            "--with-curses"
        }
        local cflags = {"-fPIC"}
        table.insert(configs, "CFLAGS=-fPIC")
        import("package.tools.autoconf").install(package, configs)
        package:add("links", "readline", "history")
        package:add("syslinks", "tinfo")
    end)

    on_test(function (package)
        if package:is_plat("windows", "mingw") or package:is_cross() then
            return
        end
        -- readline.h expects <stdio.h> to be included first (it uses FILE), so a
        -- plain has_cxxincludes() check compiles a broken snippet with modern glibc.
        assert(package:has_cfuncs("readline", {includes = {"stdio.h", "readline/readline.h"}}))
    end)
