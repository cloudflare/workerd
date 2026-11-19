// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <kj/async.h>
#include <kj/common.h>
#include <kj/debug.h>

namespace workerd::util {

// Keeps the current event loop on the thread it is on for as long as an instance lives.
//
// A KJ event loop usually has a thread to itself. It can also be driven by a scheduler that
// enters it on whichever thread is free and leaves it when it has nothing to do
// (`kj_rs_tokio::LoopUnit`, as `workerd serve --loops` runs its loops). That is sound as long as
// nothing on the loop has stack frames that outlive an event, and a fiber does: a fiber that
// waits keeps its frames until it resumes. Resuming them on another thread is not safe in
// general, since a compiler may keep the address of a thread-local across the call that
// suspended, and C++'s exception state is per thread.
//
// So code that runs on a fiber declares a LoopPin first in the fiber's function. The scheduler
// asks `LoopPin::isPinned()` before it lets a loop go, and keeps a pinned loop on its thread,
// blocking the thread when the loop is idle. A fiber stack that is idle in its pool has no such
// frames, and needs none. A loop with a thread to itself is not affected.
class LoopPin final {
 public:
  LoopPin(): thread(kj::ThreadId::current()) {
    ++*pins;
  }
  ~LoopPin() noexcept(false) {
    --*pins;
    KJ_DTOR_ASSERT(
        thread == kj::ThreadId::current(), "an event loop moved to another thread while pinned");
  }
  KJ_DISALLOW_COPY_AND_MOVE(LoopPin);

  // Whether the current thread's event loop must stay on this thread.
  static bool isPinned() {
    return *pins > 0;
  }

 private:
  kj::ThreadId thread;
  inline static const kj::EventLoopLocal<kj::uint> pins;
};

}  // namespace workerd::util
