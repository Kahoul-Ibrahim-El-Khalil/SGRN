package("sqlite_modern_cpp")
    set_kind("library", {headeronly = true})
    set_description("C++ wrapper for sqlite (header-only)")
    set_license("MIT")

    add_urls("https://github.com/SqliteModernCpp/sqlite_modern_cpp.git")
    add_versions("3.2", "v3.2")
    add_deps("sqlite3")

    on_install(function (package)
        local src = package:sourcedir() or "."
        local hdrdir = path.join(src, "hdr")
        if not os.isdir(hdrdir) then
            local matches = os.dirs(path.join(src, "**/hdr"))
            if matches and #matches > 0 then
                hdrdir = matches[1]
            end
        end

        local incdir = path.join(package:installdir(), "include")
        os.mkdir(incdir)
        os.cp(path.join(hdrdir, "*"), incdir)
    end)
