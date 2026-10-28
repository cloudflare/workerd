#!/bin/bash

# Test script to verify process stdio output against golden files
set -euo pipefail

# Arguments passed from Bazel
WORKERD_BINARY="$1"
TEST_CONFIG="$2"
EXPECTED_STDOUT="$3"
EXPECTED_STDERR="$4"

# Run the workerd test and capture stdout and stderr
ACTUAL_STDOUT=$(mktemp)
ACTUAL_STDERR=$(mktemp)
FILTERED_STDOUT=$(mktemp)
FILTERED_STDERR=$(mktemp)
trap "rm -f $ACTUAL_STDOUT $ACTUAL_STDERR $FILTERED_STDOUT $FILTERED_STDERR" EXIT

"$WORKERD_BINARY" test "$TEST_CONFIG" --experimental >"$ACTUAL_STDOUT" 2>"$ACTUAL_STDERR" --compat-date 2025-05-01

# Remove [ PASS ] [ TEST ] [ FAIL ] lines from stderr, and the warning logged when V8 rejects the
# per-isolate bootstrap's compile cache. Builds that generate the cache for other V8 flags
# (--@workerd//build/config:compile_cache_v8_flags) log it on every run of this binary.
grep -vE "\[ PASS \]|\[ FAIL \]|\[ TEST \]|per-isolate bootstrap compile cache" "$ACTUAL_STDERR" > "$FILTERED_STDERR" || true

# Compare with expected output (normalize line endings for cross-platform compatibility)
echo "Comparing stdout..."
if ! diff -u <(tr -d '\r' < "$EXPECTED_STDOUT") <(tr -d '\r' < "$ACTUAL_STDOUT"); then
    echo "FAIL: stdout does not match expected output"
    exit 1
fi

# Compare with expected output
echo "Comparing stderr..."
if ! diff -u <(tr -d '\r' < "$EXPECTED_STDERR") <(tr -d '\r' < "$FILTERED_STDERR"); then
    echo "FAIL: stderr does not match expected output"
    exit 1
fi

echo "PASS: All stdio output matches golden files"
