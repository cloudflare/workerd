// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "v8-platform-impl.h"

#include <kj/time.h>

namespace workerd::server {

double WorkerdPlatform::CurrentClockTimeMillis() noexcept {
  return (kj::systemPreciseCalendarClock().now() - kj::UNIX_EPOCH) / kj::MILLISECONDS;
}

}  // namespace workerd::server
