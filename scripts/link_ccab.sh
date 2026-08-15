#!/usr/bin/env bash
# scripts/link_ccab.sh
# Point compile_commands.json at a specific build preset's database.
# Usage: ./scripts/link_ccab.sh [preset]
#   preset defaults to "linux-static-release"
#
# This is needed because clangd reads compile_commands.json from the project
# root (via the .clangd CompilationDatabase directive). The symlink must stay
# valid or clangd will silently fall back to no index.

set -euo pipefail

PRESET="${1:-linux-static-release}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DB="${ROOT}/.build/${PRESET}/compile_commands.json"

if [[ ! -f "$DB" ]]; then
  echo "ERROR: compile_commands.json not found for preset '${PRESET}'"
  echo "       Expected: $DB"
  echo "       Run: cmake --preset ${PRESET} first."
  exit 1
fi

ln -sf ".build/${PRESET}/compile_commands.json" "${ROOT}/compile_commands.json"
echo "[clangd] compile_commands.json → .build/${PRESET}/compile_commands.json"
echo "         Restart clangd in nvim (:LspRestart) to pick up changes."
