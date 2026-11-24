package("libmodbus")
    set_homepage("https://libmodbus.org")
    set_description("A Modbus library for Linux, Mac OS X, FreeBSD, QNX and Windows (static)")
    set_license("LGPL-2.1")

    add_urls("https://github.com/stephane/libmodbus/archive/refs/tags/v$(version).tar.gz",
             "https://github.com/stephane/libmodbus.git")
    add_versions("3.2.0", "0c61007b2815daf452618f8b877d15cdcd376b71ad5dbd06b330d70f53b2ccaa")

    on_install(function (package)
        local src = package:sourcedir() or "."
        local srcdir = path.join(src, "src")
        if not os.isdir(srcdir) then
            local matches = os.dirs(path.join(src, "**/src"))
            for _, d in ipairs(matches) do
                if os.isfile(path.join(d, "modbus.c")) then
                    srcdir = d
                    break
                end
            end
        end

        local gen_dir = path.join(package:buildir(), "gen")
        os.mkdir(gen_dir)

        io.writefile(path.join(gen_dir, "config.h"), [[
#ifndef CONFIG_H
#define CONFIG_H
#ifndef _WIN32
#  define _GNU_SOURCE 1
#  define _DEFAULT_SOURCE 1
#  define _POSIX_C_SOURCE 200809L
#  define HAVE_SYS_SOCKET_H 1
#  define HAVE_ARPA_INET_H 1
#  define HAVE_NETINET_IN_H 1
#  define HAVE_NETINET_IP_H 1
#  define HAVE_NETINET_TCP_H 1
#  define HAVE_UNISTD_H 1
#endif
#define HAVE_STDINT_H 1
#define HAVE_INTTYPES_H 1
#define PACKAGE_VERSION "3.2.0"
#endif
]])

        io.writefile(path.join(gen_dir, "modbus-version.h"), [[
#ifndef MODBUS_VERSION_H
#define MODBUS_VERSION_H
#define LIBMODBUS_VERSION_MAJOR 3
#define LIBMODBUS_VERSION_MINOR 2
#define LIBMODBUS_VERSION_MICRO 0
#define LIBMODBUS_VERSION_STRING "3.2.0"
#endif
]])

        local sources = {
            path.join(srcdir, "modbus.c"),
            path.join(srcdir, "modbus-data.c"),
            path.join(srcdir, "modbus-rtu.c"),
            path.join(srcdir, "modbus-tcp.c")
        }

        local cc = package:build_getenv("CC") or os.getenv("CC") or
                   (package:is_plat("linux") and not package:is_cross() and "/usr/bin/gcc") or "gcc"
        local cflags = {
            "-std=c11",
            "-fPIC",
            "-D_GNU_SOURCE",
            "-D_DEFAULT_SOURCE",
            "-D_POSIX_C_SOURCE=200809L",
            -- modbus-tcp.c / modbus-rtu.c do not include config.h themselves
            "-DHAVE_CONFIG_H",
            "-DHAVE_STDINT_H", "-DHAVE_INTTYPES_H",
            "-I" .. srcdir,
            "-I" .. gen_dir
        }

        if not package:is_plat("windows", "mingw") then
            table.join2(cflags, {"-DHAVE_SYS_SOCKET_H", "-DHAVE_ARPA_INET_H", "-DHAVE_NETINET_IN_H",
                                 "-DHAVE_NETINET_IP_H", "-DHAVE_NETINET_TCP_H", "-DHAVE_UNISTD_H"})
        end

        local objdir = path.join(package:buildir(), "obj")
        os.mkdir(objdir)
        local objs = {}
        for _, f in ipairs(sources) do
            local obj = path.join(objdir, path.basename(f) .. ".o")
            os.vrunv(cc, table.join(cflags, {"-c", f, "-o", obj}))
            table.insert(objs, obj)
        end

        local libdir = path.join(package:installdir(), "lib")
        local incdir = path.join(package:installdir(), "include")
        local incdir_modbus = path.join(incdir, "modbus")
        os.mkdir(libdir)
        os.mkdir(incdir)
        os.mkdir(incdir_modbus)
        local ar = package:build_getenv("AR") or os.getenv("AR") or "ar"
        os.vrunv(ar, table.join({"rcs", path.join(libdir, "libmodbus.a")}, objs))

        for _, h in ipairs(os.files(path.join(srcdir, "*.h"))) do
            os.cp(h, incdir)
            os.cp(h, incdir_modbus)
        end
        for _, h in ipairs(os.files(path.join(gen_dir, "*.h"))) do
            os.cp(h, incdir)
            os.cp(h, incdir_modbus)
        end
    end)

    on_test(function (package)
        if package:is_cross() then return end
        assert(package:has_cincludes("modbus.h"))
    end)
