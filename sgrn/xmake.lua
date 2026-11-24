-- sgrn/xmake.lua
includes("lib")
if has_config("datastore") then
    includes("crud")
end
includes("apps")
