#!/usr/bin/env bash

# Copyright (c) 2017-2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

# Tests the `custom-iocontext-run-manual-capture` query-based clang-tidy check.
# Unlike the plugin checks (e.g. workerd-consume), this check is defined entirely
# in the workerd `.clang-tidy` config as a CustomChecks entry, so the test drives
# clang-tidy against the real merged config with --experimental-custom-checks.

set -euo pipefail

readonly ROOT="${TEST_SRCDIR}/${TEST_WORKSPACE}"
readonly CLANG_TIDY="${ROOT}/tools/clang_tidy"
readonly CONFIG="${ROOT}/merged.clang-tidy"
readonly POSITIVE="${ROOT}/tools/clang-tidy/iocontext-run-manual-capture-positive-test.c++"
readonly NEGATIVE="${ROOT}/tools/clang-tidy/iocontext-run-manual-capture-negative-test.c++"
readonly CHECK="custom-iocontext-run-manual-capture"
readonly CHECKS="-*,${CHECK}"
readonly MESSAGE="lambda captures the IoContext manually"

run_check() {
  local file="$1"
  "${CLANG_TIDY}" \
    --experimental-custom-checks \
    --config-file="${CONFIG}" \
    --checks="${CHECKS}" \
    --warnings-as-errors='*' \
    "${file}" -- -xc++ -std=c++23 2>&1
}

# Positive cases: the check must fire (non-zero exit) and emit its diagnostic.
set +e
positive_output=$(run_check "${POSITIVE}")
positive_status=$?
set -e

if [[ ${positive_status} -eq 0 ]]; then
  printf '%s\n' "Expected ${CHECK} to flag manual IoContext captures." >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

if [[ "${positive_output}" != *"${MESSAGE}"* ]]; then
  printf '%s\n' "Expected ${CHECK} diagnostic message." >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

# Each of the four positive cases should be flagged.
positive_count=$(printf '%s\n' "${positive_output}" | grep -c "${CHECK}")
if [[ ${positive_count} -ne 4 ]]; then
  printf '%s\n' "Expected 4 ${CHECK} diagnostics, got ${positive_count}." >&2
  printf '%s\n' "${positive_output}" >&2
  exit 1
fi

# Negative cases: the check must not fire on any of them.
negative_output=$(run_check "${NEGATIVE}")

if [[ "${negative_output}" == *"${CHECK}"* ]]; then
  printf '%s\n' "Expected ${CHECK} to accept two-arg form and unrelated captures." >&2
  printf '%s\n' "${negative_output}" >&2
  exit 1
fi
