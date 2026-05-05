// Copyright (c) 2017-2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "duration-exceeded-logger.h"

#include <kj/test.h>
#include <kj/timer.h>

namespace workerd::util {
namespace {

KJ_TEST("Duration alert triggers when time is exceeded") {
  kj::TimerImpl timer(kj::origin<kj::TimePoint>());

  KJ_EXPECT_LOG(WARNING, "durationAlert Test Message; warningDuration = 10s; actualDuration = ");
  // we don't check the actual duration emitted to avoid making the test flaky.
  // this is OK because KJ_EXPECT_LOG just checks for substring occurrences
  {
    DurationExceededLogger duration(timer, 10 * kj::SECONDS, "durationAlert Test Message");
    timer.advanceTo(timer.now() + 100 * kj::SECONDS);
  }
}

KJ_TEST("DURATION_EXCEEDED_LOG with extra params triggers when time is exceeded") {
  kj::TimerImpl timer(kj::origin<kj::TimePoint>());

  int requestId = 42;
  kj::StringPtr operation = "doSomething";

  KJ_EXPECT_LOG(WARNING,
      "test message; requestId = 42; operation = doSomething; warningDuration = 10s; "
      "actualDuration = ");
  {
    DURATION_EXCEEDED_LOG(duration, timer, 10 * kj::SECONDS, "test message", requestId, operation);
    timer.advanceTo(timer.now() + 100 * kj::SECONDS);
  }
}

KJ_TEST("DURATION_EXCEEDED_LOG without extra params triggers when time is exceeded") {
  kj::TimerImpl timer(kj::origin<kj::TimePoint>());

  KJ_EXPECT_LOG(WARNING, "no extras test; warningDuration = 10s; actualDuration = ");
  {
    DURATION_EXCEEDED_LOG(duration, timer, 10 * kj::SECONDS, "no extras test");
    timer.advanceTo(timer.now() + 100 * kj::SECONDS);
  }
}

KJ_TEST("DURATION_EXCEEDED_LOG extra params are lazily evaluated") {
  kj::TimerImpl timer(kj::origin<kj::TimePoint>());

  // This test verifies that the extra params lambda is NOT called when the duration
  // threshold is not exceeded. We use a counter to track whether stringification occurred.
  int stringifyCount = 0;
  auto makeExpensiveString = [&]() -> kj::StringPtr {
    ++stringifyCount;
    return "expensive"_kj;
  };

  {
    DURATION_EXCEEDED_LOG(duration, timer, 10 * kj::SECONDS, "lazy test", makeExpensiveString());
    timer.advanceTo(timer.now() + 1 * kj::SECONDS);
  }
  KJ_EXPECT(stringifyCount == 0, "extra params should not be evaluated when duration not exceeded");
}

}  // namespace
}  // namespace workerd::util
