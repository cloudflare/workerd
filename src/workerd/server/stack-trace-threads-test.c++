// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <kj/exception.h>
#include <kj/test.h>
#include <kj/thread.h>
#include <kj/vector.h>

namespace workerd::server {
namespace {

// The loop threads of `workerd serve --threads` log exceptions independently, and a debug build
// symbolizes each one's stack trace with a subprocess. KJ's symbolizer must stand several
// threads doing so at once: a subprocess that inherits another thread's pipes keeps that
// thread's subprocess from ever seeing the end of its input, and the threads wait forever
// (patches/capnp-cpp). A hang here is that; where stack traces are not symbolized (a release
// build, or no llvm-symbolizer) the threads have nothing to wait for.
KJ_TEST("threads stringify stack traces at the same time") {
  kj::Vector<kj::Own<kj::Thread>> threads;
  for (auto thread KJ_UNUSED: kj::zeroTo(4)) {
    threads.add(kj::heap<kj::Thread>([]() {
      for (auto round KJ_UNUSED: kj::zeroTo(6)) {
        void* space[8]{};
        auto text KJ_UNUSED = kj::stringifyStackTrace(kj::getStackTrace(space, 0));
      }
    }));
  }
}

}  // namespace
}  // namespace workerd::server
