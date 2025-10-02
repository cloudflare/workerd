#!/bin/bash

# Generic test script to verify workerd test output against golden files
# Usage: test_golden_output.sh WORKERD_BINARY TEST_CONFIG EXPECTED_STDOUT EXPECTED_STDERR [EXTRA_ARGS...]
set -euo pipefail

# Arguments
WORKERD_BINARY="$1"
TEST_CONFIG="$2"
EXPECTED_STDOUT="$3"
EXPECTED_STDERR="$4"
shift 4
EXTRA_ARGS=("$@")

# Create temp files
ACTUAL_STDOUT=$(mktemp)
ACTUAL_STDERR=$(mktemp)
FILTERED_STDERR=$(mktemp)
trap "rm -f $ACTUAL_STDOUT $ACTUAL_STDERR $FILTERED_STDERR" EXIT

# Run the workerd test and capture stdout and stderr
if ! "$WORKERD_BINARY" test "$TEST_CONFIG" "${EXTRA_ARGS[@]}" >"$ACTUAL_STDOUT" 2>"$ACTUAL_STDERR"; then
    echo "Test failed with non-zero exit code. stderr content:"
    cat "$ACTUAL_STDERR"
    exit 1
fi

# Remove [ PASS ] [ TEST ] [ FAIL ] lines from stderr
grep -vE "\[ PASS \]|\[ FAIL \]|\[ TEST \]" "$ACTUAL_STDERR" > "$FILTERED_STDERR" || true

# Compare stderr with expected output FIRST
if [[ -f "$EXPECTED_STDERR" ]]; then
    echo "Comparing stderr..."
    EXPECTED_STDERR_CONTENT=$(tr -d '\r' < "$EXPECTED_STDERR")
    if ! diff -u <(echo "$EXPECTED_STDERR_CONTENT") <(tr -d '\r' < "$FILTERED_STDERR") 2>/dev/null; then
        echo "FAIL: stderr does not match expected output"
        echo "STDOUT:"
        cat "$ACTUAL_STDOUT"
        exit 1
    fi
elif [[ -s "$FILTERED_STDERR" ]]; then
    echo "Unexpected STDERR:"
    cat "$FILTERED_STDERR"
    exit 1
fi

# Compare stdout with expected output (normalize line endings for cross-platform compatibility)
echo "Comparing stdout..."
if ! diff -u <(tr -d '\r' < "$EXPECTED_STDOUT") <(tr -d '\r' < "$ACTUAL_STDOUT"); then
    echo "FAIL: stdout does not match expected output"
    exit 1
fi

echo "PASS: All output matches golden files"
