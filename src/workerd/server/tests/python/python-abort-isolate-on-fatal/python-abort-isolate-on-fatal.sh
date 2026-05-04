#!/bin/bash
# Test that when the python-abort-isolate-on-fatal-error autogate is enabled,
# triggering a Python fatal error causes the workerd process to abort.

set -uo pipefail

WORKERD=$1
CONFIG=$2
PYODIDE_BUNDLE=$3
WHEELS_DIR=$4

TEST_TMPDIR="${TEST_TMPDIR:-/tmp}"

# Pyodide bundle directory is the parent of the .capnp.bin file.
BUNDLE_DIR=$(dirname "$PYODIDE_BUNDLE")

output=$("$WORKERD" test \
  "$CONFIG" \
  --experimental \
  --compat-date=2000-01-01 \
  --pyodide-bundle-disk-cache-dir "$BUNDLE_DIR" \
  --pyodide-package-disk-cache-dir "$WHEELS_DIR" \
  -dTEST_TMPDIR="$TEST_TMPDIR" 2>&1)
exit_code=$?

echo "--- captured output ---" >&2
echo "$output" >&2
echo "--- end captured output ---" >&2

if [ "$exit_code" -eq 0 ]; then
  echo "FAIL: expected nonzero exit code from abortIsolate" >&2
  exit 1
fi

if ! echo "$output" | grep -qF "abortIsolate() called, terminating process"; then
  echo "FAIL: expected abortIsolate fatal message not found" >&2
  exit 1
fi

if ! echo "$output" | grep -qF "Python worker fatal error"; then
  echo "FAIL: expected Python worker fatal error reason not found" >&2
  exit 1
fi

echo "Success"
exit 0
