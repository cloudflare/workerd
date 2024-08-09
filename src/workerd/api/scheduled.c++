// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "scheduled.h"

namespace workerd::api {

ScheduledEvent::ScheduledEvent(double scheduledTime, kj::StringPtr cron)
    : ExtendableEvent("scheduled"),
      scheduledTime(scheduledTime),
      cron(kj::str(cron)) {}

void ScheduledEvent::noRetry() {
  IoContext::current().setNoRetryScheduled();
}

}  // namespace workerd::api
