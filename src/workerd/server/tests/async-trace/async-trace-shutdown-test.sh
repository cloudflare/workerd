#!/bin/bash

set -euo pipefail

# $1 -> workerd binary path
# $2 -> config whose Durable Object context is still open at shutdown
WORKERD_BINARY=$1
CONFIG=$2

STATUS=0
fail() {
  echo "$1" >&2
  STATUS=1
}

# Each helper reads one trace file, $1.
context_ids() { grep '"e":"ctx",' "$1" | sed 's/.*"ctx":\([0-9]*\),.*/\1/'; }
ends_of() { grep -c "\"e\":\"ctx_end\",\"ctx\":$2," "$1" || true; }
actor_ctx() { grep '"e":"ctx",' "$1" | grep -v '"actor":null' | sed 's/.*"ctx":\([0-9]*\),.*/\1/'; }

# workerd destroys the worker factory before it exits, with or without KJ_CLEAN_SHUTDOWN. That ends
# the actor's context, and the exit line comes after that end. $1 names the run, $2 is its trace.
check() {
  local name=$1 trace=$2
  [[ $(tail -n 1 "$trace") == '{"e":"exit",'*'"open":[]}' ]] ||
    fail "$name: the last line is not an exit with no open contexts: $(tail -n 1 "$trace")"
  [[ -n $(actor_ctx "$trace") ]] || fail "$name: no actor context"
  local ids
  ids=$(context_ids "$trace")
  [[ $(wc -l <<<"$ids") -eq 2 ]] || fail "$name: expected 2 contexts: $ids"
  # Each context, the actor's included, ends exactly once (before the exit line, which is last).
  for ctx in $ids; do
    [[ $(ends_of "$trace" "$ctx") -eq 1 ]] ||
      fail "$name: context $ctx has $(ends_of "$trace" "$ctx") ctx_end lines, not 1"
  done
  [[ $(grep -c '"e":"ctx_end",' "$trace") -eq 2 ]] ||
    fail "$name: expected 2 ctx_end lines in all, got $(grep -c '"e":"ctx_end",' "$trace")"
  [[ $(grep -c '"e":"exit",' "$trace") -eq 1 ]] || fail "$name: not exactly one exit line"
}

DEFAULT=$TEST_TMPDIR/default.ndjson
env -u KJ_CLEAN_SHUTDOWN "$WORKERD_BINARY" test "$CONFIG" --async-trace="$DEFAULT"
check default "$DEFAULT"

CLEAN=$TEST_TMPDIR/clean.ndjson
KJ_CLEAN_SHUTDOWN=1 "$WORKERD_BINARY" test "$CONFIG" --async-trace="$CLEAN"
check clean "$CLEAN"

exit $STATUS
