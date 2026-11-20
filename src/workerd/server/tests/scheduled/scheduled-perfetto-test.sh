#!/bin/bash

set -euo pipefail

# $1 -> workerd binary path
# $2 -> config that sends the scheduled events
WORKERD_BINARY=$1
CONFIG=$2

# Runs the config with and without the autogates, among them RUST_ENTRYPOINT_SCHEDULED, and
# checks the Perfetto spans of each run: both have the spans of WorkerEntrypoint::runScheduled(),
# and only the run with the autogates has the span of the Rust path, which shows that the gate took
# effect. Perfetto interns names, so each appears in the file as plain bytes. Decoding the
# structure needs trace_processor, which isn't available here.
STATUS=0
for gates in off on; do
  trace="$TEST_TMPDIR/$gates.pftrace"
  # Setting WORKERD_ALL_AUTOGATES to anything, even 0, enables them.
  if [[ "$gates" == on ]]; then
    export WORKERD_ALL_AUTOGATES=1
  else
    unset WORKERD_ALL_AUTOGATES
  fi
  "$WORKERD_BINARY" test "$CONFIG" --experimental "--perfetto-trace=$trace=workerd"

  for name in "WorkerEntrypoint::runScheduled()" "WorkerEntrypoint::runScheduled() run" \
    "WorkerEntrypoint::runScheduled() waitForFinished()"; do
    if ! grep -q -a -F "$name" "$trace"; then
      echo "autogates $gates: missing span: $name" >&2
      STATUS=1
    fi
  done

  rust=0
  grep -q -a -F "entrypoint::runScheduled()" "$trace" && rust=1
  if [[ "$gates" == on && "$rust" == 0 ]]; then
    echo "autogates on: the event was not delivered from Rust" >&2
    STATUS=1
  elif [[ "$gates" == off && "$rust" == 1 ]]; then
    echo "autogates off: the event was delivered from Rust" >&2
    STATUS=1
  fi
done
exit $STATUS
