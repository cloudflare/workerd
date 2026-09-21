#!/usr/bin/env bash

# Copyright (c) 2017-2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

set -euo pipefail

readonly ROOT="${TEST_SRCDIR}/${TEST_WORKSPACE}"
readonly CLANG_TIDY="${ROOT}/tools/clang_tidy"
readonly PLUGIN="${ROOT}/tools/clang-tidy/libworkerd-lint.so"
readonly POSITIVE="${ROOT}/tools/clang-tidy/use-after-move-positive-test.c++"
readonly NEGATIVE="${ROOT}/tools/clang-tidy/use-after-move-negative-test.c++"
readonly CHECKS="-*,workerd-use-after-move"
readonly CONFIG='{CheckOptions: [{key: workerd-use-after-move.InvalidationFunctions, value: "::kj::mv"}]}'

set +e
positive_output=$("${CLANG_TIDY}" "--load=${PLUGIN}" --checks="${CHECKS}" --config="${CONFIG}" \
  --warnings-as-errors='*' "${POSITIVE}" -- -std=c++23 2>&1)
positive_status=$?
set -e

if [[ ${positive_status} -eq 0 ]]; then
  printf '%s\n' "Expected workerd-use-after-move diagnostics." >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

if [[ $(printf '%s\n' "${positive_output}" | grep -c '\[workerd-use-after-move') -ne 5 ]]; then
  printf '%s\n' "Expected five workerd-use-after-move diagnostics." >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

for line in 28 32 37 42; do
  if [[ "${positive_output}" != *"use-after-move-positive-test.c++:${line}:"* ]]; then
    printf '%s\n' "Expected a workerd-use-after-move diagnostic on line ${line}." >&2
    printf '%s\n' "${positive_output}" >&2
    exit 1
  fi
done

set +e
negative_output=$("${CLANG_TIDY}" "--load=${PLUGIN}" --checks="${CHECKS}" --config="${CONFIG}" \
  "${NEGATIVE}" -- -std=c++23 2>&1)
negative_status=$?
set -e

if [[ ${negative_status} -ne 0 ]]; then
  printf '%s\n' "Expected negative fixture to compile." >&2
  printf '%s\n' "${negative_output}" >&2
  exit 1
fi

if [[ "${negative_output}" == *"workerd-use-after-move"* ]]; then
  printf '%s\n' "Expected path-sensitive cases to be accepted." >&2
  printf '%s\n' "${negative_output}" >&2
  exit 1
fi
