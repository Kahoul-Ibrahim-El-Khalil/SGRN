-- xmake/modules/sgrn/vfs_generator.lua
-- Native Lua replacement for scripts/generate_embedded_assets.py.
-- Generates Zstd-compressed C++ Embedded Virtual File System (VFS) headers.

local MIME_MAP = {
    [".html"]  = "text/html; charset=utf-8",
    [".css"]   = "text/css; charset=utf-8",
    [".js"]    = "application/javascript; charset=utf-8",
    [".mjs"]   = "application/javascript; charset=utf-8",
    [".svg"]   = "image/svg+xml",
    [".png"]   = "image/png",
    [".jpg"]   = "image/jpeg",
    [".jpeg"]  = "image/jpeg",
    [".ico"]   = "image/x-icon",
    [".webp"]  = "image/webp",
    [".woff"]  = "font/woff",
    [".woff2"] = "font/woff2",
    [".json"]  = "application/json",
    [".txt"]   = "text/plain; charset=utf-8",
    [".sql"]   = "text/plain; charset=utf-8",
    [".toml"]  = "text/plain; charset=utf-8",
}

local KIND_MAP = {
    web    = "sgrn::AssetKind::Web",
    sql    = "sgrn::AssetKind::Sql",
    config = "sgrn::AssetKind::Config",
    cert   = "sgrn::AssetKind::Cert",
    other  = "sgrn::AssetKind::Other",
}

local DEFAULT_EXTENSIONS = {
    web    = {".html", ".js", ".mjs", ".css", ".svg", ".png", ".jpg", ".jpeg", ".ico", ".webp", ".woff", ".woff2"},
    sql    = {".sql"},
    config = {".conf", ".service"},
    cert   = {".pem", ".crt", ".key", ".cer"},
}

-- ── Text & encoding helpers ───────────────────────────────────────────────────

local function _make_identifier(rel_path)
    local s = rel_path:gsub("[^%w]", "_"):gsub("_+", "_"):gsub("^_", ""):gsub("_$", "")
    return s:upper()
end

local function _bytes_to_cpp(data)
    local lines = {}
    local chunk = {}
    for i = 1, #data do
        table.insert(chunk, string.format("0x%02X", string.byte(data, i)))
        if #chunk == 16 then
            table.insert(lines, "    " .. table.concat(chunk, ", "))
            chunk = {}
        end
    end
    if #chunk > 0 then
        table.insert(lines, "    " .. table.concat(chunk, ", "))
    end
    return table.concat(lines, ",\n")
end

local function _strip_sql_comments(text)
    text = text:gsub("/%*.-%*/", "")
    local lines = {}
    for line in (text .. "\n"):gmatch("(.-)\r?\n") do
        table.insert(lines, (line:gsub("%-%-.*$", "")))
    end
    text = table.concat(lines, "\n"):gsub("\n\n\n+", "\n\n")
    return text:match("^%s*(.-)%s*$") or ""
end

local function _flatten_sql(filepath)
    local dir = path.directory(filepath)
    local content = io.readfile(filepath) or ""
    local out_lines = {}
    for line in (content .. "\n"):gmatch("(.-)\r?\n") do
        local stripped = line:match("^%s*(.-)%s*$") or ""
        if stripped:sub(1, 3) == "\\i " or stripped:sub(1, 4) == "\\ir " then
            local inc_rel = stripped:match("^\\i[r]?%s+(.+)$")
            if inc_rel then
                inc_rel = inc_rel:gsub("^['\"]", ""):gsub("['\"]$", "")
                table.insert(out_lines, "-- INLINED: " .. inc_rel)
                table.insert(out_lines, _flatten_sql(path.join(dir, inc_rel)))
            end
        else
            table.insert(out_lines, line)
        end
    end
    return _strip_sql_comments(table.concat(out_lines, "\n"))
end

local function _strip_config_comments(text)
    local lines = {}
    for line in (text .. "\n"):gmatch("(.-)\r?\n") do
        local stripped = line:match("^%s*(.-)%s*$") or ""
        if not stripped:find("^#") then
            table.insert(lines, (line:gsub("%s+#[^\n]*", "")))
        end
    end
    return (table.concat(lines, "\n"):gsub("\n\n\n+", "\n\n")):match("^%s*(.-)%s*$") or ""
end

local function _read_asset_bytes(file_path, kind)
    if kind == "sql" then
        return _flatten_sql(file_path)
    end
    if kind == "config" then
        local ext = path.extension(file_path):lower()
        if ext == ".conf" or ext == ".service" or ext == ".ini" then
            return _strip_config_comments(io.readfile(file_path) or "")
        end
    end
    return io.readfile(file_path, {encoding = "binary"}) or ""
end

local function _compress(raw_bytes, level)
    local tmpin = os.tmpfile() .. ".raw"
    local tmpout = os.tmpfile() .. ".zst"
    io.writefile(tmpin, raw_bytes, {encoding = "binary"})
    local lvl = tonumber(level) or 19
    local args = {"-" .. tostring(lvl)}
    if lvl > 19 then
        table.insert(args, "--ultra")
    end
    table.join2(args, {"-f", "-q", tmpin, "-o", tmpout})
    os.vrunv("zstd", args)
    local compressed = io.readfile(tmpout, {encoding = "binary"})
    os.rm(tmpin)
    os.rm(tmpout)
    return compressed
end

-- ── File filtering & header emission ──────────────────────────────────────────

local function _build_extension_set(opt, kind)
    local list = (opt.extensions and #opt.extensions > 0) and opt.extensions or DEFAULT_EXTENSIONS[kind]
    if not list then return nil end
    local set = {}
    for _, ext in ipairs(list) do
        local e = ext:lower()
        if not e:find("^%.") then e = "." .. e end
        set[e] = true
    end
    return set
end

local function _should_include_file(filepath, rel_posix, ext, kind, src_dir, allowed_exts, excludes)
    if allowed_exts and not allowed_exts[ext] then
        return false
    end
    -- For SQL, only root init.sql is the entry point (it inlines all others).
    if kind == "sql" and (path.filename(filepath) ~= "init.sql" or path.directory(filepath) ~= src_dir) then
        return false
    end
    for _, pat in ipairs(excludes) do
        if rel_posix:find(pat, 1, true) or path.filename(filepath) == pat then
            return false
        end
    end
    return true
end

local function _generate_asset_header(file_path, out_dir, level, namespace, rel_path, kind)
    local raw_bytes = _read_asset_bytes(file_path, kind)
    local original_size = #raw_bytes
    local compressed = _compress(raw_bytes, level)
    local symbol = _make_identifier(rel_path)
    local header_stem = rel_path:gsub("[/\\]", "_")
    local header_path = path.join(out_dir, header_stem .. ".hpp")

    cprint("  Compressing %s: %d -> %d bytes", path.filename(file_path), original_size, #compressed)

    local content = string.format([[// Auto-generated — do not edit. Source: %s
// Payload compressed with Zstd. High entropy by design (IP protection).
#pragma once
#include <cstdint>
#include <cstddef>

namespace %s {

// .rodata — Zstd-compressed payload for %s
inline constexpr uint8_t %s_DATA[] = {
%s
};

inline constexpr size_t %s_COMPRESSED_SIZE = sizeof(%s_DATA);
inline constexpr size_t %s_ORIGINAL_SIZE   = %d;

} // namespace %s
]], rel_path, namespace, rel_path, symbol, _bytes_to_cpp(compressed),
    symbol, symbol, symbol, original_size, namespace)

    io.writefile(header_path, content)
    return symbol, header_stem
end

local function _generate_master_header(out_dir, namespace, kind, virtual_prefix, assets, master_name)
    local kind_cpp = KIND_MAP[kind] or "sgrn::AssetKind::Other"
    local master_path = path.join(out_dir, master_name)

    local lines = {
        "// Auto-generated master asset header — do not edit.",
        "// Namespace : " .. namespace,
        "// Kind      : " .. kind,
        "#pragma once",
        "#include <sgrn/assets/EmbeddedAsset.hpp>\n",
    }
    for _, a in ipairs(assets) do
        table.insert(lines, string.format('#include "%s.hpp"', a.header_stem))
    end

    table.insert(lines, string.format("\nnamespace %s {\n", namespace))
    table.insert(lines, "// .rodata — asset descriptor table")
    table.insert(lines, "inline constexpr sgrn::EmbeddedAsset ASSETS[] = {")
    for _, a in ipairs(assets) do
        local vpath = (virtual_prefix and virtual_prefix ~= "")
            and ((virtual_prefix .. "/" .. a.virtual_path):gsub("//+", "/"))
            or a.virtual_path
        table.insert(lines, string.format('    {"%s", "%s",', vpath, a.mime))
        table.insert(lines, string.format('      %s_DATA, %s_COMPRESSED_SIZE, %s_ORIGINAL_SIZE,', a.symbol, a.symbol, a.symbol))
        table.insert(lines, string.format('      %s},', kind_cpp))
    end
    table.insert(lines, "};\n")
    table.insert(lines, "// Compile-time sorted registry — O(log N) lookup, zero heap allocation.")
    table.insert(lines, "inline constexpr auto VFS = sgrn::AssetRegistry(ASSETS);\n")
    table.insert(lines, "inline constexpr size_t ASSET_COUNT = VFS.size();\n")
    table.insert(lines, string.format("} // namespace %s\n", namespace))

    io.writefile(master_path, table.concat(lines, "\n"))
    cprint("${color.build.target}Generated master header: %s", master_path)
end

-- ── Public entry point ────────────────────────────────────────────────────────

function generate(opt)
    local src_dir = opt.src_dir
    local out_dir = opt.out_dir
    local namespace = opt.namespace or "sgrn::datastore::assets::web"
    local kind = opt.kind or "web"
    local level = opt.level or 19
    local master_name = opt.master or "assets.hpp"
    local virtual_prefix = opt.virtual_prefix or ""
    local excludes = opt.exclude or {}

    assert(os.isdir(src_dir), "[vfs_generator] source directory does not exist: " .. tostring(src_dir))
    os.mkdir(out_dir)

    local allowed_exts = _build_extension_set(opt, kind)
    local all_files = os.files(path.join(src_dir, "**"))
    table.sort(all_files)

    local assets = {}
    for _, f in ipairs(all_files) do
        local ext = path.extension(f):lower()
        local rel_posix = path.relative(f, src_dir):gsub("\\", "/")
        if _should_include_file(f, rel_posix, ext, kind, src_dir, allowed_exts, excludes) then
            local symbol, header_stem = _generate_asset_header(f, out_dir, level, namespace, rel_posix, kind)
            table.insert(assets, {
                symbol       = symbol,
                header_stem  = header_stem,
                virtual_path = rel_posix,
                mime         = MIME_MAP[ext] or "application/octet-stream",
            })
        end
    end

    assert(#assets > 0, "[vfs_generator] no matching files found in " .. src_dir)
    _generate_master_header(out_dir, namespace, kind, virtual_prefix, assets, master_name)
end
