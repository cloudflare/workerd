#!/usr/bin/env bash

# Copyright (c) 2017-2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

set -euo pipefail

readonly ROOT="${TEST_SRCDIR}/${TEST_WORKSPACE}"
readonly CLANG_TIDY="${ROOT}/tools/clang_tidy"
readonly CONFIG="${ROOT}/merged.clang-tidy"
readonly POSITIVE="${ROOT}/tools/clang-tidy/coroutine-hostile-raii-positive-test.c++"
readonly NEGATIVE="${ROOT}/tools/clang-tidy/coroutine-hostile-raii-negative-test.c++"
readonly CHECK="misc-coroutine-hostile-raii"
readonly CHECKS="-*,${CHECK}"
readonly MESSAGE="'detector' persists across a suspension point of coroutine"

run_check() {
  local file="$1"
  "${CLANG_TIDY}" \
    --config-file="${CONFIG}" \
    --checks="${CHECKS}" \
    --warnings-as-errors='*' \
    "${file}" -- -xc++ -std=c++23 2>&1
}

set +e
positive_output=$(run_check "${POSITIVE}")
positive_status=$?
set -e

if [[ ${positive_status} -eq 0 ]]; then
  printf '%s\n' "Expected ${CHECK} to flag UnwindDetector across co_await." >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

if [[ "${positive_output}" != *"${MESSAGE}"* ]]; then
  printf '%s\n' "Expected ${CHECK} diagnostic message." >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

positive_count=$(printf '%s\n' "${positive_output}" | grep -c "${CHECK}")
if [[ ${positive_count} -ne 2 ]]; then
  printf '%s\n' "Expected 2 ${CHECK} diagnostics, got ${positive_count}." >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

negative_output=$(run_check "${NEGATIVE}")
if [[ "${negative_output}" == *"${CHECK}"* ]]; then
  printf '%s\n' "Expected ${CHECK} to accept detectors outside await lifetimes." >&2
  printf '%s\n' "${negative_output}" >&2
  exit 1
fi
