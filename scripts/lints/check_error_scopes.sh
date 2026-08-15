#!/bin/bash
set -e

BACKEND_FILE="sgrn/datastore/include/sgrn/datastore/BackendError.hpp"
FRONTEND_FILE="sgrn/datastore/web/types/src/index.ts"

if [ ! -f "$BACKEND_FILE" ]; then
    echo "Error: $BACKEND_FILE not found"
    exit 1
fi

if [ ! -f "$FRONTEND_FILE" ]; then
    echo "Error: $FRONTEND_FILE not found"
    exit 1
fi

# Extract scopes from BackendError.hpp (lines matching 'constexpr const char scope_*')
# The sed command extracts just the string value inside quotes
BACKEND_SCOPES=$(grep 'constexpr const char scope_' "$BACKEND_FILE" | grep -v 'scope_unknown' | sed -n 's/.*= "\(.*\)";/\1/p' | sort)

# Extract scopes from index.ts (lines inside `export const ErrorScope = { ... }`)
# We look for lines containing colon and quotes between 'export const ErrorScope = {' and '}'
FRONTEND_SCOPES=$(awk '/export const ErrorScope = \{/{flag=1; next} /\}/{flag=0} flag' "$FRONTEND_FILE" | grep ':' | grep -v 'Unknown' | sed -n 's/.*: "\(.*\)".*/\1/p' | sort)

if [ "$BACKEND_SCOPES" != "$FRONTEND_SCOPES" ]; then
    echo "ERROR: Mismatch between BackendError.hpp and @sgrn/types/index.ts ErrorScopes!"
    echo ""
    echo "Backend Scopes:"
    echo "$BACKEND_SCOPES"
    echo ""
    echo "Frontend Scopes:"
    echo "$FRONTEND_SCOPES"
    echo ""
    echo "Please ensure both files define the exact same error scopes."
    exit 1
fi

echo "Success: ErrorScopes match between backend and frontend."
exit 0
