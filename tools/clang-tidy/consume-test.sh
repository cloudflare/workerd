#!/usr/bin/env bash

# Copyright (c) 2017-2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

set -euo pipefail

readonly ROOT="${TEST_SRCDIR}/${TEST_WORKSPACE}"
readonly CLANG_TIDY="${ROOT}/tools/clang_tidy"
readonly PLUGIN="${ROOT}/tools/clang-tidy/libworkerd-lint.so"
readonly POSITIVE="${ROOT}/tools/clang-tidy/consume-positive-test.c++"
readonly NEGATIVE="${ROOT}/tools/clang-tidy/consume-negative-test.c++"
readonly CHECKS="-*,workerd-consume"

set +e
positive_output=$("${CLANG_TIDY}" "--load=${PLUGIN}" --checks="${CHECKS}" \
  --warnings-as-errors='*' "${POSITIVE}" -- -std=c++23 2>&1)
positive_status=$?
set -e

if [[ ${positive_status} -eq 0 ]]; then
  printf '%s\n' "Expected workerd-consume to reject direct kj::Ptr call." >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

if [[ "${positive_output}" != *"mustConsume may synchronously destroy its target"* ]]; then
  printf '%s\n' "Expected workerd-consume diagnostic for mustConsume()." >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

negative_output=$("${CLANG_TIDY}" "--load=${PLUGIN}" --checks="${CHECKS}" \
  --warnings-as-errors='*' "${NEGATIVE}" -- -std=c++23 2>&1)

if [[ "${negative_output}" == *"workerd-consume"* ]]; then
  printf '%s\n' "Expected consume() and unannotated calls to be accepted." >&2
  printf '%s\n' "${negative_output}" >&2
  exit 1
fi
