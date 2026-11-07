#!/usr/bin/env bash

# Copyright (c) 2017-2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

set -euo pipefail

readonly ROOT="${TEST_SRCDIR}/${TEST_WORKSPACE}"
readonly CLANG_TIDY="${ROOT}/tools/clang_tidy"
readonly PLUGIN="${ROOT}/tools/clang-tidy/libworkerd-lint.so"
readonly POSITIVE="${ROOT}/tools/clang-tidy/js-reentry-positive-test.c++"
readonly NEGATIVE="${ROOT}/tools/clang-tidy/js-reentry-negative-test.c++"
readonly CHECKS="-*,jsg-js-reentry"

set +e
positive_output=$("${CLANG_TIDY}" "--load=${PLUGIN}" --checks="${CHECKS}" \
  --warnings-as-errors='*' "${POSITIVE}" -- -std=c++23 2>&1)
positive_status=$?
set -e

if [[ ${positive_status} -eq 0 ]]; then
  printf '%s\n' "Expected jsg-js-reentry to reject the positive fixtures." >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

expect_diag() {
  local needle="$1"
  local description="$2"
  if [[ "${positive_output}" != *"${needle}"* ]]; then
    printf 'Missing expected diagnostic (%s): %s\n' "${description}" "${needle}" >&2
    printf '%s\n' "${positive_output}" >&2
    exit 1
  fi
}

readonly CALL_ON="borrows JSG-owned state, and this call on it can run JavaScript"
readonly USED_AFTER="borrows JSG-owned state and is used after a call that can run JavaScript"

expect_diag "'signal' ${CALL_ON}" "P1: AbortSignal::any follower aborted through a reference"
expect_diag "'attached' ${CALL_ON}" "P2: pull() called through a KJ_IF_SOME borrow"
expect_diag "this loop iterates JSG-owned state across a call that can run JavaScript" \
  "P3: callbacks run while iterating the subscriber list"
expect_diag "'sub' ${CALL_ON}" "P3: callback called through the element reference"
expect_diag "'req' ${USED_AFTER}" "P4: pending read request used after push()"
expect_diag "'entry' ${USED_AFTER}" "P5: module entry used after Evaluate()"
expect_diag "'line' ${USED_AFTER}" "P6: view into a buffer used after a JS call"

# Exact-count lock: one diagnostic per expectation above, no more, no fewer.
expected_count=7
actual_count=$(grep -c "\[jsg-js-reentry" <<<"${positive_output}" || true)
if [[ "${actual_count}" -ne "${expected_count}" ]]; then
  printf 'Expected exactly %s jsg-js-reentry diagnostics, got %s\n' \
    "${expected_count}" "${actual_count}" >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

negative_output=$("${CLANG_TIDY}" "--load=${PLUGIN}" --checks="${CHECKS}" \
  --warnings-as-errors='*' "${NEGATIVE}" -- -std=c++23 2>&1)

if [[ "${negative_output}" == *"jsg-js-reentry"* ]]; then
  printf '%s\n' "Expected the negative fixtures to be accepted." >&2
  printf '%s\n' "${negative_output}" >&2
  exit 1
fi
