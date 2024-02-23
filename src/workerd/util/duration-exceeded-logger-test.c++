// Copyright (c) 2017-2024 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "duration-exceeded-logger.h"
#include <kj/timer.h>
#include <kj/test.h>

namespace workerd::util {
namespace {

KJ_TEST("Duration alert triggers when time is exceeded") {
    kj::TimerImpl timer(kj::origin<kj::TimePoint>());

    KJ_EXPECT_LOG(WARNING, "durationAlert Test Message; warningDuration = 10s; actualDuration = ");
    // we don't check the actual duration emitted to avoid making the test flaky.
    // this is okay because KJ_EXPECT_LOG just checks for substring occurences
    {
        DurationExceededLogger duration(timer, 10*kj::SECONDS, "durationAlert Test Message");
        timer.advanceTo(timer.now() + 100*kj::SECONDS);
    }
}

}  // namespace
}  // namespace workerd::util
