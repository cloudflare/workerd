// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Tests of IoContext::awaitJs() with a continuation function.

#include <workerd/io/io-context.h>
#include <workerd/jsg/jsg.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd {
namespace {

// Records its destruction so a test can tell who owned it and when it went away.
struct Tracked {
  explicit Tracked(bool& destroyed): destroyed(destroyed) {}
  ~Tracked() noexcept(false) {
    destroyed = true;
  }
  bool& destroyed;
};

KJ_TEST("awaitJs() with a continuation resolves to the continuation's result") {
  TestFixture fixture;
  auto result = fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto promise = env.js.resolvedPromise(kj::str("input"));
    return env.context.awaitJs(env.js, kj::mv(promise),
        [](jsg::Lock&, kj::String&& value) { return kj::str(value, "-transformed"); });
  });
  KJ_EXPECT(result == "input-transformed");
}

KJ_TEST("awaitJs() with a continuation supports void JS promises") {
  TestFixture fixture;
  auto result = fixture.runInIoContext([&](const TestFixture::Environment& env) {
    return env.context.awaitJs(env.js, env.js.resolvedPromise(), [](jsg::Lock&) { return 42; });
  });
  KJ_EXPECT(result == 42);
}

KJ_TEST("awaitJs() with a continuation supports void results") {
  TestFixture fixture;
  bool ran = false;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    return env.context.awaitJs(
        env.js, env.js.resolvedPromise(kj::str("input")), [&ran](jsg::Lock&, kj::String&& value) {
      KJ_EXPECT(value == "input");
      ran = true;
    });
  });
  KJ_EXPECT(ran);
}

KJ_TEST("awaitJs() with a continuation chains a returned KJ promise") {
  TestFixture fixture;
  auto result = fixture.runInIoContext([&](const TestFixture::Environment& env) {
    return env.context.awaitJs(
        env.js, env.js.resolvedPromise(), [](jsg::Lock&) -> kj::Promise<int> { return 7; });
  });
  KJ_EXPECT(result == 7);
}

KJ_TEST("awaitJs() rejects with a tunneled exception when the continuation throws") {
  TestFixture fixture;
  auto exception = kj::runCatchingExceptions([&]() {
    fixture.runInIoContext([&](const TestFixture::Environment& env) {
      return env.context.awaitJs(env.js, env.js.resolvedPromise(),
          [](jsg::Lock&) -> int { JSG_FAIL_REQUIRE(TypeError, "boom"); });
    });
  });
  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_EXPECT(e.getDescription().contains("boom"), e.getDescription());
  KJ_EXPECT(jsg::isTunneledException(e.getDescription()), e.getDescription());
}

KJ_TEST("awaitJs() rejects when the JS promise rejects without running the continuation") {
  TestFixture fixture;
  bool ran = false;
  auto exception = kj::runCatchingExceptions([&]() {
    fixture.runInIoContext([&](const TestFixture::Environment& env) {
      auto promise = env.js.rejectedPromise<void>(env.js.typeError("rejected"));
      return env.context.awaitJs(env.js, kj::mv(promise), [&ran](jsg::Lock&) -> int {
        ran = true;
        return 0;
      });
    });
  });
  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_EXPECT(e.getDescription().contains("rejected"), e.getDescription());
  KJ_EXPECT(!ran);
}

KJ_TEST("awaitJs() hands the continuation's result to KJ in the same microtask") {
  // The continuation's result must be owned by the returned KJ promise as soon as the
  // continuation returns. A microtask queued right behind the continuation drops the KJ promise,
  // which must destroy the result. If the transfer to KJ took another microtask, that microtask
  // would be queued behind this one, and the result would still be parked in the IoContext.
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  auto& context = request->getContext();

  bool destroyed = false;
  bool ran = false;
  bool destroyedWhenDropped = false;
  kj::Maybe<kj::Promise<kj::Own<Tracked>>> kjPromise;
  context
      .run([&](Worker::Lock& lock, IoContext& context) {
    auto& js = jsg::Lock::from(lock.getIsolate());
    // The JS promise is already resolved, so the continuation is queued immediately, ahead of
    // the microtask below.
    kjPromise = context.awaitJs(js, js.resolvedPromise(), [&](jsg::Lock&) {
      ran = true;
      return kj::heap<Tracked>(destroyed);
    });
    js.resolvedPromise().then(js, [&](jsg::Lock&) {
      KJ_ASSERT(ran);
      KJ_ASSERT(!destroyed);
      kjPromise = kj::none;
      destroyedWhenDropped = destroyed;
    });
  }).wait(fixture.getWaitScope());

  KJ_EXPECT(destroyedWhenDropped);
  KJ_EXPECT(kjPromise == kj::none);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("awaitJs() doesn't run the continuation once the KJ promise is destroyed") {
  // The continuation may refer to state owned by the caller, which is gone once the caller is
  // canceled. The JS promise isn't canceled along with it, so it can still resolve later.
  TestFixture fixture;
  auto request = fixture.newIncomingRequest();
  auto& context = request->getContext();

  bool ran = false;
  kj::Maybe<kj::Promise<void>> kjPromise;
  kj::Maybe<jsg::Promise<void>::Resolver> resolver;
  context
      .run([&](Worker::Lock& lock, IoContext& context) {
    auto& js = jsg::Lock::from(lock.getIsolate());
    auto paf = js.newPromiseAndResolver<void>();
    resolver = kj::mv(paf.resolver);
    kjPromise = context.awaitJs(js, kj::mv(paf.promise), [&ran](jsg::Lock&) { ran = true; });
  }).wait(fixture.getWaitScope());

  kjPromise = kj::none;

  context
      .run([&](Worker::Lock& lock, IoContext& context) {
    auto& js = jsg::Lock::from(lock.getIsolate());
    KJ_ASSERT_NONNULL(resolver).resolve(js);
    resolver = kj::none;
  }).wait(fixture.getWaitScope());

  KJ_EXPECT(!ran);

  fixture.drainAndDestroy(kj::mv(request));
}

}  // namespace
}  // namespace workerd
