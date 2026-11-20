#!/bin/bash

set -euo pipefail

# $1 -> workerd binary path
# $2 -> config whose tests pass and fail
WORKERD_BINARY=$1
CONFIG=$2
log="$TEST_TMPDIR/log"
trace="$TEST_TMPDIR/trace.ndjson"

fail() {
  echo "$1"
  cat "$log"
  exit 1
}

if "$WORKERD_BINARY" test "$CONFIG" --verbose --async-trace="$trace" >"$log" 2>&1; then
  fail "workerd test passed, though some tests fail"
fi

# The verdict of each test, in the order the tests run.
verdicts=$(grep -oE '\[ (PASS|FAIL) \] [^ ]+' "$log" | tr '\n' ',')
expected='[ PASS ] modules:deletesTest,[ FAIL ] modules:failsAsynchronously,'
expected+='[ FAIL ] modules:noTest,[ PASS ] modules:passes,[ FAIL ] service-worker,'
if [[ "$verdicts" != "$expected" ]]; then
  fail "verdicts: $verdicts"
fi

# Each test's request is named after the event in the async trace.
requests=$(grep -c '"kind":"request","name":"test"' "$trace" || true)
if [[ "$requests" != 5 ]]; then
  fail "$requests test requests in the async trace, not 5"
fi

# What the asynchronous failure rejected the handler's promise with fails its waitUntil task.
if ! grep -q 'uncaught exception; exception = .*jsg\.Error: async test failure' "$log"; then
  fail "the asynchronous failure was not logged"
fi

# Without a test() to call, the run fails with a KJ exception for a JavaScript Error, which the
# waitUntil task logs as it was thrown. It is not logged as an exception thrown by JavaScript, as
# it would be had it been thrown into JavaScript.
for message in 'Entrypoint does not export a test\(\) function\.' \
  'Tests are not currently supported with Service Workers syntax\.'; do
  kj=$(grep -cE "uncaught exception; exception = .*failed: .*jsg\.Error: $message" "$log" || true)
  thrown=$(grep -cE "stack = Error: $message" "$log" || true)
  if [[ "$kj" != 1 || "$thrown" != 0 ]]; then
    fail "'$message': logged as a KJ exception $kj times, as thrown by JavaScript $thrown times"
  fi
done
