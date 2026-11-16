// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/request-io.h>
#include <workerd/jsg/script.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

// Runs `body` as the body of a function of `io`, returning its result as a string.
kj::String runIoScript(jsg::Lock& js, kj::StringPtr body, jsg::JsValue io) {
  auto code = kj::str("((io) => {", body, "})");
  auto fn = KJ_ASSERT_NONNULL(jsg::NonModuleScript::compile(js, code, "request-io-test.js"_kj)
                                  .runAndReturn(js)
                                  .tryCast<jsg::JsFunction>());
  return fn.call(js, js.undefined(), io).toString(js);
}

KJ_TEST("A RequestIo is current only within the request that created it") {
  TestFixture fixture;
  kj::Maybe<jsg::JsRef<jsg::JsValue>> io;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto created = RequestIo::createForBootstrap(env.js);
    auto result = runIoScript(env.js, R"js(
      io.requireCurrent();
      return [typeof io, io.isCurrent()].join();
    )js"_kj,
        created);
    KJ_EXPECT(result == "object,true"_kj, result);
    io = jsg::JsRef(env.js, created);
  });
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto& handle = KJ_ASSERT_NONNULL(io);
    auto result = runIoScript(env.js, R"js(
      const results = [io.isCurrent()];
      for (const fn of [
        () => io.requireCurrent(),
        () => io.logUncaughtException(new Error('x')),
      ]) {
        try {
          fn();
          results.push('ok');
        } catch (e) {
          results.push(/different request/.test(e.message));
        }
      }
      return results.join();
    )js"_kj,
        handle.getHandle(env.js));
    KJ_EXPECT(result == "false,true,true"_kj, result);
    io = kj::none;
  });
}

}  // namespace
}  // namespace workerd::api
