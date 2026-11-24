-- xmake/modules/sgrn/assets.lua
-- Generates the embedded-asset C++ headers (web SPA, docs, SQL, default
-- configs). Every step is incremental: it only re-runs when one of its
-- inputs changed or its output header is missing.

import("core.project.config")
import("core.project.depend")
import("lib.detect.find_program")
import("sgrn.vfs_generator")

-- ── Path & dependency helpers ─────────────────────────────────────────────────

function gendir(...)
    local base = path.absolute(config.buildir(), os.projectdir())
    return path.join(base, "gen", ...)
end

local function _root(...)
    return path.join(os.projectdir(), ...)
end

local function _glob(dir, pattern)
    local files = {}
    if os.isdir(dir) then
        for _, f in ipairs(os.files(path.join(dir, pattern or "**"))) do
            if not f:find("node_modules", 1, true) then
                table.insert(files, f)
            end
        end
    end
    return files
end

local function _existing(list)
    local out = {}
    for _, f in ipairs(list) do
        if os.isfile(f) then table.insert(out, f) end
    end
    return out
end

local function _on_changed(name, files, outputs, callback)
    local force = false
    for _, o in ipairs(outputs) do
        if not os.isfile(o) then force = true end
    end
    os.mkdir(gendir())
    depend.on_changed(callback, {
        dependfile = path.join(gendir(), name .. ".d"),
        files = files,
        changed = force
    })
end

-- ── Bun / Vite helpers ────────────────────────────────────────────────────────

local function _bun_run(workdir, args, envs)
    local bun = find_program("bun")
    assert(bun, "[sgrn/assets] 'bun' was not found in PATH (needed to build the web dashboards). " ..
                "Install bun, or build with `xmake f --web=n` to reuse an existing build output.")
    os.vrunv(bun, args, {curdir = workdir, envs = envs})
end

local function _bun_install(name, workdir, stamp_dir, lockfiles)
    local stamp = path.join(stamp_dir, "bun_install.stamp")
    _on_changed(name .. "-bun-install", _existing(lockfiles), {stamp}, function ()
        cprint("${color.build.object}[web] installing JS dependencies in %s via bun", path.filename(workdir))
        _bun_run(workdir, {"install"})
        os.mkdir(path.directory(stamp))
        io.writefile(stamp, "")
    end)
end

local function _collect_web_inputs(src_dir, config_files, ts_dirs)
    local inputs = _glob(path.join(src_dir, "src"))
    table.join2(inputs, _glob(path.join(src_dir, "public")))
    local abs_cfg = {}
    for _, f in ipairs(config_files) do table.insert(abs_cfg, path.join(src_dir, f)) end
    table.join2(inputs, _existing(abs_cfg))

    for _, pkg_dir in ipairs(ts_dirs or {}) do
        table.join2(inputs, _glob(path.join(pkg_dir, "src"), "**.ts"))
        table.join2(inputs, _existing({
            path.join(pkg_dir, "README.md"),
            path.join(pkg_dir, "package.json"),
            path.join(pkg_dir, "tsconfig.json"),
        }))
    end
    return inputs
end

-- ── Generic embedded-assets & web-bundle builders ─────────────────────────────

local function _embed(name, opt)
    local outdir = gendir(unpack(opt.out))
    local files = _glob(opt.srcdir)
    local output = path.join(outdir, opt.master)
    _on_changed(name, files, {output}, function ()
        os.mkdir(outdir)
        cprint("${color.build.object}[assets] %s", opt.comment)
        vfs_generator.generate({
            src_dir        = opt.srcdir,
            out_dir        = outdir,
            namespace      = opt.namespace,
            kind           = opt.kind,
            master         = opt.master,
            level          = opt.level or 22,
            extensions     = opt.extensions,
            exclude        = opt.exclude,
            virtual_prefix = opt.virtual_prefix
        })
    end)
    return outdir
end

local function _web_bundle(name, opt)
    local dist_dir = path.join(opt.web_dir, opt.dist_subdir)
    if config.get("web") ~= false then
        _bun_install(name, opt.web_dir, gendir(opt.out[1]), opt.lockfiles)
        local inputs = _collect_web_inputs(opt.web_dir, opt.config_files, opt.ts_dirs)
        local sentinel = path.join(dist_dir, ".build_done")
        _on_changed(name .. "-vite-build", inputs, {path.join(dist_dir, "index.html"), sentinel}, function ()
            cprint("${color.build.object}[web] %s", opt.build_comment)
            _bun_run(opt.web_dir, {"run", "build"}, opt.envs)
            io.writefile(sentinel, "")
        end)
    else
        assert(os.isfile(path.join(dist_dir, "index.html")),
               "[sgrn/assets] --web=n but " .. dist_dir .. " has no build output")
    end

    return _embed(name .. "-headers", {
        out            = opt.out,
        srcdir         = dist_dir,
        master         = "web_assets.hpp",
        comment        = opt.embed_comment,
        namespace      = opt.namespace,
        kind           = "web",
        level          = 22,
        virtual_prefix = "/",
    })
end

-- ── Public asset targets ──────────────────────────────────────────────────────

function gateway_web()
    local web = _root("sgrn", "web", "gateway")
    local bindings = _root("sgrn", "typescript", "gateway")
    return _web_bundle("gateway-web", {
        web_dir       = web,
        dist_subdir   = "dist",
        out           = {"gateway", "web"},
        namespace     = "sgrn::gateway::assets::web",
        build_comment = "building Svelte SPA -> dist/ via bun + vite",
        embed_comment = "[gateway] compressing SPA assets into C++ headers",
        lockfiles     = {path.join(web, "package.json"), path.join(web, "bun.lock"), path.join(bindings, "package.json")},
        config_files  = {"index.html", "replay.html", "vite.config.ts", "svelte.config.js", "package.json", "bun.lock"},
        ts_dirs       = {bindings},
    })
end

function gateway_doc()
    return _embed("gateway-doc", {
        out = {"gateway", "doc"},
        srcdir = _root("documentation", "gateway"),
        master = "doc_assets.hpp",
        comment = "[gateway] compressing documentation assets",
        namespace = "sgrn::gateway::assets::doc",
        kind = "other",
        extensions = {".5", ".md", ".txt"},
        level = 22
    })
end

function datastore_sql()
    return _embed("datastore-sql", {
        out = {"datastore", "sql"},
        srcdir = _root("sgrn", "lib", "datastore", "postgres"),
        master = "sql_assets.hpp",
        comment = "[datastore] compressing SQL schemas",
        namespace = "sgrn::datastore::assets::sql",
        kind = "sql",
        level = 22
    })
end

function datastore_config()
    return _embed("datastore-config", {
        out = {"datastore", "config"},
        srcdir = _root("sgrn", "lib", "datastore", "configs"),
        master = "config_assets.hpp",
        comment = "[datastore] compressing default configurations",
        namespace = "sgrn::datastore::assets::config",
        kind = "config",
        extensions = {".json", ".conf", ".service"},
        exclude = {"compile_commands.json", "endpoints.json",
                   "fastcgi.conf", "fastcgi-php.conf", "snakeoil.conf"},
        level = 22
    })
end

function datastore_dashboard()
    local src = _root("sgrn", "web", "datastore")
    local bindings = _root("sgrn", "typescript", "datastore")
    local types = _root("sgrn", "typescript", "types")
    return _web_bundle("datastore-dashboard", {
        web_dir       = src,
        dist_subdir   = "build",
        out           = {"datastore", "dashboard"},
        namespace     = "sgrn::datastore::assets::web",
        build_comment = "building React dashboard -> build/ via bun + vite",
        embed_comment = "[datastore] compressing dashboard assets into C++ headers",
        envs          = {VITE_BASE_PATH = "/datastore"},
        lockfiles     = {
            path.join(src, "package.json"), path.join(src, "bun.lock"),
            path.join(bindings, "package.json"), path.join(types, "package.json"),
        },
        config_files  = {
            "index.html", "vite.config.ts", "package.json", "bun.lock",
            "tsconfig.json", "tsconfig.app.json", "tsconfig.node.json",
        },
        ts_dirs       = {bindings, types},
    })
end
