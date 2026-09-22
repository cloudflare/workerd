#!/usr/bin/env bash

# Copyright (c) 2017-2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

set -euo pipefail

readonly ROOT="${TEST_SRCDIR}/${TEST_WORKSPACE}"
readonly CLANG_TIDY="${ROOT}/tools/clang_tidy"
readonly PLUGIN="${ROOT}/tools/clang-tidy/libworkerd-lint.so"
readonly POSITIVE="${ROOT}/tools/clang-tidy/legacy-stream-alloc-positive-test.c++"
readonly NEGATIVE="${ROOT}/tools/clang-tidy/legacy-stream-alloc-negative-test.c++"
readonly CHECK="workerd-legacy-stream-alloc"
readonly CHECKS="-*,${CHECK}"

readonly READABLE_MESSAGE="direct allocation of legacy ReadableStream; use JsReadableStream::create()"
readonly WRITABLE_MESSAGE="direct allocation of legacy WritableStream; use JsWritableStream::create()"

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

# Asserts that a diagnostic with the given message is reported on the given
# fixture line, identifying the case that produced it.
expect_diag_at() {
  local line="$1"
  local message="$2"
  local description="$3"
  if ! grep -qF "legacy-stream-alloc-positive-test.c++:${line}:" <<<"${positive_output}"; then
    printf 'Missing expected diagnostic (%s) on line %s\n' "${description}" "${line}" >&2
    printf '%s\n' "${positive_output}" >&2
    exit 1
  fi
  if ! grep -F "legacy-stream-alloc-positive-test.c++:${line}:" <<<"${positive_output}" |
    grep -qF "${message}"; then
    printf 'Diagnostic on line %s (%s) has the wrong message; expected: %s\n' \
      "${line}" "${description}" "${message}" >&2
    printf '%s\n' "${positive_output}" >&2
    exit 1
  fi
}

# Resolves the line of a fixture case from a unique snippet of its source, so
# the assertions below do not depend on hard-coded line numbers.
line_of() {
  local needle="$1"
  local line
  line=$(grep -nF -- "${needle}" "${POSITIVE}" | head -n1 | cut -d: -f1)
  if [[ -z "${line}" ]]; then
    printf 'Fixture snippet not found: %s\n' "${needle}" >&2
    exit 1
  fi
  printf '%s' "${line}"
}

expect_diag_at "$(line_of '  return js.alloc<ReadableStream>(source);')" \
  "${READABLE_MESSAGE}" "P1: js.alloc<ReadableStream>"
expect_diag_at "$(line_of '  return js.alloc<WritableStream>(sink);')" \
  "${WRITABLE_MESSAGE}" "P2: js.alloc<WritableStream>"
expect_diag_at "$(line_of 'return js.allocAccounted<ReadableStream>(sizeof(ReadableStream), source);')" \
  "${READABLE_MESSAGE}" "P3: js.allocAccounted<ReadableStream>"
expect_diag_at "$(line_of 'return jsg::alloc<WritableStream>(sink);')" \
  "${WRITABLE_MESSAGE}" "P4: free-function jsg::alloc<WritableStream>"
expect_diag_at "$(line_of 'return js.alloc<::workerd::api::ReadableStream>(source);')" \
  "${READABLE_MESSAGE}" "P5: namespace-qualified type"
expect_diag_at "$(line_of 'auto make = [&]() { return js.alloc<ReadableStream>(source); };')" \
  "${READABLE_MESSAGE}" "P6: lambda in a non-dispatch function"
expect_diag_at "$(line_of 'return js.alloc<ReadableStream>(otherSource);')" \
  "${READABLE_MESSAGE}" "P7: JsReadableStream member other than create()"
expect_diag_at "$(line_of 'return js.alloc<WritableStream>(unrelatedSink);')" \
  "${WRITABLE_MESSAGE}" "P8: create() on an unrelated class"
expect_diag_at "$(line_of 'return js.alloc<T>(arg);')" \
  "${READABLE_MESSAGE}" "P9: generic helper instantiated with ReadableStream"

# Exact-count lock: one diagnostic per positive case, no more, no fewer.
expected_count=9
actual_count=$(grep -c "\[${CHECK}" <<<"${positive_output}" || true)
if [[ "${actual_count}" -ne "${expected_count}" ]]; then
  printf 'Expected exactly %s %s diagnostics, got %s\n' \
    "${expected_count}" "${CHECK}" "${actual_count}" >&2
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
