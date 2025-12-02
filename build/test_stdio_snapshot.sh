#!/bin/bash

# Generic test script to verify workerd test output against stdio snapshot files
# Usage: test_stdio_snapshot.sh WORKERD_BINARY TEST_CONFIG EXPECTED_STDOUT EXPECTED_STDERR [EXTRA_ARGS...]
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

# Normalize line numbers in output (e.g. worker:456: -> worker:***:)
normalize_line_numbers() {
    sed -E 's/:[0-9]+:/:***:/g' "$1"
}

# Compare stderr with expected output FIRST
if [[ -f "$EXPECTED_STDERR" ]]; then
    echo "Comparing stderr..."
    if ! diff -u <(normalize_line_numbers "$EXPECTED_STDERR" | tr -d '\r') \
                 <(normalize_line_numbers "$FILTERED_STDERR" | tr -d '\r') 2>/dev/null; then
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
if ! diff -u <(normalize_line_numbers "$EXPECTED_STDOUT" | tr -d '\r') \
             <(normalize_line_numbers "$ACTUAL_STDOUT" | tr -d '\r'); then
    echo "FAIL: stdout does not match expected output"
    exit 1
fi

echo "PASS: All output matches stdio snapshot files"
