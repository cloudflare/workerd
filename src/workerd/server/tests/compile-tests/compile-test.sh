#!/bin/bash

set -euo pipefail

# $1 -> workerd binary path
# $2 -> path to desired file to compile
# $3 -> expected output file
WORKERD_BINARY=$1
CAPNP_SOURCE=$2
EXPECTED=$3

# Bazel creates a fresh, writable $TEST_TMPDIR for each test and cleans it up after the test finishes.
CAPNP_BINARY=$TEST_TMPDIR/compiled-workerd
PORT_FILE=$TEST_TMPDIR/port

# Compile the app
$WORKERD_BINARY compile "$CAPNP_SOURCE" > "$CAPNP_BINARY"

# Run the app
$CAPNP_BINARY -shttp=localhost:0 --control-fd=1 > "$PORT_FILE" &
CAPNP_BINARY_PID=$!

cleanup() {
  kill $CAPNP_BINARY_PID 2>/dev/null || true
  wait $CAPNP_BINARY_PID 2>/dev/null || true
}
trap cleanup EXIT

OUTPUT=$TEST_TMPDIR/output
FIXED_OUTPUT=$TEST_TMPDIR/fixed-output
FIXED_EXPECTED=$TEST_TMPDIR/fixed-expected

# Wait on the port bindings to occur
while ! grep -q '"socket":"http"' "$PORT_FILE"; do
  if ! kill -0 $CAPNP_BINARY_PID 2>/dev/null; then
    wait $CAPNP_BINARY_PID && STATUS=0 || STATUS=$?
    echo "Compiled workerd exited with code $STATUS before binding its HTTP socket." >&2
    exit 1
  fi
  sleep .1
done

# Identify the port chosen by the binary
PORT=$(grep '"socket":"http"' "$PORT_FILE" | sed 's/^.*\"port\"://g' | sed 's/\}//g' |head -n 1)

# Request output
curl localhost:"$PORT" -o "$OUTPUT"

# Compare the tests to the expected output
sed -e's/[[:space:]]*$//' "$OUTPUT" > "$FIXED_OUTPUT"
sed -e's/[[:space:]]*$//' "$EXPECTED" > "$FIXED_EXPECTED"
diff "$FIXED_OUTPUT" "$FIXED_EXPECTED"
