#!/usr/bin/env bash

# Copyright (c) 2017-2026 Cloudflare, Inc.
# Licensed under the Apache 2.0 license found in the LICENSE file or at:
#     https://opensource.org/licenses/Apache-2.0

set -euo pipefail

readonly ROOT="${TEST_SRCDIR}/${TEST_WORKSPACE}"
readonly CLANG_TIDY="${ROOT}/tools/clang_tidy"
readonly PLUGIN="${ROOT}/tools/clang-tidy/libworkerd-lint.so"
readonly POSITIVE="${ROOT}/tools/clang-tidy/ctor-initializer-this-capture-positive-test.c++"
readonly NEGATIVE="${ROOT}/tools/clang-tidy/ctor-initializer-this-capture-negative-test.c++"
readonly CHECK="workerd-ctor-initializer-this-capture"
readonly CHECKS="-*,${CHECK}"

readonly EXPLICIT_MESSAGE="captures 'this' before the object is fully constructed"
readonly IMPLICIT_MESSAGE="implicitly captures 'this' through its capture default"

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

# Asserts that a diagnostic with the given message is reported at the given
# fixture line and column, identifying the case that produced it.
expect_diag_at() {
  local location="$1"
  local message="$2"
  local description="$3"
  if ! grep -F "ctor-initializer-this-capture-positive-test.c++:${location}:" \
    <<<"${positive_output}" | grep -qF "${message}"; then
    printf 'Missing expected diagnostic (%s) at %s: %s\n' \
      "${description}" "${location}" "${message}" >&2
    printf '%s\n' "${positive_output}" >&2
    exit 1
  fi
}

# Resolves the line:column of a needle within a unique snippet of a fixture
# case, so the assertions below do not depend on hard-coded line numbers.
location_of() {
  local snippet="$1"
  local needle="$2"
  local line
  line=$(grep -nF -- "${snippet}" "${POSITIVE}" | head -n1 | cut -d: -f1)
  if [[ -z "${line}" ]]; then
    printf 'Fixture snippet not found: %s\n' "${snippet}" >&2
    exit 1
  fi
  local text
  text=$(sed -n "${line}p" "${POSITIVE}")
  local prefix="${text%%"${needle}"*}"
  printf '%s:%s' "${line}" "$((${#prefix} + 1))"
}

expect_diag_at "$(location_of 'onEvent([this]() { handle(); }),' 'this')" \
  "'onEvent' ${EXPLICIT_MESSAGE}" "P1: explicit this"
expect_diag_at "$(location_of 'onEvent([*this]() mutable' '*this')" \
  "'onEvent' ${EXPLICIT_MESSAGE}" "P2: *this"
expect_diag_at "$(location_of 'onEvent([&]() { handle(); }),' '&')" \
  "'onEvent' ${IMPLICIT_MESSAGE}" "P3: implicit through [&]"
expect_diag_at "$(location_of 'onEvent([=]() { handle(); }),' '=')" \
  "'onEvent' ${IMPLICIT_MESSAGE}" "P4: implicit through [=]"
expect_diag_at "$(location_of ': Base([this]() { handle(); }),' 'this')" \
  "'Base' ${EXPLICIT_MESSAGE}" "P5: base initializer"
expect_diag_at "$(location_of 'Widget(wrap([this]() { handle(); }))' 'this')" \
  "'Widget' ${EXPLICIT_MESSAGE}" "P6: delegating initializer"
expect_diag_at "$(location_of 'onEvent([self = this]()' 'self')" \
  "'onEvent' ${EXPLICIT_MESSAGE}" "P7: init-capture of this"
expect_diag_at "$(location_of 'onEvent([&self = *this]()' 'self')" \
  "'onEvent' ${EXPLICIT_MESSAGE}" "P8: init-capture of *this"
expect_diag_at "$(location_of 'task(start().then([this]()' 'this')" \
  "'task' ${EXPLICIT_MESSAGE}" "P9: continuation"
expect_diag_at "$(location_of 'return wrap([this]() { handle(); }); }()),' 'this')" \
  "'onEvent' ${EXPLICIT_MESSAGE}" "P10: escapes from an immediately invoked lambda"
expect_diag_at "$(location_of 'onEvent([inner = wrap([this]()' 'this')" \
  "'onEvent' ${EXPLICIT_MESSAGE}" "P11: inside an init-capture"
expect_diag_at "$(location_of 'onEvent([&, this]()' 'this')" \
  "'onEvent' ${EXPLICIT_MESSAGE}" "P12: [&, this]"
expect_diag_at "$(location_of 'OutOfLine::OutOfLine(): onEvent([this]()' 'this')" \
  "'onEvent' ${EXPLICIT_MESSAGE}" "P13: out-of-line constructor"
expect_diag_at "$(location_of 'Holder(): onEvent([this]()' 'this')" \
  "'onEvent' ${EXPLICIT_MESSAGE}" "P14: class template"
expect_diag_at "$(location_of 'DependentHolder(): onEvent([&]()' '&')" \
  "'onEvent' ${IMPLICIT_MESSAGE}" "P15: dependent capture default"

# Exact-count lock: one diagnostic per positive case, no more, no fewer.
expected_count=15
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
