// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Verifies that kj::setupAsyncIo() -- the public kj entry point, called exactly as cli-main.c++
// calls it -- resolves to the tokio-backed implementation from //src/workerd/util:setup-async-io.
// If kj's own definition were linked instead (kj-async-os leaking in through a dependency edge),
// the process would run on kj::UnixEventPort while tests that build their own event loop stayed
// green. This test observes which one is actually bound.

#include <kj/async-io.h>
#include <kj/test.h>
#include <kj/time.h>

#if !_WIN32
#include <kj/async-unix.h>
#endif

#include <kj-rs-io/async-io.h>

namespace workerd {
namespace {

KJ_TEST("kj::setupAsyncIo() resolves to the tokio I/O backend") {
  auto io = kj::setupAsyncIo();

  // The event loop must actually be driven: a timer wait completes only if the port installed by
  // setupAsyncIo() sleeps and wakes correctly.
  io.provider->getTimer().afterDelay(1 * kj::MILLISECONDS).wait(io.waitScope);

  // The providers are the tokio-backed ones from kj-rs-io...
  KJ_EXPECT(dynamic_cast<kj_rs_io::TokioAsyncIoProvider*>(io.provider.get()) != nullptr,
      "kj::setupAsyncIo() returned a non-tokio AsyncIoProvider: "
      "kj's own setupAsyncIo() won the link (kj-async-os is being linked)");
  KJ_EXPECT(
      dynamic_cast<kj_rs_io::TokioLowLevelAsyncIoProvider*>(io.lowLevelProvider.get()) != nullptr,
      "kj::setupAsyncIo() returned a non-tokio LowLevelAsyncIoProvider");
#if !_WIN32
  // ...and the kj::UnixEventPort the context names is the shim's inert one (kj's real port would
  // poll and return), which also proves the shim's TU, not async-unix.c++, defines the class.
  KJ_EXPECT_THROW_MESSAGE("inert (tokio drives the loop)", io.unixEventPort.poll());
#endif
}

}  // namespace
}  // namespace workerd
