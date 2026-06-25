// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/global-scope.h>

#include <kj/test.h>

// Unit tests for alarmRetryCountsAgainstLimit(), the policy that decides whether a failed alarm's
// retry counts against the bounded retry limit (so the alarm is eventually abandoned) or retries
// forever as a transient infrastructure fault.

namespace workerd::api {
namespace {

KJ_TEST(
    "alarmRetryCountsAgainstLimit: a broken output gate alone does NOT count (retries forever)") {
  // The only case that does not count against the limit: the output gate broke for a reason we
  // cannot attribute to the worker (neither a user error nor a resource-limit breach).
  KJ_EXPECT(!alarmRetryCountsAgainstLimit({
    .outputGateBroken = true,
    .isUserError = false,
    .resourceLimitExceeded = false,
  }));
}

KJ_TEST("alarmRetryCountsAgainstLimit: a resource-limit breach that broke the gate counts "
        "(STOR-5337 regression)") {
  // Regression test for STOR-5337: when the output gate is broken by a non-user error AND a
  // resource limit was exceeded (e.g. the CPU limiter interrupting an in-flight SQLite query,
  // surfacing as a non-user "broken.outputGateBroken; ... SQLITE_INTERRUPT" error), the retry MUST
  // count against the limit so the alarm is eventually abandoned rather than retried forever.
  //
  // This is the precise combination the fix added: it differs from the case above only in
  // resourceLimitExceeded, and before the fix this combination returned false (retry forever).
  KJ_EXPECT(alarmRetryCountsAgainstLimit({
    .outputGateBroken = true,
    .isUserError = false,
    .resourceLimitExceeded = true,
  }));
}

KJ_TEST("alarmRetryCountsAgainstLimit: a user error that broke the gate counts") {
  KJ_EXPECT(alarmRetryCountsAgainstLimit({
    .outputGateBroken = true,
    .isUserError = true,
    .resourceLimitExceeded = false,
  }));
}

KJ_TEST("alarmRetryCountsAgainstLimit: failures without a broken output gate always count") {
  // When the output gate is intact, the failure is always counted regardless of the other facts
  // (the failure is the alarm handler's own exception, not an unattributable infra fault).
  for (bool isUserError: {false, true}) {
    for (bool resourceLimitExceeded: {false, true}) {
      KJ_EXPECT(alarmRetryCountsAgainstLimit({
        .outputGateBroken = false,
        .isUserError = isUserError,
        .resourceLimitExceeded = resourceLimitExceeded,
      }));
    }
  }
}

// Compile-time verification of the full truth table. The policy is
//   !outputGateBroken || isUserError || resourceLimitExceeded
// so the single false case is {outputGateBroken, !isUserError, !resourceLimitExceeded}.
static_assert(!alarmRetryCountsAgainstLimit(
    {.outputGateBroken = true, .isUserError = false, .resourceLimitExceeded = false}));
static_assert(alarmRetryCountsAgainstLimit(
    {.outputGateBroken = true, .isUserError = false, .resourceLimitExceeded = true}));
static_assert(alarmRetryCountsAgainstLimit(
    {.outputGateBroken = true, .isUserError = true, .resourceLimitExceeded = false}));
static_assert(alarmRetryCountsAgainstLimit(
    {.outputGateBroken = true, .isUserError = true, .resourceLimitExceeded = true}));
static_assert(alarmRetryCountsAgainstLimit(
    {.outputGateBroken = false, .isUserError = false, .resourceLimitExceeded = false}));
static_assert(alarmRetryCountsAgainstLimit(
    {.outputGateBroken = false, .isUserError = false, .resourceLimitExceeded = true}));
static_assert(alarmRetryCountsAgainstLimit(
    {.outputGateBroken = false, .isUserError = true, .resourceLimitExceeded = false}));
static_assert(alarmRetryCountsAgainstLimit(
    {.outputGateBroken = false, .isUserError = true, .resourceLimitExceeded = true}));

}  // namespace
}  // namespace workerd::api
