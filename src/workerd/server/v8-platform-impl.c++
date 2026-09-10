// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "v8-platform-impl.h"

#include <workerd/io/io-util.h>

namespace workerd::server {

double WorkerdPlatform::CurrentClockTimeMillis() noexcept {
  return dateNow();
}

}  // namespace workerd::server
