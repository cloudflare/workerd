#!/bin/bash

# Test script to verify process stdio output against golden files
set -euo pipefail

# Find the test files in Bazel runfiles
if [[ -f "src/workerd/api/node/tests/process-stdio-nodejs-test.wd-test" ]]; then
    SCRIPT_DIR="src/workerd/api/node/tests"
else
    echo "ERROR: Cannot find test files"
    exit 1
fi

# Run the workerd test and capture stdout and stderr
ACTUAL_STDOUT=$(mktemp)
ACTUAL_STDERR=$(mktemp)
FILTERED_STDOUT=$(mktemp)
FILTERED_STDERR=$(mktemp)
trap "rm -f $ACTUAL_STDOUT $ACTUAL_STDERR $FILTERED_STDOUT $FILTERED_STDERR" EXIT

"$1" test "$SCRIPT_DIR/process-stdio-nodejs-test.wd-test" --experimental >"$ACTUAL_STDOUT" 2>"$ACTUAL_STDERR"

# Remove [ PASS ] [ TEST ] [ FAIL ] lines from stderr
grep -vE "\[ PASS \]|\[ FAIL \]|\[ TEST \]" "$ACTUAL_STDERR" > "$FILTERED_STDERR"

# Compare with expected output (normalize line endings for cross-platform compatibility)
echo "Comparing stdout..."
if ! diff -u <(tr -d '\r' < "$SCRIPT_DIR/process-stdio-nodejs-test.expected_stdout") <(tr -d '\r' < "$ACTUAL_STDOUT"); then
    echo "FAIL: stdout does not match expected output"
    exit 1
fi

# Compare with expected output
echo "Comparing stderr..."
if ! diff -u <(tr -d '\r' < "$SCRIPT_DIR/process-stdio-nodejs-test.expected_stderr") <(tr -d '\r' < "$FILTERED_STDERR"); then
    echo "FAIL: stderr does not match expected output"
    exit 1
fi

echo "PASS: All stdio output matches golden files"
