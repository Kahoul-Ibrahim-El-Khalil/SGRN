#!/usr/bin/env bash
# check_boundaries.sh
# Checks if boundary error types are used outside their home directories
# in files not matching a *_bridge.* or *_boundary.* convention.

set -e

# Configuration
# Format: "ErrorType|AllowedDirectory"
declare -a BOUNDARIES=(
    "scl::Error|sgrn/scl/"
    "S7Error|sgrn/gateway/.*s7/"
    "PlcMemoryError|sgrn/gateway/.*twin/"
)

FAIL=0

echo "Running Error Boundary Linter..."

for entry in "${BOUNDARIES[@]}"; do
    ERROR_TYPE="${entry%%|*}"
    ALLOWED_DIR="${entry##*|}"

    # Search for the error type in the whole codebase
    MATCHES=$(git grep -n -w "$ERROR_TYPE" -- "sgrn/*" | \
        grep -vE "^$ALLOWED_DIR" | \
        grep -vE "_bridge\.(cpp|hpp|h)$|_boundary\.(cpp|hpp|h)$" | \
        grep -v "check_boundaries.sh" | \
        grep -v "docs/" | \
        grep -v "tests/" || true)

    if [ -n "$MATCHES" ]; then
        # We found potential violations.
        # Allow specific legacy files that serve as boundaries but haven't been renamed,
        # or establish a baseline for legacy code that will be fixed in future phases.
        FILTERED_MATCHES=$(echo "$MATCHES" | \
                           grep -v "sgrn/gateway/src/twin/" | \
                           grep -v "sgrn/gateway/include/sgrn/gateway/twin/" | \
                           grep -v "sgrn/s7shell/src/PlcTagTable.cpp" | \
                           grep -v "sgrn/s7shell/src/connection/ScriptS7Connection.cpp" | \
                           grep -v "sgrn/s7shell/src/connection/S7Connection.cpp" | \
                           grep -v "sgrn/s7shell/src/connection/S7ShellServer.cpp" | \
                           grep -v "sgrn/s7shell/src/S7BatchEngine.cpp" | \
                           grep -v "sgrn/s7shell/include/sgrn/s7shell/connection/S7Connection.hpp" | \
                           grep -v "sgrn/s7shell/include/sgrn/s7shell/PlcTagTable.hpp" | \
                           grep -v "sgrn/s7shell/src/script/" | \
                           grep -v "sgrn/s7shell/src/utils/" | \
                           grep -v "sgrn/s7shell/include/" | \
                           grep -v "sgrn/scl/include/sgrn/scl/" | \
                           grep -v "sgrn/gateway/src/io/s7_address_utils.cpp" | \
                           grep -v "sgrn/gateway/src/adapters/modbus/ModbusAdapter.cpp" | \
                           grep -v "sgrn/gateway/include/sgrn/gateway/adapters/modbus/ModbusAdapter.hpp" | \
                           grep -v "sgrn/gateway/include/sgrn/gateway/wrappers/s7/ProtocolError.hpp" | \
                           grep -v "sgrn/gateway/src/adapters/http/memory_handlers.cpp" || true)
        
        # Only fail if there are any non-whitelisted matches left
        if [ -n "$FILTERED_MATCHES" ]; then
            echo "❌ Boundary violation found for '$ERROR_TYPE':"
            echo "$FILTERED_MATCHES"
            FAIL=1
        fi
    fi
done

if [ $FAIL -eq 0 ]; then
    echo "✅ No boundary violations found."
    exit 0
else
    echo "❌ Error boundaries were violated. Please fix the above issues."
    exit 1
fi
