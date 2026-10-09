#!/bin/bash

set -euo pipefail

# $1 -> workerd binary path
# $2 -> config that does the traced work
WORKERD_BINARY=$1
SCENARIO=$2

ON=$TEST_TMPDIR/on.pftrace
OFF=$TEST_TMPDIR/off.pftrace
"$WORKERD_BINARY" test "$SCENARIO" "--perfetto-trace=$ON=workerd,workerd.async"
"$WORKERD_BINARY" test "$SCENARIO" "--perfetto-trace=$OFF=workerd"

# Perfetto interns names, so each appears in the file as plain bytes. Decoding the structure needs
# trace_processor, which isn't available here.
NAMES=("scenario ctx" "setTimeout" "scheduler.wait" "queueMicrotask" "awaitIo" "turn" "ctx_end")
STATUS=0
for name in "${NAMES[@]}"; do
  if ! grep -q -a -F "$name" "$ON"; then
    echo "missing with workerd.async: $name" >&2
    STATUS=1
  fi
done
for name in "scenario ctx" "scheduler.wait" "queueMicrotask" "ctx_end"; do
  if grep -q -a -F "$name" "$OFF"; then
    echo "present without workerd.async: $name" >&2
    STATUS=1
  fi
done
exit $STATUS
