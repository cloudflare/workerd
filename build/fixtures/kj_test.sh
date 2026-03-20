#!/bin/bash
set -euo pipefail

if [ -n "${COVERAGE_DIR:-}" ]; then
    export LLVM_PROFILE_FILE="$COVERAGE_DIR/%p-%m.profraw"
fi

exec "$@"
