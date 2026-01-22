#!/bin/bash
# Test runner script for wd_test. Handles three modes:
# - Normal: runs workerd test directly
# - Sidecar: runs workerd test via supervisor which manages a parallel sidecar process
# - Snapshot: runs workerd twice (save snapshot, then load snapshot) for Python tests
#
# Environment variables (set by wd_test.bzl):
#   SIDECAR_COMMAND     - If set, enables sidecar mode
#   SIDECAR_SUPERVISOR  - Path to supervisor executable (required for sidecar mode)
#   PORTS_TO_ASSIGN     - Comma-separated port binding names for sidecar
#   RANDOMIZE_IP        - "true"/"false" for sidecar IP randomization
#   PYTHON_SNAPSHOT_TEST - If set, enables snapshot mode
#   PYTHON_SAVE_SNAPSHOT_ARGS - Additional args for snapshot save phase

set -euo pipefail

# Set up coverage for workerd subprocess.
# LLVM_PROFILE_FILE tells the coverage-instrumented workerd binary where to write profraw data.
# KJ_CLEAN_SHUTDOWN ensures proper shutdown so coverage data is flushed.
if [ -n "${COVERAGE_DIR:-}" ]; then
    export LLVM_PROFILE_FILE="$COVERAGE_DIR/%p.profraw"
    export KJ_CLEAN_SHUTDOWN=1
fi

run_test() {
    "$@" -dTEST_TMPDIR="${TEST_TMPDIR:-/tmp}"
}

# Sidecar mode: run via supervisor which starts sidecar process in parallel.
# The supervisor handles port assignment and ensures sidecar is ready before test starts.
if [ -n "${SIDECAR_COMMAND:-}" ]; then
    "$SIDECAR_SUPERVISOR" run_test "$@"

# Snapshot mode: run workerd twice for Python memory snapshot tests.
# First run creates the snapshot, second run uses it.
elif [ -n "${PYTHON_SNAPSHOT_TEST:-}" ]; then
    echo "Creating Python Snapshot"
    run_test "$@" --python-save-snapshot ${PYTHON_SAVE_SNAPSHOT_ARGS:-}
    echo ""
    echo "Using Python Snapshot"
    run_test "$@" --python-load-snapshot snapshot.bin

# Normal mode: just run the test.
else
    run_test "$@"
fi
