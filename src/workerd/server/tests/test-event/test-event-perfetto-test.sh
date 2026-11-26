#!/bin/bash

set -euo pipefail

# $1 -> workerd binary path
# $2 -> config whose tests pass and fail
WORKERD_BINARY=$1
CONFIG=$2
trace="$TEST_TMPDIR/test.pftrace"

# Some of the config's tests fail, so workerd test does too.
"$WORKERD_BINARY" test "$CONFIG" "--perfetto-trace=$trace=workerd" >/dev/null 2>&1 || true

# The test event has the spans of WorkerEntrypoint::test(). Perfetto interns names, so each
# appears in the file as plain bytes. Decoding the structure needs trace_processor, which isn't
# available here. The first name is a prefix of the other two, so it must be followed by something
# other than the space that continues them.
STATUS=0
for name in 'WorkerEntrypoint::test\(\)([^ ]|$)' 'WorkerEntrypoint::test\(\) run' \
  'WorkerEntrypoint::test\(\) waitForFinished\(\)'; do
  if ! LC_ALL=C grep -q -a -E "$name" "$trace"; then
    echo "missing span: $name" >&2
    STATUS=1
  fi
done
exit $STATUS
