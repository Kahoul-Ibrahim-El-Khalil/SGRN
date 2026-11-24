package("angelscript")
    set_homepage("https://www.angelcode.com/angelscript/")
    set_description("AngelScript scripting library + SGRN addons (static)")
    set_license("zlib")

    add_urls("https://github.com/anjo76/angelscript.git")
    add_versions("2.38.0", "v2.38.0")

    add_deps("cmake")

    on_install(function (package)
        local src = package:sourcedir() or "."
        local as_root = path.join(src, "sdk/angelscript")
        if not os.isdir(as_root) then
            local matches = os.dirs(path.join(src, "**/sdk/angelscript"))
            if matches and #matches > 0 then
                as_root = matches[1]
                src = path.directory(path.directory(as_root))
            end
        end

        -- Provide a root CMakeLists.txt with explicit install rules
        local rel_cmake = path.relative(path.join(as_root, "projects/cmake"), src)
        local rel_inc = path.relative(path.join(as_root, "include"), src)
        io.writefile(path.join(src, "CMakeLists.txt"), string.format([[
cmake_minimum_required(VERSION 3.10)
project(angelscript_wrapper)
add_subdirectory(%s as_build)
install(TARGETS angelscript ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(DIRECTORY %s/ DESTINATION include)
]], rel_cmake, rel_inc))

        local configs = {
            "-DBUILD_SHARED_LIBS=OFF",
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
        }
        if package:is_plat("linux") and not package:is_cross() then
            table.insert(configs, "-DCMAKE_C_COMPILER=/usr/bin/gcc")
            table.insert(configs, "-DCMAKE_CXX_COMPILER=/usr/bin/g++")
        end
        import("package.tools.cmake").install(package, configs)

        -- Build addons using the exact logic from cmake/deps/angelscript.cmake
        local addon = path.join(src, "sdk/add_on")
        local as_inc = path.join(as_root, "include")
        local addon_sources = {
            path.join(addon, "scriptstdstring/scriptstdstring.cpp"),
            path.join(addon, "scriptarray/scriptarray.cpp"),
            path.join(addon, "scripthelper/scripthelper.cpp"),
            path.join(addon, "scriptbuilder/scriptbuilder.cpp"),
            path.join(addon, "scriptdictionary/scriptdictionary.cpp")
        }

        local cxx = package:build_getenv("CXX") or os.getenv("CXX") or
                    (package:is_plat("linux") and "/usr/bin/g++") or "g++"
        local cxxflags = {"-std=c++11", "-fPIC", "-I" .. as_inc, "-I" .. addon}
        local objdir = path.join(package:buildir(), "addon_obj")
        os.mkdir(objdir)

        local addon_objs = {}
        for _, f in ipairs(addon_sources) do
            if os.isfile(f) then
                local obj = path.join(objdir, path.basename(f) .. ".o")
                os.vrunv(cxx, table.join(cxxflags, {"-c", f, "-o", obj}))
                table.insert(addon_objs, obj)
            end
        end

        local libdir = path.join(package:installdir(), "lib")
        local incdir = path.join(package:installdir(), "include")
        os.mkdir(libdir)
        os.mkdir(incdir)
        local ar = package:build_getenv("AR") or os.getenv("AR") or "ar"
        os.vrunv(ar, table.join({"rcs", path.join(libdir, "libangelscript_addons.a")}, addon_objs))

        -- Copy all headers into include/
        os.cp(path.join(as_inc, "*.h"), incdir)
        local as_inc_sub = path.join(incdir, "angelscript")
        os.mkdir(as_inc_sub)
        os.cp(path.join(as_inc, "*.h"), as_inc_sub)

        local addon_inc = path.join(incdir, "add_on")
        os.mkdir(addon_inc)
        for _, subdir in ipairs(os.dirs(path.join(addon, "*"))) do
            local subname = path.filename(subdir)
            -- 1. include/<subname>/*.h (e.g., include/scriptarray/scriptarray.h)
            local inc_sub = path.join(incdir, subname)
            os.mkdir(inc_sub)
            os.cp(path.join(subdir, "*.h"), inc_sub)

            -- 2. include/*.h flat (e.g., include/scriptarray.h)
            os.cp(path.join(subdir, "*.h"), incdir)

            -- 3. include/add_on/<subname>/*.h (e.g., include/add_on/scriptarray/scriptarray.h)
            local addon_sub = path.join(addon_inc, subname)
            os.mkdir(addon_sub)
            os.cp(path.join(subdir, "*.h"), addon_sub)

            -- 4. include/add_on/*.h flat (e.g., include/add_on/scriptarray.h)
            os.cp(path.join(subdir, "*.h"), addon_inc)
        end

        package:add("links", "angelscript_addons", "angelscript")
    end)

    on_test(function (package)
        -- Skip test during cross-compilation (host cannot run target binaries)
        if package:is_cross() then return end
        assert(package:has_cxxincludes("angelscript.h"))
        assert(package:has_cxxincludes("scriptarray/scriptarray.h"))
        assert(package:has_cxxincludes("scriptdictionary/scriptdictionary.h"))
        assert(package:has_cxxincludes("scripthelper/scripthelper.h"))
        assert(package:has_cxxincludes("scriptstdstring/scriptstdstring.h"))
        assert(package:has_cxxincludes("scriptbuilder/scriptbuilder.h"))
    end)
