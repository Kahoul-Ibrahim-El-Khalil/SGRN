-- deps-versions.lua : which version of each third-party dependency SGRN builds against.
--
-- Assigns the global table SGRN_DEPS (read by xmake/options.lua and xmake/packages.lua). Values:
--   "latest"        newest version the package source offers (xmake-repo / recipe)
--   "1.2.3"         an exact release
--   "<branch|tag|commit>"  for packages built from git (snap7, angelscript, opener, libmodbus,
--                          open62541, drogon, reproc, xml_h, sqlite_modern_cpp, glaze), e.g. "main"
-- Missing keys mean "latest". After editing run: xmake f -c -y
--
-- Defaults below match what the former CMake build pinned (FetchDeps.cmake), except where noted.
SGRN_DEPS = {
    -- pinned by the old CMake build
    fmt               = "12.2.0",
    jsoncpp           = "1.9.8",
    open62541         = "v1.5.7",
    angelscript       = "2.38.0",
    libmodbus         = "3.2.0",
    opener            = "8c8fa9f",    -- Pinned commit 8c8fa9f0df7d72ffe17a2443f8457017e71c68bf (defines CipAttributeEncodeInMessage)
    snap7             = "1.4.3",      -- davenardella/snap7 commit 30f37da (hardened by the recipe)
    drogon            = "1.9.14",
    reproc            = "14.2.5",
    xml_h             = "2.1",
    sqlite_modern_cpp = "3.2",

    -- CMake: crow 1.3.3, cpp-httplib 0.58.0, unordered_dense 4.9.2, ixwebsocket 12.1.0, glaze main.
    -- Left on "latest" because xmake-repo may not carry those exact versions; pin them if it does.
    crow              = "latest",
    ["cpp-httplib"]   = "latest",
    unordered_dense   = "latest",
    ixwebsocket       = "latest",
    glaze             = "latest",
    cxxopts           = "latest",
    asio              = "latest",

    -- taken from conda/system by the old CMake build
    zlib              = "latest",
    zstd              = "latest",
    openssl           = "latest",
    sqlite3           = "latest",
    libpq             = "17.12",
    libuuid           = "latest",
    ["c-ares"]        = "latest",

    -- LibTorch prebuilt zip downloaded from pytorch.org when no local install is found
    torch             = "2.5.1",
}
