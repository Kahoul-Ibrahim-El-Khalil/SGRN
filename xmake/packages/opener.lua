local local_opener_src = path.join(os.projectdir(), ".cpm-cache/opener/457c")
if not os.isdir(local_opener_src) then
    local home = os.getenv("HOME") or "/home/odahim"
    local alt = path.join(home, ".cache/CPM/opener/457c")
    if os.isdir(alt) then
        local_opener_src = alt
    else
        local_opener_src = nil
    end
end

package("opener")
    set_description("OpENer EtherNet/IP Stack (static)")
    set_license("BSD-3-Clause")

    add_versions("8c8fa9f", "8c8fa9f0df7d72ffe17a2443f8457017e71c68bf")
    add_versions("2.3", "8c8fa9f0df7d72ffe17a2443f8457017e71c68bf")

    if local_opener_src and os.isdir(local_opener_src) then
        set_sourcedir(local_opener_src)
    else
        add_urls("https://github.com/EIPStackGroup/OpENer.git")
    end

    on_install(function (package)
        local src = package:sourcedir() or "."
        local srcdir = path.join(src, "source/src")
        if not os.isdir(srcdir) then
            local matches = os.dirs(path.join(src, "**/source/src"))
            if matches and #matches > 0 then
                srcdir = matches[1]
            else
                srcdir = path.join(src, "src")
            end
        end

        local gen_dir = path.join(package:buildir(), "gen")
        os.mkdir(gen_dir)

        -- Generate devicedata.h
        local devicedata_in = io.readfile(path.join(srcdir, "ports/devicedata.h.in"))
        devicedata_in = devicedata_in:gsub("@OpENer_Device_Config_Vendor_Id@", "1")
        devicedata_in = devicedata_in:gsub("@OpENer_Device_Config_Device_Type@", "12")
        devicedata_in = devicedata_in:gsub("@OpENer_Device_Config_Product_Code@", "65001")
        devicedata_in = devicedata_in:gsub("@OpENer_Device_Config_Device_Name@", "SGRN EtherNet/IP Adapter")
        devicedata_in = devicedata_in:gsub("@OpENer_Device_Major_Version@", "2")
        devicedata_in = devicedata_in:gsub("@OpENer_Device_Minor_Version@", "3")
        devicedata_in = devicedata_in:gsub("@OpENer_VERSION_MAJOR@", "2")
        devicedata_in = devicedata_in:gsub("@OpENer_VERSION_MINOR@", "3")
        io.writefile(path.join(gen_dir, "devicedata.h"), devicedata_in)

        -- Generate opener_user_conf.h
        local user_conf = [[
#ifndef OPENER_USER_CONF_H_
#define OPENER_USER_CONF_H_
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef unsigned short in_port_t;
#else
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <arpa/inet.h>
  #include <sys/select.h>
#endif
#include <assert.h>
#include "typedefs.h"
#include "devicedata.h"
#ifndef RESTRICT
  #if defined(__cplusplus)
    #define RESTRICT
  #elif defined(_MSC_VER)
    #define RESTRICT __restrict
  #elif defined(__GNUC__) || defined(__clang__)
    #define RESTRICT __restrict__
  #else
    #define RESTRICT
  #endif
#endif
#ifndef PC_OPENER_ETHERNET_BUFFER_SIZE
  #define PC_OPENER_ETHERNET_BUFFER_SIZE 4096
#endif
#ifndef CIP_FILE_OBJECT
  #define CIP_FILE_OBJECT 0
#endif
#ifndef CIP_SECURITY_OBJECTS
  #define CIP_SECURITY_OBJECTS 0
#endif
#ifndef OPENER_IS_DLR_DEVICE
  #define OPENER_IS_DLR_DEVICE 0
#endif
#ifndef OPENER_TCPIP_IFACE_CFG_SETTABLE
  #define OPENER_TCPIP_IFACE_CFG_SETTABLE 0
#endif
#ifndef OPENER_ETHLINK_INSTANCE_CNT
  #define OPENER_ETHLINK_INSTANCE_CNT 1
#endif
#ifndef OPENER_ETHLINK_LABEL_ENABLE
  #define OPENER_ETHLINK_LABEL_ENABLE 0
#endif
#ifndef OPENER_ETHLINK_CNTRS_ENABLE
  #define OPENER_ETHLINK_CNTRS_ENABLE 0
#endif
#ifndef OPENER_ETHLINK_IFACE_CTRL_ENABLE
  #define OPENER_ETHLINK_IFACE_CTRL_ENABLE 0
#endif
#ifndef OPENER_MESSAGE_DATA_REPLY_BUFFER
  #define OPENER_MESSAGE_DATA_REPLY_BUFFER 500
#endif
#define OPENER_CIP_NUM_APPLICATION_SPECIFIC_CONNECTABLE_OBJECTS 1
#define OPENER_CIP_NUM_EXPLICIT_CONNS 6
#define OPENER_CIP_NUM_EXLUSIVE_OWNER_CONNS 1
#define OPENER_CIP_NUM_INPUT_ONLY_CONNS 1
#define OPENER_CIP_NUM_INPUT_ONLY_CONNS_PER_CON_PATH 3
#define OPENER_CIP_NUM_LISTEN_ONLY_CONNS 1
#define OPENER_CIP_NUM_LISTEN_ONLY_CONNS_PER_CON_PATH 3
#define OPENER_NUMBER_OF_SUPPORTED_SESSIONS 20
static const MilliSeconds kOpenerTimerTickInMilliSeconds = 10;
#ifndef OPENER_UNIT_TEST
  #define OPENER_ASSERT(assertion) assert(assertion);
#endif
#endif
]]
        io.writefile(path.join(gen_dir, "opener_user_conf.h"), user_conf)

        -- Platform port selection (matching cmake/deps/opener.cmake)
        local is_win = package:is_plat("windows", "mingw")
        local port_dir = is_win and path.join(srcdir, "ports/WIN32") or path.join(srcdir, "ports/POSIX")

        local sources = {}
        for _, f in ipairs(os.files(path.join(srcdir, "cip/*.c"))) do table.insert(sources, f) end
        for _, f in ipairs(os.files(path.join(srcdir, "enet_encap/*.c"))) do table.insert(sources, f) end
        for _, f in ipairs(os.files(path.join(srcdir, "utils/*.c"))) do table.insert(sources, f) end
        for _, f in ipairs(os.files(path.join(srcdir, "ports/*.c"))) do table.insert(sources, f) end
        for _, f in ipairs(os.files(path.join(port_dir, "*.c"))) do
            if not f:endswith("main.c") then table.insert(sources, f) end
        end

        local incdirs = {
            srcdir,
            path.join(srcdir, "cip"),
            path.join(srcdir, "enet_encap"),
            path.join(srcdir, "utils"),
            path.join(srcdir, "ports"),
            port_dir,
            gen_dir
        }

        local cc = package:build_getenv("CC") or os.getenv("CC") or
                   (package:is_plat("linux") and not package:is_cross() and "/usr/bin/gcc") or "gcc"
        local cflags = {
            "-std=c99",
            "-fPIC",
            "-DPC_OPENER_ETHERNET_BUFFER_SIZE=4096",
            "-Wno-incompatible-pointer-types",
            "-Wno-error=incompatible-pointer-types",
            "-Wno-discarded-qualifiers",
            "-Wno-error=discarded-qualifiers"
        }

        if is_win then
            table.insert(cflags, "-DRESTRICT=__restrict")
            table.insert(cflags, "-D_CRT_SECURE_NO_WARNINGS")
        else
            table.insert(cflags, "-DRESTRICT=__restrict__")
            table.insert(cflags, "-D_GNU_SOURCE")
            table.insert(cflags, "-D_DEFAULT_SOURCE")
            table.insert(cflags, "-D_POSIX_C_SOURCE=200112L")
            table.insert(cflags, "-DOPENER_POSIX")
            table.insert(cflags, "-include")
            table.insert(cflags, "net/if.h")
        end

        for _, inc in ipairs(incdirs) do table.insert(cflags, "-I" .. inc) end

        local objdir = path.join(package:buildir(), "obj")
        os.mkdir(objdir)
        local objs = {}
        for i, srcfile in ipairs(sources) do
            local objfile = path.join(objdir, "obj_" .. i .. "_" .. path.basename(srcfile) .. ".o")
            os.vrunv(cc, table.join(cflags, {"-c", srcfile, "-o", objfile}))
            table.insert(objs, objfile)
        end

        local libdir = path.join(package:installdir(), "lib")
        local incdir = path.join(package:installdir(), "include")
        local incdir_sub = path.join(incdir, "opener")
        os.mkdir(libdir)
        os.mkdir(incdir)
        os.mkdir(incdir_sub)
        local ar = package:build_getenv("AR") or os.getenv("AR") or "ar"
        os.vrunv(ar, table.join({"rcs", path.join(libdir, "libopener.a")}, objs))

        -- Install subdirectories preserving layout: include/cip, include/ports, etc.
        os.cp(path.join(srcdir, "cip"), incdir)
        os.cp(path.join(srcdir, "enet_encap"), incdir)
        os.cp(path.join(srcdir, "utils"), incdir)
        os.cp(path.join(srcdir, "ports"), incdir)
        if os.isdir(port_dir) then
            os.cp(path.join(port_dir, "*.h"), path.join(incdir, "ports"))
        end
        os.cp(path.join(gen_dir, "*.h"), incdir)

        -- Also install flat headers to both include/ and include/opener/
        local header_dirs = {
            srcdir,
            path.join(srcdir, "cip"),
            path.join(srcdir, "enet_encap"),
            path.join(srcdir, "utils"),
            path.join(srcdir, "ports"),
            port_dir,
            gen_dir
        }
        for _, dir in ipairs(header_dirs) do
            for _, h in ipairs(os.files(path.join(dir, "*.h"))) do
                os.cp(h, incdir)
                os.cp(h, incdir_sub)
            end
        end

        package:add("defines", "PC_OPENER_ETHERNET_BUFFER_SIZE=4096")
        if is_win then
            package:add("defines", "RESTRICT=__restrict")
            package:add("syslinks", "ws2_32", "iphlpapi")
        else
            package:add("defines", "RESTRICT=__restrict__")
            package:add("syslinks", "pthread")
        end
    end)

    on_test(function (package)
        if package:is_cross() then return end
        assert(package:has_cincludes("opener_api.h"))
    end)
