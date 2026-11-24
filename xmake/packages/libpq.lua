-- xmake/packages/libpq.lua
-- Builds PostgreSQL client libs from source so we get the full set of static
-- archives needed for self-contained linking:
--   libpq.a + libpgcommon_shlib.a + libpgport_shlib.a
--
-- Why not use the system libpq.a?
--   The Ubuntu libpq-dev package ships libpq.a but NOT the companion
--   libpgcommon.a / libpgport.a archives (those are in postgresql-server-dev).
--   Without them, every PQ* symbol pulls in unresolved pg_* internal helpers.
--   Building from source gives us the full set in one installdir.
package("libpq")
    set_homepage("https://www.postgresql.org/")
    set_description("PostgreSQL C client library (static, built from source)")
    set_license("PostgreSQL")

    -- Use PG 17 (latest stable) for client lib static build.
    -- Build system has PG 18 as system default but PG 17 static client
    -- archives are what we need for self-contained linking.
    add_urls("https://ftp.postgresql.org/pub/source/v$(version)/postgresql-$(version).tar.bz2")
    add_versions("17.12", "9cf136a703d8ee2f83465c43be4e9e8e7930c82b0bfba13be7e2537be769ac55")

    add_deps("zlib", "openssl")

    on_install("linux", "macosx", function (package)
        local configs = {
            "--without-readline",
            "--without-icu",
            "--without-systemd",
            "--with-ssl=openssl",
            "--with-zlib",
        }
        -- Inject xmake-managed openssl / zlib paths when built from source
        local openssl = package:dep("openssl")
        if openssl and not openssl:is_system() and openssl:installdir() then
            table.insert(configs, "--with-openssl=" .. openssl:installdir())
        end
        local zlib = package:dep("zlib")
        if zlib and not zlib:is_system() and zlib:installdir() then
            table.insert(configs, "--with-zlib=" .. zlib:installdir())
        end

        -- Build only the client-side libraries (no server, no pg_dump, etc.)
        import("package.tools.autoconf").install(package, configs)

        -- The autoconf install puts everything under installdir/; for static
        -- linking we also need to copy the internal helper archives that the
        -- pg build tree produces but does not install by default.
        local builddir = package:buildir()
        local libdir   = package:installdir("lib")
        for _, name in ipairs({"libpgcommon.a", "libpgcommon_shlib.a",
                                "libpgport.a",   "libpgport_shlib.a"}) do
            local found = os.files(path.join(builddir, "**", name))
            if found and #found > 0 and not os.isfile(path.join(libdir, name)) then
                os.cp(found[1], libdir)
            end
        end
    end)

    on_fetch("linux", function (package, opt)
        -- Opportunistic: if the user already has a full static pg install
        -- (i.e., postgresql-server-dev-* is present), skip building from source.
        import("lib.detect.find_path")
        import("lib.detect.find_file")
        local libdirs = {
            "/usr/lib/postgresql/18/lib",
            "/usr/lib/postgresql/17/lib",
            "/usr/lib/postgresql/16/lib",
            "/usr/lib/x86_64-linux-gnu",
            "/usr/lib64",
        }
        local pgcommon_a = find_file("libpgcommon_shlib.a", libdirs)
                        or find_file("libpgcommon.a", libdirs)
        local pgport_a   = find_file("libpgport_shlib.a", libdirs)
                        or find_file("libpgport.a", libdirs)
        local libpq_a    = find_file("libpq.a", {"/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib"})
        -- Debian's static libpq.a is built against the *_shlib helper
        -- archives and pulls its OAuth client out into libpq-oauth.a.
        local oauth_a    = find_file("libpq-oauth.a", libdirs)
        local feutils_a  = find_file("libpgfeutils.a", libdirs)
        local incdir     = find_path("libpq-fe.h",
                               {"/usr/include/postgresql", "/usr/local/include/postgresql", "/usr/include"})

        -- Guard against mixing versions: Debian/Ubuntu can install libpq-dev
        -- from a newer major version while only an older postgresql-server-dev
        -- provides the internal helper archives. libpq.a from the newer client
        -- then references helpers that the older archives do not provide.
        local pg_major
        if incdir then
            local pgconf = path.join(incdir, "pg_config.h")
            if os.isfile(pgconf) then
                local content = io.readfile(pgconf)
                if content then
                    pg_major = content:match('#define%s+PG_MAJORVERSION%s+"(%d+)"')
                end
            end
        end
        local helper_major = pgcommon_a and pgcommon_a:match("/postgresql/(%d+)/lib/")
        if pg_major and helper_major and pg_major ~= helper_major then
            libpq_a = nil
        end

        -- Debian ships libldap/libgssapi_krb5 without the unversioned .so
        -- development symlink, so `-lldap` only resolves when some package
        -- contributes a lib dir to -L (a conda prefix, for instance). Resolve
        -- them to absolute paths instead, so we do not need to add system lib
        -- dirs to the link search path (an older libfmt.so there would shadow
        -- the pinned fmt package).
        local function _resolve_syslib(name)
            local dirs = {"/usr/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib"}
            return find_file("lib" .. name .. ".so", dirs)
                or find_file("lib" .. name .. ".so.*", dirs)
        end
        local syslinks = {"ssl", "crypto", "zstd"}
        local sysflags = {}
        for _, name in ipairs({"ldap", "gssapi_krb5", "curl"}) do
            local libfile = _resolve_syslib(name)
            if libfile then
                table.insert(sysflags, libfile)
            else
                table.insert(syslinks, name)
            end
        end

        if libpq_a and pgcommon_a and pgport_a and incdir then
            -- Pass the archives by absolute path instead of adding their
            -- directories to -L. The system lib dir holds an older libfmt.so,
            -- and if it ends up before the pinned fmt package in the link
            -- search path, `-lfmt` resolves to the wrong ABI. The archives
            -- reference each other, so group them.
            local archives = {libpq_a}
            for _, extra in ipairs({oauth_a, pgcommon_a, pgport_a, feutils_a}) do
                if extra then table.insert(archives, extra) end
            end
            return {
                includedirs = {incdir},
                syslinks    = syslinks,
                ldflags     = table.join({"-Wl,--start-group"}, archives,
                                         {"-Wl,--end-group"}, sysflags),
            }
        end

        local libpq_so = find_file("libpq.so", libdirs)
        if libpq_so and incdir then
            return {
                includedirs = {incdir},
                syslinks    = syslinks,
                ldflags     = table.join({libpq_so, "-Wl,-rpath," .. path.directory(libpq_so)}, sysflags),
            }
        end
        -- Fall through to on_install if neither is found
    end)

    on_test(function (package)
        assert(package:has_cfuncs("PQconnectdb", {includes = "libpq-fe.h"}))
    end)
