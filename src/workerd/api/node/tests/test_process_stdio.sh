#!/bin/bash

# Test script to verify process stdio output against golden files
set -euo pipefail

# Find the test files in Bazel runfiles
if [[ -f "src/workerd/api/node/tests/process-stdio-test.wd-test" ]]; then
    SCRIPT_DIR="src/workerd/api/node/tests"
else
    echo "ERROR: Cannot find test files"
    exit 1
fi

# Temporary files for actual output
ACTUAL_STDOUT=$(mktemp)
ACTUAL_STDERR=$(mktemp)

# Cleanup on exit
cleanup() {
    rm -f "$ACTUAL_STDOUT" "$ACTUAL_STDERR"
}
trap cleanup EXIT

# Run the workerd test and capture stdout/stderr
"$1" test "$SCRIPT_DIR/process-stdio-test.wd-test" --experimental \
    > "$ACTUAL_STDOUT" \
    2> "$ACTUAL_STDERR"

# Strip first and last lines from stderr (debug output) before comparing
ACTUAL_STDERR_CLEAN=$(mktemp)
sed '1d;$d' "$ACTUAL_STDERR" > "$ACTUAL_STDERR_CLEAN"

# Compare outputs with golden files
echo "Comparing stdout..."
if ! diff -u "$SCRIPT_DIR/process-stdio-test.expected_stdout" "$ACTUAL_STDOUT"; then
    echo "FAIL: stdout does not match expected output"
    exit 1
fi

echo "Comparing stderr..."
if ! diff -u "$SCRIPT_DIR/process-stdio-test.expected_stderr" "$ACTUAL_STDERR_CLEAN"; then
    echo "FAIL: stderr does not match expected output"  
    exit 1
fi

# Clean up the extra temp file
rm -f "$ACTUAL_STDERR_CLEAN"

echo "PASS: All stdio output matches golden files"