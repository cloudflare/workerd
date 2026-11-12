// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/io-context.h>
#include <workerd/jsg/setup.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd {
namespace {

KJ_TEST("an isolate whose Rust resource Drop panicked is aborted before it runs JavaScript") {
  TestFixture fixture;
  uint runCount = 0;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    ++runCount;
    jsg::IsolateBase::from(env.isolate).setResourceDropPanicked();
  });

  // TestFixture's abortIsolate() fails with "no abortIsolate", which shows the request reached it.
  KJ_EXPECT_THROW_MESSAGE("no abortIsolate",
      fixture.runInIoContext([&](const TestFixture::Environment& env) { ++runCount; }));
  KJ_EXPECT(runCount == 1);
}

}  // namespace
}  // namespace workerd
