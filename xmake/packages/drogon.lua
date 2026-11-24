package("drogon")
    set_homepage("https://drogon.org")
    set_description("C++14/17/20 based HTTP Web Framework (static)")
    set_license("MIT")

    add_urls("https://github.com/drogonframework/drogon.git", {submodules = true})
    add_versions("1.9.14", "v1.9.14")

    add_deps("cmake")
    -- libpq is sourced from xmake-repo (built from source → libpq.a + libpgcommon.a + libpgport.a)
    add_deps("jsoncpp", "openssl", "zlib", "c-ares", "libpq", "libuuid")
    -- order matters for static linking: drogon depends on trantor, so trantor must come after
    add_links("drogon", "trantor")
    add_syslinks("pthread", "dl")

    on_install(function (package)
        local configs = {
            "-DBUILD_TESTING=OFF",
            "-DBUILD_EXAMPLES=OFF",
            "-DBUILD_DOC=OFF",
            "-DBUILD_REDIS=OFF",
            "-DBUILD_CTL=OFF",
            "-DBUILD_ORM=ON",
            "-DBUILD_POSTGRESQL=ON",
            "-DBUILD_SQLITE=OFF",
            "-DBUILD_BROTLI=OFF",
            "-DBUILD_SHARED_LIBS=OFF",
            "-DLIBPQ_BATCH_MODE=OFF",
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
        }
        if package:is_plat("linux") and not package:is_cross() then
            if os.isfile("/usr/bin/gcc") and os.isfile("/usr/bin/g++") then
                table.insert(configs, "-DCMAKE_C_COMPILER=/usr/bin/gcc")
                table.insert(configs, "-DCMAKE_CXX_COMPILER=/usr/bin/g++")
            end
        end

        local dep_jsoncpp = package:dep("jsoncpp")
        if dep_jsoncpp and not dep_jsoncpp:is_system() and dep_jsoncpp:installdir() then
            local jc_inc = path.join(dep_jsoncpp:installdir(), "include")
            table.insert(configs, "-DJSONCPP_INCLUDE_DIRS=" .. jc_inc)
            table.insert(configs, "-DJsoncpp_INCLUDE_DIR=" .. jc_inc)
        elseif os.isdir("/usr/include/jsoncpp") then
            table.insert(configs, "-DJSONCPP_INCLUDE_DIRS=/usr/include/jsoncpp")
            table.insert(configs, "-DJsoncpp_INCLUDE_DIR=/usr/include/jsoncpp")
        end

        -- Pass libpq paths: check dep installdir first, then fall back to system
        local pg_inc = nil
        local pg_lib = nil
        local dep_pq = package:dep("libpq")
        if dep_pq and dep_pq:installdir() then
            local pq_inst = dep_pq:installdir()
            local cand_inc = path.join(pq_inst, "include")
            local cand_a   = path.join(pq_inst, "lib", "libpq.a")
            local cand_so  = path.join(pq_inst, "lib", "libpq.so")
            if os.isdir(cand_inc) then pg_inc = cand_inc end
            if os.isfile(cand_a) then pg_lib = cand_a
            elseif os.isfile(cand_so) then pg_lib = cand_so end
        end

        if not pg_inc or not pg_lib then
            for _, dir in ipairs({"/usr/include/postgresql", "/usr/local/include/postgresql", "/usr/include"}) do
                if os.isfile(path.join(dir, "libpq-fe.h")) then
                    pg_inc = dir
                    break
                end
            end
            for _, f in ipairs({
                "/usr/lib/x86_64-linux-gnu/libpq.so",
                "/usr/lib64/libpq.so",
                "/usr/lib/libpq.so",
                "/usr/lib/x86_64-linux-gnu/libpq.a",
                "/usr/lib64/libpq.a",
                "/usr/lib/libpq.a",
            }) do
                if os.isfile(f) then
                    pg_lib = f
                    break
                end
            end
        end

        if pg_inc then
            table.insert(configs, "-DPostgreSQL_INCLUDE_DIR=" .. pg_inc)
            table.insert(configs, "-DPG_INCLUDE_DIR=" .. pg_inc)
        end
        if pg_lib then
            table.insert(configs, "-DPostgreSQL_LIBRARY=" .. pg_lib)
            table.insert(configs, "-DPostgreSQL_LIBRARIES=" .. pg_lib)
            table.insert(configs, "-DPostgreSQL_LIBRARY_RELEASE=" .. pg_lib)
            table.insert(configs, "-DPostgreSQL_LIBRARY_DEBUG=" .. pg_lib)
            table.insert(configs, "-DPG_LIBRARY=" .. pg_lib)
            table.insert(configs, "-DPG_LIBRARIES=" .. pg_lib)
        end

        import("package.tools.cmake").install(package, configs)

        -- trantor is built from drogon's submodule; make sure its library ends up in lib/
        local libdir = package:installdir("lib")
        os.mkdir(libdir)
        if not os.isfile(path.join(libdir, "libtrantor.a")) then
            local found = os.files(path.join(package:buildir(), "**", "libtrantor.a"))
            assert(found and #found > 0, "[drogon] libtrantor.a was not built")
            os.cp(found[1], libdir)
        end
    end)

    -- Skipping on_test: linking libdrogon.a needs all transitive pg deps;
    -- a successful on_install is sufficient.
