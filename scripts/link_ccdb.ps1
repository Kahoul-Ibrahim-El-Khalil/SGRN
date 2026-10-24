# Copy the compile_commands.json of a CMake preset into the project root,
# where the root .clangd (CompilationDatabase: ".") expects to find it.
# Usage:  powershell -File scripts/link_ccdb.ps1 [-Preset win64-native-static]
param(
    [string]$Preset = "win64-native-static"
)

$ErrorActionPreference = "Stop"
$src = Join-Path $PSScriptRoot "..\.build\$Preset\compile_commands.json"
$dst = Join-Path $PSScriptRoot "..\compile_commands.json"

if (-not (Test-Path $src)) {
    Write-Error "compile_commands.json not found for preset '$Preset' at: $src`nConfigure the preset first (cmake --preset $Preset)."
}

Copy-Item $src $dst -Force
Write-Host "compile_commands.json <- preset '$Preset' copied to project root."
