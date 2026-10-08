#!/bin/bash

set -euo pipefail

# $1 -> workerd binary path
# $2 -> config that does the traced work
# $3 -> config that checks the trace
WORKERD_BINARY=$1
SCENARIO=$2
CHECK=$3

"$WORKERD_BINARY" test "$SCENARIO" --async-trace="$TEST_TMPDIR/trace.ndjson"
"$WORKERD_BINARY" test "$CHECK" -dtrace-dir="$TEST_TMPDIR"
