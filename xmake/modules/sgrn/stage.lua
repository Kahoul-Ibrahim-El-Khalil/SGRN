-- xmake/modules/sgrn/stage.lua
-- Auto-stages public SGRN headers into .prefix/include/sgrn

local HEADER_DIRS = {
    "sgrn/lib/core/include/sgrn",
    "sgrn/lib/utils/include/sgrn",
    "sgrn/lib/scl/include/sgrn",
    "sgrn/lib/scripting/include/sgrn",
    "sgrn/lib/gateway/include/sgrn",
    "sgrn/lib/wrappers/include/sgrn",
    "sgrn/lib/s7shell/include/sgrn",
    "sgrn/lib/plcsim/include/sgrn",
    "sgrn/lib/common/include/sgrn",
    "sgrn/lib/datastore/include/sgrn",
    "sgrn/lib/datastore/client/include/sgrn",
    "sgrn/lib/datastore/shell/include/sgrn",
}

function headers()
    local root = os.projectdir()
    local dst = path.join(root, ".prefix", "include", "sgrn")
    os.mkdir(dst)
    for _, src in ipairs(HEADER_DIRS) do
        local full = path.join(root, src)
        if os.isdir(full) then
            os.cp(path.join(full, "*"), dst)
        end
    end
end
