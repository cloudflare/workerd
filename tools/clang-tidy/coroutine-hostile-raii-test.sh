#!/usr/bin/env bash

# Copyright (c) 2017-2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

set -euo pipefail

readonly ROOT="${TEST_SRCDIR}/${TEST_WORKSPACE}"
readonly CLANG_TIDY="${ROOT}/tools/clang_tidy"
readonly PLUGIN="${ROOT}/tools/clang-tidy/libworkerd-lint.so"
readonly POSITIVE="${ROOT}/tools/clang-tidy/coroutine-hostile-raii-positive-test.c++"
readonly NEGATIVE="${ROOT}/tools/clang-tidy/coroutine-hostile-raii-negative-test.c++"
readonly CHECKS="-*,workerd-coroutine-hostile-raii"
readonly CONFIG="{CheckOptions: [{key: workerd-coroutine-hostile-raii.RAIITypesList, value: 'kj::UnwindDetector'}, {key: workerd-coroutine-hostile-raii.AllowedAwaitablesList, value: 'AllowedYieldable'}]}"

set +e
positive_output=$("${CLANG_TIDY}" "--load=${PLUGIN}" --checks="${CHECKS}" \
  "--config=${CONFIG}" --warnings-as-errors='*' "${POSITIVE}" -- -std=c++20 2>&1)
positive_status=$?
set -e

if [[ ${positive_status} -eq 0 ]]; then
  printf '%s\n' "Expected a hostile RAII diagnostic for a declaration initializer." >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

if [[ "${positive_output}" != *"'detector' persists across a suspension point"* ]]; then
  printf '%s\n' "Expected the hostile RAII diagnostic for detector." >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

if [[ "${positive_output}" != *"'yieldDetector' persists across a suspension point"* ]]; then
  printf '%s\n' "Expected the hostile RAII diagnostic for a disallowed co_yield operand." >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

set +e
negative_output=$("${CLANG_TIDY}" "--load=${PLUGIN}" --checks="${CHECKS}" \
  "--config=${CONFIG}" --warnings-as-errors='*' "${NEGATIVE}" -- -std=c++20 2>&1)
negative_status=$?
set -e

if [[ ${negative_status} -ne 0 ]]; then
  printf '%s\n' "Expected negative coroutine cases to be accepted." >&2
  printf '%s\n' "${negative_output}" >&2
  exit 1
fi
