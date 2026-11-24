package("snap7")
    set_description("Snap7 S7 Communication library (SGRN hardened, static)")
    set_license("LGPL-3.0")

    -- "1.4.3" = commit 30f37da (what the CMake build pinned). Archive hashes are sha256; any other
    -- version string (branch/tag/commit from deps-versions.lua) falls back to the git URL.
    add_urls("https://github.com/davenardella/snap7/archive/$(version).tar.gz",
             {version = function (version)
                 local v = type(version) == "string" and version or version:rawstr()
                 return v == "1.4.3" and "30f37da3114024a71ba93f7fd855c680b97a406f" or v
             end})
    add_urls("https://github.com/davenardella/snap7.git")
    add_versions("1.4.3", "2840d50a833d928e5175b8ccc5383fae0fcc3e826c07e60b2b4190a30c613c36")

    on_install(function (package)
        local src = package:sourcedir() or "."
        if not os.isfile(path.join(src, "src/core/s7_peer.cpp")) then
            local matches = os.files(path.join(src, "**/src/core/s7_peer.cpp"))
            if matches and #matches > 0 then
                src = path.directory(path.directory(path.directory(matches[1])))
            end
        end

        -- ── PDU-length hardening (mirrors cmake/deps/FetchDeps.cmake) ─────────
        local peer = path.join(src, "src/core/s7_peer.cpp")
        local content = io.readfile(peer)
        local vulnerable = "PDULength = SwapWord(ResNegotiate->PDULength);"
        -- the original statement is the unbraced body of `if (Result == 0)`, so the patch must be one block
        local hardened = "{ word NegotiatedPDULength = SwapWord(ResNegotiate->PDULength); " ..
                         "if (NegotiatedPDULength > IsoPayload_Size) NegotiatedPDULength = IsoPayload_Size; " ..
                         "PDULength = NegotiatedPDULength; }"

        if content:find("NegotiatedPDULength", 1, true) then
            cprint("[snap7] PDU hardening already applied, skipping.")
        elseif content:find(vulnerable, 1, true) then
            local pattern = vulnerable:gsub("[%(%)%+%-%*%.%?%[%^%$]", "%%%1")
            local patched = content:gsub(pattern, (hardened:gsub("%%", "%%%%")), 1)
            io.writefile(peer, patched)
            cprint("[snap7] PDU hardening applied.")
        else
            raise("[snap7] hardening failed: vulnerable pattern not found in s7_peer.cpp")
        end

        -- ── Sources ───────────────────────────────────────────────────────────
        local wrap = path.join(src, "release/wrappers/c-cpp")
        local sources = {
            path.join(src, "src/sys/snap_msgsock.cpp"),
            path.join(src, "src/sys/snap_sysutils.cpp"),
            path.join(src, "src/sys/snap_tcpsrvr.cpp"),
            path.join(src, "src/sys/snap_threads.cpp"),
            path.join(src, "src/core/s7_client.cpp"),
            path.join(src, "src/core/s7_isotcp.cpp"),
            path.join(src, "src/core/s7_partner.cpp"),
            path.join(src, "src/core/s7_peer.cpp"),
            path.join(src, "src/core/s7_server.cpp"),
            path.join(src, "src/core/s7_text.cpp"),
            path.join(src, "src/core/s7_micro_client.cpp"),
            path.join(src, "src/lib/snap7_libmain.cpp"),
            path.join(wrap, "snap7.cpp"),
        }

        local incdirs = {
            path.join(src, "src/sys"),
            path.join(src, "src/core"),
            path.join(src, "src/lib"),
            wrap,
        }

        local cxxflags = {"-std=c++11", "-fPIC"}
        if package:is_plat("linux") then
            table.insert(cxxflags, "-pthread")
        elseif package:is_plat("windows", "mingw") then
            table.insert(cxxflags, "-lws2_32 -lwinmm")
        end
        for _, d in ipairs(incdirs) do table.insert(cxxflags, "-I" .. d) end

        -- compile
        local objs = {}
        local objdir = path.join(package:buildir(), "obj")
        os.mkdir(objdir)
        local cxx = package:build_getenv("CXX") or os.getenv("CXX") or
                    (package:is_plat("linux") and not package:is_cross() and "/usr/bin/g++") or "g++"
        for _, f in ipairs(sources) do
            local obj = path.join(objdir, path.basename(f) .. ".o")
            os.vrunv(cxx, table.join(cxxflags, {"-c", f, "-o", obj}))
            table.insert(objs, obj)
        end

        -- archive
        local libdir = path.join(package:installdir(), "lib")
        local incdir = path.join(package:installdir(), "include")
        local incdir_sub = path.join(incdir, "snap7cpp")
        os.mkdir(libdir)
        os.mkdir(incdir)
        os.mkdir(incdir_sub)
        local ar = package:build_getenv("AR") or os.getenv("AR") or "ar"
        os.vrunv(ar, table.join({"rcs", path.join(libdir, "libsnap7cpp.a")}, objs))

        -- headers
        for _, d in ipairs(incdirs) do
            for _, h in ipairs(os.files(path.join(d, "*.h"))) do
                os.cp(h, incdir)
                os.cp(h, incdir_sub)
            end
        end
        if os.isfile(path.join(wrap, "snap7.h")) then
            os.cp(path.join(wrap, "snap7.h"), incdir)
            os.cp(path.join(wrap, "snap7.h"), incdir_sub)
        end
    end)

    on_test(function (package)
        -- Skip test during cross-compilation (host cannot run target binaries)
        if package:is_cross() then return end
        assert(package:has_cxxincludes("snap7.h"))
    end)
