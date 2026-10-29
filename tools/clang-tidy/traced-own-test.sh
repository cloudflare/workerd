#!/usr/bin/env bash

# Copyright (c) 2017-2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

set -euo pipefail

readonly ROOT="${TEST_SRCDIR}/${TEST_WORKSPACE}"
readonly CLANG_TIDY="${ROOT}/tools/clang_tidy"
readonly PLUGIN="${ROOT}/tools/clang-tidy/libworkerd-lint.so"
readonly POSITIVE="${ROOT}/tools/clang-tidy/traced-own-positive-test.c++"
readonly NEGATIVE="${ROOT}/tools/clang-tidy/traced-own-negative-test.c++"
readonly CHECK="jsg-traced-own"
readonly CHECKS="-*,${CHECK}"

readonly VISIT_MESSAGE="visitForGc visits through a kj::Own"
readonly MOVE_MESSAGE="moving a container of jsg::TracedOwn moves none of its elements"

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
  if ! grep -F "traced-own-positive-test.c++:${line}:" <<<"${positive_output}" |
    grep -qF "${message}"; then
    printf 'Missing expected diagnostic (%s) on line %s: %s\n' "${description}" "${line}" \
      "${message}" >&2
    printf '%s\n' "${positive_output}" >&2
    exit 1
  fi
}

expect_diag "visitor.visit(*request);" "${VISIT_MESSAGE}" "P1: visit(*own)"
expect_diag "visitor.visit(request->resolver);" "${VISIT_MESSAGE}" "P2: visit(own->field)"
expect_diag "      visitor.visit(request->resolver);" "${VISIT_MESSAGE}" \
  "P3: visiting elements of a container of Owns"
expect_diag "request->visitForGc(visitor);" "${VISIT_MESSAGE}" "P4: own->visitForGc()"
expect_diag "return kj::mv(requests);" "${MOVE_MESSAGE}" "P5: moving a kj::Vector field"
expect_diag "auto all = kj::mv(ready.requests);" "${MOVE_MESSAGE}" \
  "P6: moving a nested RingBuffer field"

expect_diag "visitor.visit(request->resolver);  // P7" "${VISIT_MESSAGE}" \
  "P7: visiting through an Own in a helper"
expect_diag "auto all = kj::mv(events);" "${MOVE_MESSAGE}" \
  "P8: moving a RingBuffer of structs holding a TracedOwn"

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
