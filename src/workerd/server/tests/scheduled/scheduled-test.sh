#!/bin/bash

set -euo pipefail

# $1 -> workerd binary path
# $2 -> config that sends the scheduled events
WORKERD_BINARY=$1
CONFIG=$2

# Runs the config with and without the autogates, among them RUST_ENTRYPOINT_SCHEDULED, which
# delivers the event from Rust, and checks that both deliver it the same way. That the gate takes
# effect is checked by scheduled-perfetto-test.
for gates in off on; do
  log="$TEST_TMPDIR/log-$gates"
  trace="$TEST_TMPDIR/trace-$gates.ndjson"
  # Setting WORKERD_ALL_AUTOGATES to anything, even 0, enables them.
  if [[ "$gates" == on ]]; then
    export WORKERD_ALL_AUTOGATES=1
  else
    unset WORKERD_ALL_AUTOGATES
  fi
  "$WORKERD_BINARY" test "$CONFIG" --experimental --verbose --async-trace="$trace" >"$log" 2>&1 || {
    cat "$log"
    exit 1
  }

  # Each event's request is named after the event in the async trace.
  requests=$(grep -c '"kind":"request","name":"scheduled"' "$trace" || true)
  if [[ "$requests" != 2 ]]; then
    echo "autogates $gates: $requests scheduled requests in the async trace, not 2"
    grep '"kind":"request"' "$trace" || true
    exit 1
  fi

  # The KJ exception for the missing handler, thrown by startScheduled(), fails the run unchanged,
  # so that the run's waitUntil task logs it as it was thrown. It is not logged as an exception
  # thrown by JavaScript, as it would be had it been converted to one.
  kj=$(grep -cE 'global-scope\.c\+\+:[0-9]+: failed: jsg\.Error: Handler does not export' "$log" \
    || true)
  thrown=$(grep -c 'source = Uncaught; stack = Error: Handler does not export' "$log" || true)
  if [[ "$kj" != 1 || "$thrown" != 0 ]]; then
    echo "autogates $gates: the KJ exception logged $kj times," \
      "as thrown by JavaScript $thrown times"
    cat "$log"
    exit 1
  fi
done
