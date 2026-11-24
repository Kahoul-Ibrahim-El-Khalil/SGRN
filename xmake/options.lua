-- xmake/options.lua
-- Replaces the CMake cache options (SGRN_BUILD_*, SGRN_WITH_TORCH, USE_PCH,
-- SGRN_SANITIZE, ...). Set with `xmake f --name=value`.

local cross = is_plat("mingw", "windows", "cross")
local versions = SGRN_DEPS or {}

option("datastore")
    set_default(not cross)
    set_showmenu(true)
    set_description("Build the datastore backend (Drogon/Postgres); default: native Linux only")

option("torch")
    set_default(not is_plat("mingw", "windows"))
    set_showmenu(true)
    set_description("Build the LibTorch backend (sgrn_ml_torch, sgrn_train); LibTorch is found or downloaded automatically")

option("torch_version")
    set_default(type(versions.torch) == "string" and versions.torch ~= "latest" and versions.torch or "2.5.1")
    set_showmenu(true)
    set_description("LibTorch version to download when none is installed")

option("torch_download")
    set_default(true)
    set_showmenu(true)
    set_description("Download the prebuilt LibTorch when none is found")

option("opc")
    set_default(true)
    set_showmenu(true)
    set_description("Build OPC UA support in s7shell")

option("tests")
    set_default(true)
    set_showmenu(true)
    set_description("Define the test executables (run with `xmake test`)")

option("pch")
    set_default(false)
    set_showmenu(true)
    set_description("Use precompiled headers (tiers in xmake/pch/)")

option("native")
    set_default(not cross)
    set_showmenu(true)
    set_description("Pass -march=native in release builds")

option("ucrt")
    set_default(true)
    set_showmenu(true)
    set_description("Target UCRT on mingw (defines _UCRT, links -lucrt)")

option("web")
    set_default(true)
    set_showmenu(true)
    set_description("Build web dashboards with bun (n = reuse existing dist/build output)")

option("sanitize")
    set_default("")
    set_showmenu(true)
    set_description("Sanitizer: asan | ubsan | tsan | asan,ubsan")

option("torch_dir")
    set_default("")
    set_showmenu(true)
    set_description("LibTorch root (else $LIBTORCH, $TORCH_DIR, or conda site-packages/torch)")

option("torch_device")
    set_default("")
    set_showmenu(true)
    set_description("Default torch device: cpu | gpu")

option("datastore_release")
    set_default(false)
    set_showmenu(true)
    set_description("Datastore: -O3 -DNDEBUG -flto and lld (former RELEASE option)")

option("datastore_debug")
    set_default("")
    set_showmenu(true)
    set_description("Datastore: comma-separated DEBUG_* flags, e.g. DEBUG_AUTH_HANDLER,DEBUG_S7_CORE")

option("auto_codegen")
    set_default(true)
    set_showmenu(true)
    set_description("Regenerate ORM models / CRUD views automatically when SQL or manifest change")
