#!/usr/bin/env bash

# Copyright (c) 2017-2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

set -euo pipefail

readonly ROOT="${TEST_SRCDIR}/${TEST_WORKSPACE}"
readonly CLANG_TIDY="${ROOT}/tools/clang_tidy"
readonly PLUGIN="${ROOT}/tools/clang-tidy/libworkerd-lint.so"
readonly POSITIVE="${ROOT}/tools/clang-tidy/visit-for-gc-positive-test.c++"
readonly NEGATIVE="${ROOT}/tools/clang-tidy/visit-for-gc-negative-test.c++"
readonly CHECKS="-*,jsg-visit-for-gc"

set +e
positive_output=$("${CLANG_TIDY}" "--load=${PLUGIN}" --checks="${CHECKS}" \
  --warnings-as-errors='*' "${POSITIVE}" -- -std=c++23 2>&1)
positive_status=$?
set -e

if [[ ${positive_status} -eq 0 ]]; then
  printf '%s\n' "Expected jsg-visit-for-gc to reject the positive fixtures." >&2
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

expect_diag "field 'missed' of visitable type" "P1: unvisited jsg::Ref field"
expect_diag \
  "field 'orphaned' of visitable type 'jsg::Ref<Widget>' is not visited in visitForGc (class has no visitForGc method)" \
  "P2: resource with no visitForGc"
expect_diag "field 'maybeRef' of visitable type" "P4: unvisited kj::Maybe<jsg::Ref>"
expect_diag "field 'stateful' of visitable type" "P5: unvisited kj::OneOf alternative"
expect_diag "field 'resolver' of visitable type" "P6: unvisited Promise<T>::Resolver"
expect_diag "field 'maybeResolver' of visitable type" "P7: unvisited Maybe<Resolver>"
expect_diag "field 'gen' of visitable type" "P8: unvisited jsg::Generator"
expect_diag "field 'gen' of visitable type 'jsg::AsyncGenerator<int>'" \
  "P9: unvisited jsg::AsyncGenerator"
expect_diag "field 'seq' of visitable type" "P10: unvisited jsg::Sequence of visitable elements"

# Exact-count lock: one diagnostic per positive case, no more, no fewer.
expected_count=9
actual_count=$(grep -c "\[jsg-visit-for-gc" <<<"${positive_output}" || true)
if [[ "${actual_count}" -ne "${expected_count}" ]]; then
  printf 'Expected exactly %s jsg-visit-for-gc diagnostics, got %s\n' \
    "${expected_count}" "${actual_count}" >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

negative_output=$("${CLANG_TIDY}" "--load=${PLUGIN}" --checks="${CHECKS}" \
  --warnings-as-errors='*' "${NEGATIVE}" -- -std=c++23 2>&1)

if [[ "${negative_output}" == *"jsg-visit-for-gc"* ]]; then
  printf '%s\n' "Expected the negative fixtures to be accepted." >&2
  printf '%s\n' "${negative_output}" >&2
  exit 1
fi
