// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "oidc.h"
#include "queue.h"

#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

class RecordingOidcFactory final: public TestFixture::DummyIoChannelFactory {
 public:
  RecordingOidcFactory(TimerChannel& timer, kj::Vector<OidcTokenRequest>& requests, uint identity)
      : DummyIoChannelFactory(timer),
        requests(requests),
        identity(identity) {}

  kj::Promise<kj::String> getOidcToken(OidcTokenRequest request) override {
    requests.add(kj::mv(request));
    // Complete asynchronously to exercise the KJ/JS promise bridge.
    return kj::evalLater([identity = identity]() { return kj::str("token-for-", identity); });
  }

 private:
  kj::Vector<OidcTokenRequest>& requests;
  uint identity;
};

KJ_TEST("OIDC binding passes fixed assignment and freshness to the current host") {
  kj::Vector<IoChannelFactory::OidcTokenRequest> requests;
  uint identity = 0;
  uint subrequests = 0;
  TestFixture fixture({
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<RecordingOidcFactory>(timer, requests, ++identity);
  }),
    .checkedSubrequestCount = subrequests,
  });
  for (auto assignment: {"orders"_kj, "inference"_kj}) {
    fixture.runInIoContext([&](const TestFixture::Environment& env) {
      auto binding = env.js.alloc<OidcIdentityBinding>(kj::str(assignment));
      auto options = env.js.obj();
      options.set(env.js, "fresh", env.js.boolean(true));
      options.set(env.js, "audience", env.js.str("urn:example:recipient"_kj));
      return env.context.awaitJs(env.js, binding->getToken(env.js, options)).ignoreResult();
    });
    KJ_EXPECT(requests.back().assignment == assignment);
    KJ_EXPECT(requests.back().fresh);
    KJ_EXPECT(KJ_ASSERT_NONNULL(requests.back().audience) == "urn:example:recipient");
  }
  KJ_EXPECT(requests.size() == 2);
  KJ_EXPECT(subrequests == 2);
}

KJ_TEST("OIDC saved binding uses the current host on every call") {
  kj::Vector<IoChannelFactory::OidcTokenRequest> requests;
  uint identity = 0;
  TestFixture fixture({
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<RecordingOidcFactory>(timer, requests, ++identity);
  }),
  });
  kj::Maybe<jsg::Ref<OidcIdentityBinding>> saved;
  for (uint i = 1; i <= 2; ++i) {
    auto token = fixture.runInIoContext([&](const TestFixture::Environment& env) {
      if (saved == kj::none) saved = env.js.alloc<OidcIdentityBinding>(kj::str("orders"));
      return env.context.awaitJs(env.js, KJ_ASSERT_NONNULL(saved)->getToken(env.js, kj::none));
    });
    KJ_EXPECT(token == kj::str("token-for-", i));
    KJ_EXPECT(requests.back().assignment == "orders");
  }
  saved = kj::none;
}

KJ_TEST("OIDC rejects unknown options before calling the host") {
  kj::Vector<IoChannelFactory::OidcTokenRequest> requests;
  TestFixture fixture({
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<RecordingOidcFactory>(timer, requests, 1);
  }),
  });
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto binding = env.js.alloc<OidcIdentityBinding>(kj::str("orders"));
    for (auto name: {"subject"_kj, "identityId"_kj, "issuer"_kj, "claims"_kj, "objectId"_kj}) {
      auto options = env.js.obj();
      options.set(env.js, name, env.js.str("forged"_kj));
      KJ_EXPECT_THROW_MESSAGE("Unknown OIDC option", binding->getToken(env.js, options));
    }
    auto options = env.js.obj();
    options.set(env.js, "fresh", env.js.str("true"_kj));
    KJ_EXPECT_THROW_MESSAGE("OIDC fresh must be a boolean", binding->getToken(env.js, options));
  });
  KJ_EXPECT(requests.size() == 0);
}

KJ_TEST("OIDC requires an active I/O context") {
  TestFixture fixture;
  fixture.enterWorkerLock([](Worker::Lock& lock) {
    v8::HandleScope handleScope(lock.getIsolate());
    v8::Context::Scope contextScope(lock.getContext());
    auto& js = jsg::Lock::from(lock.getIsolate());
    auto binding = js.alloc<OidcIdentityBinding>(kj::str("orders"));
    v8::TryCatch caught(js.v8Isolate);
    try {
      binding->getToken(js, kj::none);
      KJ_FAIL_EXPECT("OIDC call outside I/O context succeeded");
    } catch (jsg::JsExceptionThrown&) {
      KJ_EXPECT(caught.HasCaught());
      v8::String::Utf8Value message(js.v8Isolate, caught.Exception());
      KJ_EXPECT(kj::StringPtr(*message).contains("perform this operation within a handler"));
    }
  });
}

KJ_TEST("OIDC propagates policy denial") {
  class RejectingFactory final: public TestFixture::DummyIoChannelFactory {
   public:
    using DummyIoChannelFactory::DummyIoChannelFactory;
    kj::Promise<kj::String> getOidcToken(OidcTokenRequest request) override {
      return JSG_KJ_EXCEPTION(FAILED, Error, "OIDC issuance denied by host policy");
    }
  };
  TestFixture fixture({
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<RejectingFactory>(timer);
  }),
  });
  KJ_EXPECT_THROW_MESSAGE("OIDC issuance denied by host policy",
      fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto binding = env.js.alloc<OidcIdentityBinding>(kj::str("orders"));
    return env.context.awaitJs(env.js, binding->getToken(env.js, kj::none)).ignoreResult();
  }));
}

}  // namespace
}  // namespace workerd::api
