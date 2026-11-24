-- LibTorch. Resolution order:
--   1. `xmake f --torch_dir=...`, $LIBTORCH, $TORCH_DIR
--   2. $CONDA_PREFIX/lib/python*/site-packages/torch
--   3. cached download in .prefix/libtorch-<version>-<variant>/
--   4. download the official prebuilt zip (pytorch.org) into that cache
-- Overrides: $SGRN_TORCH_URL (mirror / other ABI or version), $SGRN_TORCH_SHA256 (verify archive).
-- Options: torch_version (default 2.5.1), torch_device (cpu|gpu -> cu124), torch_download=n to disable step 4.
package("sgrn_torch")
    set_description("LibTorch (local install or official prebuilt download)")

    on_fetch(function (package, opt)
        if not opt.system then return end
        import("core.project.config")
        import("net.http")
        import("utils.archive")
        import("lib.detect.find_directory")

        local function valid(root)
            return root and os.isdir(path.join(root, "include", "torch")) and os.isdir(path.join(root, "lib"))
        end

        local root = config.get("torch_dir")
        if type(root) ~= "string" or root == "" then
            root = os.getenv("LIBTORCH") or os.getenv("TORCH_DIR")
        end
        if not valid(root) then
            root = nil
            local conda = os.getenv("CONDA_PREFIX")
            if conda then
                local m = os.dirs(path.join(conda, "lib", "python*", "site-packages", "torch"))
                if m and #m > 0 and valid(m[1]) then root = m[1] end
            end
        end

        if not root then
            local version = config.get("torch_version")
            if type(version) ~= "string" or version == "" then version = "2.5.1" end
            local gpu = config.get("torch_device") == "gpu"
            local variant = gpu and "cu124" or "cpu"
            local cache = path.join(os.projectdir(), ".prefix", "libtorch-" .. version .. "-" .. variant)
            if valid(path.join(cache, "libtorch")) then
                root = path.join(cache, "libtorch")
            elseif config.get("torch_download") ~= false then
                local url = os.getenv("SGRN_TORCH_URL") or
                    format("https://download.pytorch.org/libtorch/%s/libtorch-cxx11-abi-shared-with-deps-%s%%2B%s.zip",
                           variant, version, variant)
                local zip = path.join(cache, "libtorch.zip")
                os.mkdir(cache)
                cprint("${color.build.object}[torch] downloading %s", url)
                http.download(url, zip)
                local want = os.getenv("SGRN_TORCH_SHA256")
                if want and want ~= "" then
                    import("core.base.hash")
                    assert(hash.sha256(zip) == want:lower(), "[torch] sha256 mismatch for " .. url)
                end
                cprint("${color.build.object}[torch] extracting")
                archive.extract(zip, cache)
                os.rm(zip)
                if valid(path.join(cache, "libtorch")) then root = path.join(cache, "libtorch") end
            end
        end
        if not root then return end

        local libdir = path.join(root, "lib")
        -- Link the torch libraries by absolute path instead of adding torch's
        -- lib dir to the global -L search path. The prebuilt LibTorch drops
        -- extra libraries (notably libfmt.a, built against an older fmt) into
        -- lib/, and a preceding -L would make `-lfmt` resolve to that copy
        -- instead of xmake's pinned fmt package.
        local names = {"torch", "torch_cpu", "c10"}
        if #os.files(path.join(libdir, "libtorch_cuda.*")) > 0 then
            table.join2(names, {"torch_cuda", "c10_cuda"})
        end
        -- Pass the torch libraries as absolute paths via ldflags. Packages have
        -- no "linkfiles" field (xmake would ignore it), and using linkdirs+
        -- links would put torch's lib/ ahead of the pinned packages in -L
        -- order, so `-lfmt` would pick up torch's bundled (older) libfmt.a.
        local ldflags = {"-Wl,--no-as-needed"}
        for _, name in ipairs(names) do
            for _, file in ipairs({
                path.join(libdir, "lib" .. name .. ".so"),
                path.join(libdir, "lib" .. name .. ".dylib"),
                path.join(libdir, "lib" .. name .. ".a"),
            }) do
                if os.isfile(file) then
                    table.insert(ldflags, file)
                    break
                end
            end
        end
        return {
            includedirs = {path.join(root, "include"),
                           path.join(root, "include", "torch", "csrc", "api", "include")},
            rpathdirs = {libdir},
            ldflags = ldflags
        }
    end)
