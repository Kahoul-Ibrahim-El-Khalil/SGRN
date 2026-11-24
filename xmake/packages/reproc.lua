package("reproc")
    set_homepage("https://github.com/DaanDeMeyer/reproc")
    set_description("Cross-platform child-process control (C99 core + C++11 wrapper), static")
    set_license("MIT")

    add_urls("https://github.com/DaanDeMeyer/reproc.git")
    add_versions("14.2.5", "v14.2.5")
    add_versions("14.2.4", "v14.2.4")

    add_deps("cmake")
    add_links("reproc++", "reproc")
    if is_plat("windows", "mingw") then
        add_syslinks("ws2_32")
    else
        add_syslinks("pthread")
    end

    on_install(function (package)
        local configs = {
            "-DREPROC++=ON", "-DREPROC_DEVELOP=OFF", "-DREPROC_TEST=OFF",
            "-DREPROC_EXAMPLES=OFF", "-DREPROC_WARNINGS=OFF", "-DREPROC_INSTALL=ON",
            "-DREPROC_INSTALL_PKGCONFIG=OFF", "-DBUILD_SHARED_LIBS=OFF",
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
        }
        import("package.tools.cmake").install(package, configs)
    end)
