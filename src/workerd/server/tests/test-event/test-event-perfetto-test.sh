#!/bin/bash

set -euo pipefail

# $1 -> workerd binary path
# $2 -> config whose tests pass and fail
WORKERD_BINARY=$1
CONFIG=$2
trace="$TEST_TMPDIR/test.pftrace"

# Some of the config's tests fail, so workerd test does too.
"$WORKERD_BINARY" test "$CONFIG" "--perfetto-trace=$trace=workerd" >/dev/null 2>&1 || true

# The test event has the spans that WorkerEntrypoint::test() had in C++. Perfetto interns names, so
# each appears in the file as plain bytes. Decoding the structure needs trace_processor, which
# isn't available here.
STATUS=0
for name in "WorkerEntrypoint::test()" "WorkerEntrypoint::test() run" \
  "WorkerEntrypoint::test() waitForFinished()"; do
  if ! grep -q -a -F "$name" "$trace"; then
    echo "missing span: $name" >&2
    STATUS=1
  fi
done
exit $STATUS
