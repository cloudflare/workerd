#!/usr/bin/env bash

# Copyright (c) 2017-2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

set -euo pipefail

readonly ROOT="${TEST_SRCDIR}/${TEST_WORKSPACE}"
readonly CLANG_TIDY="${ROOT}/tools/clang_tidy"
readonly PLUGIN="${ROOT}/tools/clang-tidy/libworkerd-lint.so"
readonly POSITIVE="${ROOT}/tools/clang-tidy/shared-visit-positive-test.c++"
readonly NEGATIVE="${ROOT}/tools/clang-tidy/shared-visit-negative-test.c++"
readonly CHECK="jsg-shared-visit"
readonly CHECKS="-*,${CHECK}"

readonly MESSAGE="visitForGc visits through a shared owner"

set +e
positive_output=$("${CLANG_TIDY}" "--load=${PLUGIN}" --checks="${CHECKS}" \
  --warnings-as-errors='*' "${POSITIVE}" -- -std=c++23 2>&1)
positive_status=$?
set -e

if [[ ${positive_status} -eq 0 ]]; then
  printf '%s\n' "Expected ${CHECK} to reject the positive fixtures." >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

# Asserts that a diagnostic with the given message is reported on the line of
# the given fixture snippet.
expect_diag() {
  local needle="$1"
  local message="$2"
  local description="$3"
  local line
  line=$(grep -nF -- "${needle}" "${POSITIVE}" | head -n1 | cut -d: -f1)
  if [[ -z "${line}" ]]; then
    printf 'Fixture snippet not found: %s\n' "${needle}" >&2
    exit 1
  fi
  if ! grep -F "shared-visit-positive-test.c++:${line}:" <<<"${positive_output}" |
    grep -qF "${message}"; then
    printf 'Missing expected diagnostic (%s) on line %s: %s\n' "${description}" "${line}" \
      "${message}" >&2
    printf '%s\n' "${positive_output}" >&2
    exit 1
  fi
}

expect_diag ";  // P1" "${MESSAGE}" "P1: visit(*rc)"
expect_diag ";  // P2" "${MESSAGE}" "P2: visit(rc->field)"
expect_diag ";  // P3" "${MESSAGE}" "P3: rc->visitForGc()"
expect_diag ";  // P4" "${MESSAGE}" "P4: visit through a kj::Own of a refcounted type"
expect_diag ";  // P5" "${MESSAGE}" "P5: visiting kj::Rc elements in a helper"
expect_diag ";  // P6" "${MESSAGE}" "P6: visit(*arc)"
expect_diag ";  // P7" "${MESSAGE}" "P7: visit through a kj::Own of an atomically refcounted type"
expect_diag ";  // P8" "${MESSAGE}" "P8: visitAll through a kj::Rc"

# Exact-count lock: one diagnostic per positive case, no more, no fewer.
expected_count=8
actual_count=$(grep -c "\[${CHECK}" <<<"${positive_output}" || true)
if [[ "${actual_count}" -ne "${expected_count}" ]]; then
  printf 'Expected exactly %s %s diagnostics, got %s\n' "${expected_count}" "${CHECK}" \
    "${actual_count}" >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

negative_output=$("${CLANG_TIDY}" "--load=${PLUGIN}" --checks="${CHECKS}" \
  --warnings-as-errors='*' "${NEGATIVE}" -- -std=c++23 2>&1)

if [[ "${negative_output}" == *"${CHECK}"* ]]; then
  printf '%s\n' "Expected the negative fixtures to be accepted." >&2
  printf '%s\n' "${negative_output}" >&2
  exit 1
fi
