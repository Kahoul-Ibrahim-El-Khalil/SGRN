-- xmake/modules/sgrn/platform.lua
-- Maps the active xmake configuration to the SGRN platform naming that the
-- former CMake presets used (SGRN_PLATFORM): "linux", "linux-arm64", "win64",
-- with a "-static" suffix for static builds.

import("core.project.config")

-- "win64" | "linux-arm64" | "linux"
function triple()
    local plat = config.get("plat")
    local arch = config.get("arch")
    if plat == "mingw" or plat == "windows" then
        return "win64"
    end
    if arch == "arm64" or arch == "arm64-v8a" or arch == "aarch64" then
        return "linux-arm64"
    end
    return "linux"
end

-- Directory-name flavour used under .prefix/ and .dist/
function dirname()
    local kind = config.get("kind") or "static"
    return triple() .. (kind == "shared" and "" or "-static")
end

function is_cross()
    local plat = config.get("plat")
    return plat == "mingw" or plat == "windows" or plat == "cross"
end
