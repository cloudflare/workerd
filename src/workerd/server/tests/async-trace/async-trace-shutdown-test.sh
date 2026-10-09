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

# Without KJ_CLEAN_SHUTDOWN, workerd exits without destroying the server, so the actor's context
# never ends: the exit line lists it as open.
DEFAULT=$TEST_TMPDIR/default.ndjson
env -u KJ_CLEAN_SHUTDOWN "$WORKERD_BINARY" test "$CONFIG" --async-trace="$DEFAULT"
ACTOR=$(actor_ctx "$DEFAULT")
[[ -n $ACTOR ]] || fail "default: no actor context"
[[ $(tail -n 1 "$DEFAULT") == '{"e":"exit",'*"\"open\":[$ACTOR]}" ]] ||
  fail "default: the last line is not an exit listing context $ACTOR: $(tail -n 1 "$DEFAULT")"
if grep -q "\"e\":\"ctx_end\",\"ctx\":$ACTOR," "$DEFAULT"; then
  fail "default: the open context $ACTOR has a ctx_end"
fi

# With KJ_CLEAN_SHUTDOWN, destroying the server ends the actor's context, and the exit line comes
# after that end.
CLEAN=$TEST_TMPDIR/clean.ndjson
KJ_CLEAN_SHUTDOWN=1 "$WORKERD_BINARY" test "$CONFIG" --async-trace="$CLEAN"
[[ $(tail -n 1 "$CLEAN") == '{"e":"exit",'*'"open":[]}' ]] ||
  fail "clean: the last line is not an exit with no open contexts: $(tail -n 1 "$CLEAN")"
CLEAN_ACTOR=$(actor_ctx "$CLEAN")
[[ -n $CLEAN_ACTOR ]] || fail "clean: no actor context"
CLEAN_IDS=$(context_ids "$CLEAN")
[[ $(wc -l <<<"$CLEAN_IDS") -eq 2 ]] || fail "clean: expected 2 contexts: $CLEAN_IDS"
# Every context, the actor's included, ends exactly once (and before the exit line, which is last).
for ctx in $CLEAN_IDS; do
  [[ $(ends_of "$CLEAN" "$ctx") -eq 1 ]] ||
    fail "clean: context $ctx has $(ends_of "$CLEAN" "$ctx") ctx_end lines, not 1"
done
[[ $(grep -c '"e":"ctx_end",' "$CLEAN") -eq 2 ]] ||
  fail "clean: expected 2 ctx_end lines in all, got $(grep -c '"e":"ctx_end",' "$CLEAN")"
[[ $(grep -c '"e":"exit",' "$CLEAN") -eq 1 ]] || fail "clean: not exactly one exit line"

exit $STATUS
