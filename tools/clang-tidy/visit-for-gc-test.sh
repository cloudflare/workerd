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

expect_diag "field 'missed' of visitable type" "unvisited jsg::Ref field"
expect_diag "field 'name' of visitable type" "unvisited jsg::Name field"
expect_diag "ownership barrier 'kj::Rc'" "visit through kj::Rc operator->"
expect_diag "ownership barrier 'kj::Arc'" "visit through kj::Arc deref"
expect_diag "ownership barrier 'kj::Own'" "visit through kj::Own deref"

own_count=$(grep -c "ownership barrier 'kj::Own'" <<<"${positive_output}")
if [[ "${own_count}" -lt 3 ]]; then
  printf 'Expected kj::Own barrier diagnostics for *own, own.get(), and own->visitForGc(); got %s\n' \
    "${own_count}" >&2
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
