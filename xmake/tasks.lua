-- xmake/tasks.lua : custom tasks (former custom CMake targets)

task("stage")
    set_menu {usage = "xmake stage", description = "Stage public headers into .prefix/include", options = {}}
    on_run(function ()
        local root = os.projectdir()
        local dst = path.join(root, ".prefix", "include", "sgrn")
        os.mkdir(dst)
        for _, src in ipairs({
            "sgrn/lib/core/include/sgrn", "sgrn/lib/utils/include/sgrn",
            "sgrn/lib/scl/include/sgrn", "sgrn/lib/scripting/include/sgrn",
            "sgrn/lib/gateway/include/sgrn", "sgrn/lib/wrappers/include/sgrn",
            "sgrn/lib/s7shell/include/sgrn", "sgrn/lib/plcsim/include/sgrn",
            "sgrn/lib/common/include/sgrn", "sgrn/lib/datastore/include/sgrn",
            "sgrn/lib/datastore/client/include/sgrn", "sgrn/lib/datastore/shell/include/sgrn"}) do
            local full = path.join(root, src)
            if os.isdir(full) then os.cp(path.join(full, "*"), dst) end
        end
        cprint("[SGRN] staged headers to %s", dst)
    end)

task("generate-views")
    set_menu {usage = "xmake generate-views",
              description = "Regenerate Drogon ORM models and CRUD views (SQL schemas + crud/manifest.json)",
              options = {}}
    on_run(function ()
        import("sgrn.codegen").run()
    end)

task("crud-generate")
    set_menu {usage = "xmake crud-generate",
              description = "Regenerate SGRN CRUD handlers from YAML manifests", options = {}}
    on_run(function ()
        import("lib.detect.find_program")
        local py = find_program("python3") or find_program("python")
        assert(py, "python3 not found")
        local dir = path.join(os.projectdir(), "sgrn", "crud")
        os.vrunv(py, {path.join(dir, "generator", "generate_crud.py")}, {curdir = dir})
    end)
