package("open62541")
    set_homepage("https://open62541.org")
    set_description("Open62541 OPC UA implementation (static)")
    set_license("MPL-2.0")

    add_urls("https://github.com/open62541/open62541/archive/refs/tags/$(version).tar.gz",
             "https://github.com/open62541/open62541.git")
    add_versions("v1.5.7", "a4018b052c93fedb55f00558a85869b86f3bb2293184d322e33f2666c497eaeb")

    add_deps("cmake")

    on_install(function (package)
        local configs = {
            "-DUA_BUILD_UNIT_TESTS=OFF",
            "-DUA_BUILD_EXAMPLES=OFF",
            "-DUA_BUILD_TOOLS=OFF",
            "-DUA_ENABLE_AMALGAMATION=OFF",
            "-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF",
            "-DBUILD_SHARED_LIBS=OFF",
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
        }
        if package:is_plat("linux") and not package:is_cross() then
            table.insert(configs, "-DCMAKE_C_COMPILER=/usr/bin/gcc")
            table.insert(configs, "-DCMAKE_CXX_COMPILER=/usr/bin/g++")
        end
        import("package.tools.cmake").install(package, configs)

        if package:is_plat("windows", "mingw") then
            package:add("syslinks", "ws2_32")
        elseif package:is_plat("linux") then
            package:add("syslinks", "pthread", "rt")
        end
    end)

    on_test(function (package)
        if package:is_cross() then return end
        assert(package:has_cincludes("open62541/server.h"))
    end)
