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

if [[ $(printf '%s\n' "${positive_output}" | grep -c '\[workerd-use-after-move') -ne 9 ]]; then
  printf '%s\n' "Expected nine workerd-use-after-move diagnostics." >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

for line in 36 40 45 50 56 64 80 86; do
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

# Use the actual dependency macros, including their switch labels and implicit breaks.
readonly ONEOF="${ROOT}/tools/clang-tidy/use-after-move-oneof-test.c++"
kj_headers=("${TEST_SRCDIR}"/*/src/kj/_virtual_includes/kj/kj/one-of.h)
[[ ${#kj_headers[@]} -eq 1 && -f "${kj_headers[0]}" ]]
resource_dir=$(clang -print-resource-dir)
if ! oneof_output=$("${CLANG_TIDY}" "--load=${PLUGIN}" --checks="${CHECKS}" --config="${CONFIG}" \
  "${ONEOF}" -- -std=c++23 -resource-dir="${resource_dir}" \
  -I"${kj_headers[0]%/kj/one-of.h}" 2>&1); then
  printf '%s\n' "${oneof_output}" >&2
  exit 1
fi
expected_lines=$(awk '/\/\/ expect-(loop-)?warning/ { print NR }' "${ONEOF}")
[[ -n "${expected_lines}" ]]
actual_lines=$(printf '%s\n' "${oneof_output}" | \
  sed -n 's/.*use-after-move-oneof-test.c++:\([0-9]*\):[0-9]*: warning:.*\[workerd-use-after-move\].*/\1/p')
if [[ "${actual_lines}" != "${expected_lines}" ]]; then
  printf '%s\n' "Unexpected KJ_CASE_ONEOF diagnostics." "${oneof_output}" >&2
  diff -u <(printf '%s\n' "${expected_lines}") <(printf '%s\n' "${actual_lines}") >&2
  exit 1
fi
expected_loops=$(awk '/\/\/ expect-loop-warning/ { print NR }' "${ONEOF}")
actual_loops=$(printf '%s\n' "${oneof_output}" | \
  sed -n 's/.*use-after-move-oneof-test.c++:\([0-9]*\):[0-9]*: note: the use happens in a later loop iteration.*/\1/p')
if [[ "${actual_loops}" != "${expected_loops}" ]]; then
  printf '%s\n' "Unexpected later-iteration notes." "${oneof_output}" >&2
  diff -u <(printf '%s\n' "${expected_loops}") <(printf '%s\n' "${actual_loops}") >&2
  exit 1
fi
