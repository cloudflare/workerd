// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "sentry.h"

#include <kj/test.h>

namespace {

void expectSentryTag(kj::StringPtr tag) {
  auto exception = KJ_EXCEPTION(FAILED, "test-error");
  exception.setDetail(workerd::SENTRY_TAG_DETAIL_ID, kj::heapArray(tag.asBytes()));

  KJ_EXPECT_LOG(ERROR, tag);
  LOG_EXCEPTION("sentryTagTest", exception);
  KJ_EXPECT_LOG(ERROR, tag);
  auto wdErrId = workerd::makeInternalErrorId();
  LOG_EXCEPTION_WITH_ID("sentryTagWithIdTest", exception, wdErrId);
  KJ_EXPECT(exception.getDescription() == "test-error", exception);
}

KJ_TEST("Sentry tags are applied only when logging") {
  expectSentryTag("SENTRY_DO"_kj);
  expectSentryTag("SENTRY_RT"_kj);
  expectSentryTag("NOSENTRY"_kj);
}

}  // namespace
