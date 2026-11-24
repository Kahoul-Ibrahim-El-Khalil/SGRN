-- xmake/modules/sgrn/codegen.lua
-- Drogon ORM model + CRUD view regeneration (native xmake buildsystem module).

import("core.base.process")
import("core.base.socket")
import("core.project.config")
import("core.project.depend")
import("lib.detect.find_program")

local DEFAULT_SCHEMAS = {"core", "storage"}
local BUILD_DB_NAME = "sgrn_build_orm"
local TARGET_NS = "sgrn"

-- ── Tool discovery helpers ───────────────────────────────────────────────────

local function _python()
    local py = find_program("python3") or find_program("python")
    assert(py, "[sgrn/codegen] python3 was not found in PATH")
    return py
end

local function _env_bin_hints()
    local hints = {}
    local conda = os.getenv("CONDA_PREFIX")
    if conda then table.insert(hints, path.join(conda, "bin")) end
    local home = os.getenv("HOME")
    if home then
        table.insert(hints, path.join(home, "micromamba/envs/SGRN/bin"))
        table.insert(hints, path.join(home, ".micromamba/envs/SGRN/bin"))
    end
    return hints
end

local function _find_clang_format()
    local hints = _env_bin_hints()
    for _, name in ipairs({"clang-format", "clang-format-22", "clang-format-21",
                           "clang-format-20", "clang-format-19"}) do
        local p = find_program(name, {paths = hints})
        if p then return p end
    end
end

local function _find_drogon_ctl()
    local root = os.projectdir()
    local hints = {
        path.join(root, ".dist", "linux-static", "bin"),
        path.join(root, ".dist", "linux", "bin"),
        path.join(root, ".prefix", "linux-static", "bin"),
        path.join(root, ".prefix", "linux", "bin"),
        path.join(root, ".prefix", "bin"),
        "/usr/local/bin",
        "/usr/bin"
    }
    for _, h in ipairs(_env_bin_hints()) do table.insert(hints, h) end
    return find_program("drogon_ctl", {paths = hints})
end

local function _find_pg_binary(name)
    local hints = _env_bin_hints()
    for _, v in ipairs({17, 16, 15, 14}) do
        table.insert(hints, "/usr/lib/postgresql/" .. tostring(v) .. "/bin")
    end
    return find_program(name, {paths = hints})
end

-- ── Ephemeral PostgreSQL lifecycle ───────────────────────────────────────────

local function _is_port_open(host, port)
    local sock = socket.connect(host, port, {timeout = 500})
    if sock then
        sock:close()
        return true
    end
    return false
end

local function _free_port()
    for port = 55432, 55532 do
        local sock = socket.bind("127.0.0.1", port)
        if sock then
            sock:close()
            return port
        end
    end
    raise("[sgrn/codegen] no free port found in range 55432..55532")
end

local function _is_root()
    if is_host("windows") then return false end
    local out = try { function () return os.iorunv("id", {"-u"}) end }
    return out and out:trim() == "0"
end

local function _wait_for_port(host, port, proc, log_path)
    for _ = 1, 75 do
        if _is_port_open(host, port) then return end
        os.sleep(200)
    end
    local err_log = os.isfile(log_path) and io.readfile(log_path) or ""
    raise("[sgrn/codegen] ephemeral postgres failed to start on %s:%d\n%s", host, port, err_log)
end

local function _with_ephemeral_postgres(fn)
    local initdb_exe = _find_pg_binary("initdb")
    local postgres_exe = _find_pg_binary("postgres")
    assert(initdb_exe and postgres_exe,
        "[sgrn/codegen] could not find 'initdb' and 'postgres' to start an ephemeral cluster")

    local tmp_dir = path.join(os.tmpdir(), format("sgrn-build-pg-%d", os.mclock()))
    local data_dir = path.join(tmp_dir, "data")
    local log_path = path.join(tmp_dir, "pg.log")
    os.mkdir(tmp_dir)

    local port = _free_port()
    cprint("${dim}[codegen] starting ephemeral PostgreSQL cluster on 127.0.0.1:%d", port)
    os.vrunv(initdb_exe, {"-D", data_dir, "-U", "postgres", "--auth=trust", "--no-locale", "--encoding=UTF8"})

    local proc = process.openv(postgres_exe, {
        "-D", data_dir, "-p", tostring(port), "-k", tmp_dir,
        "-c", "listen_addresses=127.0.0.1", "-c", "fsync=off", "-c", "synchronous_commit=off"
    }, {stdout = log_path, stderr = log_path})
    assert(proc, "[sgrn/codegen] failed to spawn ephemeral postgres")

    try {
        function ()
            _wait_for_port("127.0.0.1", port, proc, log_path)
            fn("127.0.0.1", port, "postgres", "")
        end,
        finally {
            function ()
                cprint("${dim}[codegen] stopping ephemeral PostgreSQL cluster")
                proc:kill()
                proc:wait()
                proc:close()
                os.tryrm(tmp_dir)
            end
        }
    }
end

-- ── Database initialization & ORM/view generation ────────────────────────────

local function _init_build_db(py, root, db_cfg, superuser, super_pass)
    os.vrunv(py, {
        path.join(root, "init-db.py"),
        "--host", db_cfg.host,
        "--port", tostring(db_cfg.port),
        "--dbname", db_cfg.dbname,
        "--user", db_cfg.user,
        "--password", db_cfg.passwd,
        "--superuser", superuser,
        "--superuser-password", super_pass
    }, {curdir = root})
end

local function _write_model_json(config_path, db_cfg, schema)
    io.writefile(config_path, format([[{
    "rdbms": "postgresql",
    "host": "%s",
    "port": %d,
    "dbname": "%s",
    "user": "%s",
    "passwd": "%s",
    "tables": [],
    "schema": "%s"
}
]], db_cfg.host, db_cfg.port, db_cfg.dbname, db_cfg.user, db_cfg.passwd, schema))
end

local function _has_existing_models(schema_dir)
    for _, ext in ipairs({"*.h", "*.cc"}) do
        if #os.files(path.join(schema_dir, ext)) > 0 then return true end
    end
    return false
end

local function _run_drogon_ctl(drogon_ctl, schema_dir)
    local yes_file = os.tmpfile()
    io.writefile(yes_file, "y\n")
    local ok, status = -1, -1
    try {
        function ()
            local proc = process.openv(drogon_ctl, {"create", "model", schema_dir}, {stdin = yes_file})
            assert(proc, "[sgrn/codegen] failed to spawn drogon_ctl")
            ok, status = proc:wait()
            proc:close()
        end,
        finally {
            function () os.tryrm(yes_file) end
        }
    }
    if ok ~= 1 or status ~= 0 then
        raise("[sgrn/codegen] drogon_ctl failed with status %d", status)
    end
end

local function _normalize_namespace(orm_dir, from_db, to_ns)
    if from_db == to_ns then return end
    for _, f in ipairs(os.files(path.join(orm_dir, "**"))) do
        local ext = path.extension(f)
        if ext == ".h" or ext == ".cc" or ext == ".json" then
            local content = io.readfile(f)
            content = content:gsub("using namespace drogon_model::" .. from_db .. "::",
                                   "using namespace drogon_model::" .. to_ns .. "::")
            content = content:gsub("namespace " .. from_db, "namespace " .. to_ns)
            content = content:gsub("} // namespace " .. from_db, "} // namespace " .. to_ns)
            content = content:gsub("drogon_model::" .. from_db .. "::", "drogon_model::" .. to_ns .. "::")
            if ext == ".json" then
                content = content:gsub('"' .. from_db .. '"', '"' .. to_ns .. '"')
            end
            io.writefile(f, content)
        end
    end
end

local function _generate_orm_models(ds, drogon_ctl, db_cfg)
    local orm_dir = path.join(ds, "src", "orm", "models")
    os.mkdir(orm_dir)

    for _, schema in ipairs(DEFAULT_SCHEMAS) do
        local schema_dir = path.join(orm_dir, schema)
        os.mkdir(schema_dir)
        _write_model_json(path.join(schema_dir, "model.json"), db_cfg, schema)

        if not drogon_ctl then
            if _has_existing_models(schema_dir) then
                cprint("${color.warning}[codegen] drogon_ctl not found; preserving existing models for '%s'", schema)
            else
                raise("[sgrn/codegen] drogon_ctl not found and no pre-generated models exist in %s", schema_dir)
            end
        else
            local err = try {
                function () _run_drogon_ctl(drogon_ctl, schema_dir) end,
                catch { function (e) return e end }
            }
            if err then
                if _has_existing_models(schema_dir) then
                    cprint("${color.warning}[codegen] drogon_ctl failed (%s); preserving existing models for '%s'", err, schema)
                else
                    raise(err)
                end
            end
        end
    end

    _normalize_namespace(orm_dir, db_cfg.dbname, TARGET_NS)
end

local function _generate_views(py, root, ds, db_cfg)
    local views_script = path.join(ds, "scripts", "generators", "generate_views.py")
    if not os.isfile(views_script) then
        cprint("${color.warning}[codegen] generate_views.py not found; skipping views")
        return
    end
    os.vrunv(py, {
        views_script,
        "--host", db_cfg.host,
        "--port", tostring(db_cfg.port),
        "--dbname", db_cfg.dbname,
        "--user", db_cfg.user,
        "--password", db_cfg.passwd
    }, {curdir = root})
end

local function _format_generated(py, root, ds)
    local fmt_args = {
        path.join(root, "scripts", "format.py"), "cpp",
        "--dir", path.join(ds, "src", "orm", "models"),
        "--dir", path.join(ds, "src", "handlers", "generated")
    }
    local clang_format = _find_clang_format()
    if clang_format then
        table.insert(fmt_args, "--clang-format")
        table.insert(fmt_args, clang_format)
    end
    os.vrunv(py, fmt_args, {curdir = root})
end

local function _run_against_db(py, root, ds, drogon_ctl, host, port, superuser, super_pass)
    local db_cfg = {
        host = host,
        port = port,
        dbname = BUILD_DB_NAME,
        user = "sgrn_datastore",
        passwd = os.getenv("POSTGRES_PASSWORD") or "dracaeris"
    }
    _init_build_db(py, root, db_cfg, superuser, super_pass)
    _generate_orm_models(ds, drogon_ctl, db_cfg)
    _generate_views(py, root, ds, db_cfg)
end

-- ── Public entry points ──────────────────────────────────────────────────────

-- Regenerates ORM models and CRUD views, then formats the generated C++.
function run()
    local root = os.projectdir()
    local ds = path.join(root, "sgrn", "lib", "datastore")
    local py = _python()
    local drogon_ctl = _find_drogon_ctl()

    cprint("${color.build.object}[codegen] regenerating Drogon ORM models and CRUD views")

    local can_ephemeral = not _is_root() and _find_pg_binary("initdb") and _find_pg_binary("postgres")
    if can_ephemeral or not _is_port_open("127.0.0.1", 5432) then
        _with_ephemeral_postgres(function (host, port, superuser, super_pass)
            _run_against_db(py, root, ds, drogon_ctl, host, port, superuser, super_pass)
        end)
    else
        local superuser = os.getenv("POSTGRES_SUPERUSER") or "postgres"
        local super_pass = os.getenv("POSTGRES_SUPERUSER_PASSWORD") or "dracaeris"
        _run_against_db(py, root, ds, drogon_ctl, "127.0.0.1", 5432, superuser, super_pass)
    end

    _format_generated(py, root, ds)
end

-- Incremental variant used by `xmake f --auto_codegen=y`.
function auto()
    local root = os.projectdir()
    local ds = path.join(root, "sgrn", "lib", "datastore")
    local files = os.files(path.join(ds, "postgres", "**.sql"))
    for _, f in ipairs({path.join(ds, "crud", "manifest.json"),
                        path.join(ds, "scripts", "generators", "generate_views.py"),
                        path.join(root, "init-db.py"),
                        path.join(root, "scripts", "format.py"),
                        path.join(root, "xmake", "modules", "sgrn", "codegen.lua")}) do
        if os.isfile(f) then table.insert(files, f) end
    end
    local gen = path.absolute(config.buildir(), root)
    os.mkdir(path.join(gen, "gen"))
    depend.on_changed(run, {dependfile = path.join(gen, "gen", "sgrn_codegen.d"), files = files})
end
