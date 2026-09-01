#!/usr/bin/env bash

# Copyright (c) 2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

set -euo pipefail

readonly ROOT="${TEST_SRCDIR}/${TEST_WORKSPACE}"

stdlib=$(find "$ROOT/build/rust/tsan_rust_std_build" \
  -name libstd_std.rlib -print -quit)
if [[ -z "$stdlib" ]] || ! nm -A "$stdlib" 2>/dev/null | grep '__tsan_func_entry' >/dev/null; then
  echo "Rust standard library does not contain TSan instrumentation" >&2
  exit 1
fi

race="$ROOT/src/rust/tsan-test/race"
set +e
output=$("$race" 2>&1)
status=$?
set -e
if [[ $status -eq 0 ]]; then
  echo "racy Rust binary unexpectedly succeeded" >&2
  exit 1
fi
if ! grep -q 'ThreadSanitizer: data race' <<<"$output"; then
  echo "racy Rust binary failed without a ThreadSanitizer report:" >&2
  echo "$output" >&2
  exit 1
fi
