#!/bin/bash

# `workerd check`: its exit status, and the errors, warnings and summary it prints.
#
# $1 -> workerd binary path
# $2 -> path to ok.capnp, beside the test's other configs

set -euo pipefail

WORKERD_BINARY=$1
CONFIGS=$(dirname "$2")
OUTPUT=$TEST_TMPDIR/output
FAILED=0

# Runs `workerd check` on the given arguments, expecting the exit status $1, and leaves its
# output in $OUTPUT.
check() {
  local expected=$1
  shift
  local status=0
  "$WORKERD_BINARY" check "$@" > "$OUTPUT" 2>&1 || status=$?
  if [[ $status != "$expected" ]]; then
    echo "FAIL: \`workerd check $*\` exited with status $status, expected $expected:" >&2
    cat "$OUTPUT" >&2
    FAILED=1
  fi
}

# Expects $OUTPUT to contain the line $1.
expect_line() {
  if ! grep -qxF -- "$1" "$OUTPUT"; then
    echo "FAIL: expected the line: $1" >&2
    echo "in the output:" >&2
    cat "$OUTPUT" >&2
    FAILED=1
  fi
}

check 0 "$CONFIGS/ok.capnp"
expect_line 'Socket "admin" has no address in the config, so `workerd serve` needs one on the command line with `--socket-addr` or `--socket-fd`.'
expect_line "Config OK: 2 services, 2 sockets."

# Every error is reported, not only the first: a Worker whose top-level code throws, and two
# misspelled service names, each with a hint.
check 1 "$CONFIGS/errors.capnp"
expect_line "service broken: Uncaught Error: failed at startup"
expect_line "Worker \"main\"'s binding \"OTHER\" refers to a service \"mian\", but no such service is defined. Did you mean \"main\"?"
expect_line 'Socket "http" refers to a service "mainn", but no such service is defined. Did you mean "main"?'
expect_line "3 config errors found."

# Overrides are checked against the config, as `serve` checks them.
check 1 "$CONFIGS/ok.capnp" --directory-path=nope=/tmp
expect_line 'Config did not define any disk service named "nope" to match the override provided on the command line.'
expect_line "1 config error found."

check 0 "$CONFIGS/wrangler/wrangler.jsonc"
expect_line "Config OK: 1 service, 1 socket."

# Options for a running server are refused rather than ignored.
check 1 "$CONFIGS/ok.capnp" --watch
expect_line 'error: --watch: `check` does not serve, so has nothing to reload.'

exit $FAILED
