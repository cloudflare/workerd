#!/bin/bash

set -euo pipefail

# $1 -> workerd binary path
# $2 -> config with one bench case
WORKERD_BINARY=$1
CONFIG=$2
trace="$TEST_TMPDIR/bench.pftrace"

"$WORKERD_BINARY" bench "$CONFIG" --no-startup "--perfetto-trace=$trace=workerd" >/dev/null

# The bench event has the spans of WorkerEntrypoint::bench(). Perfetto interns names, so each
# appears in the file as plain bytes. Decoding the structure needs trace_processor, which isn't
# available here. The first name is a prefix of the second, so it must be followed by something
# other than the space that continues it.
STATUS=0
for name in 'WorkerEntrypoint::bench\(\)([^ ]|$)' 'WorkerEntrypoint::bench\(\) run'; do
  if ! LC_ALL=C grep -q -a -E "$name" "$trace"; then
    echo "missing span: $name" >&2
    STATUS=1
  fi
done
exit $STATUS
