// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "io-util.h"

#include "io-context.h"

#include <kj/time.h>

namespace workerd {

double dateNow() {
  KJ_IF_SOME(ioContext, IoContext::tryCurrent()) {
    return (ioContext.now() - kj::UNIX_EPOCH) / kj::MILLISECONDS;
  }

  return 0.0;
}

}  // namespace workerd
