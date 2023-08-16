// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "gpu-async-runner.h"
#include "workerd/io/io-context.h"
#include <kj/common.h>
#include <kj/debug.h>

#define BUSY_LOOP_DELAY_MS 50

namespace workerd::api::gpu {

void AsyncRunner::Begin() {
  KJ_ASSERT(count_ != std::numeric_limits<decltype(count_)>::max());
  if (count_++ == 0) {
    QueueTick();
  }
}

void AsyncRunner::End() {
  KJ_ASSERT(count_ > 0);
  count_--;
}

void AsyncRunner::QueueTick() {
  if (tick_queued_) {
    return;
  }
  tick_queued_ = true;

  IoContext::current().setTimeoutImpl(
      timeoutIdGenerator, false,
      [this](jsg::Lock& js) mutable {
        this->tick_queued_ = false;
        if (this->count_ > 0) {
          this->device_.Tick();
          QueueTick();
        }
      },
      BUSY_LOOP_DELAY_MS);
}

} // namespace workerd::api::gpu
