-- xmake/modules/sgrn/harvester.lua
-- Replaces cmake/packaging.cmake and harvest_{linux,win64,arm64}.cmake.
-- Discovers non-system runtime shared libraries (AWS SDK, LibTorch, etc.)
-- and copies them into the binary staging directory alongside the executables.

local MANAGED_PATTERNS = {
    "libtorch", ".prefix"
}

local function _is_managed(path_str)
    for _, pat in ipairs(MANAGED_PATTERNS) do
        if path_str:find(pat, 1, true) then
            return true
        end
    end
    return false
end

function harvest_binary(bin_path, dest_dir)
    if not os.isfile(bin_path) then return end

    if is_plat("linux") or is_host("linux") then
        local out
        try {
            function () out = os.iorun("ldd " .. bin_path) end,
            catch { function (err) end }
        }
        if not out then return end

        for line in out:gmatch("[^\r\n]+") do
            local soname, libpath = line:match("%s*([^%s]+)%s*=>%s*(/[^%s]+)")
            if not soname then
                libpath = line:match("%s*(/[^%s]+)")
                if libpath then soname = path.filename(libpath) end
            end

            if libpath and _is_managed(libpath) then
                local norm_path = path.normalize(libpath)
                local file_name = path.filename(norm_path)
                local dest_file = path.join(dest_dir, file_name)

                if not os.isfile(dest_file) then
                    os.cp(norm_path, dest_file)
                end

                if soname and soname ~= file_name then
                    local dest_link = path.join(dest_dir, soname)
                    if not os.isfile(dest_link) then
                        os.cp(norm_path, dest_link)
                    end
                end
            end
        end
    end
end
