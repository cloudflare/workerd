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

constexpr kj::StringPtr WORKER_SOURCE = R"JS(
  let saved;
  export default {
    async fetch(request, env, ctx) {
      if (ctx.oidc !== ctx.oidc) throw new Error("unstable OIDC getter");
      saved ??= ctx.oidc;
      const options = await request.json();
      return new Response(await saved.getToken(options));
    },
    async scheduled(controller, env, ctx) {
      await ctx.oidc.getToken();
    },
    async queue(batch, env, ctx) {
      await ctx.oidc.getToken();
    },
  };
)JS"_kj;

KJ_TEST("OIDC uses the current host channel for saved handles, cron, and queues") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setWorkersOidc(true);
  kj::Vector<IoChannelFactory::OidcTokenRequest> requests;
  uint identity = 0;
  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .mainModuleSource = WORKER_SOURCE,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<RecordingOidcFactory>(timer, requests, ++identity);
  }),
  });

  auto first = fixture.runRequest(kj::HttpMethod::POST, "https://example.test", "{}");
  KJ_EXPECT(first.body == "token-for-1");
  auto second = fixture.runRequest(kj::HttpMethod::POST, "https://example.test",
      R"({"audience":"urn:example:recipient","workerId":"forged","subjectId":"forged"})");
  KJ_EXPECT(second.body == "token-for-2");
  auto entrypoint = fixture.makeWorkerEntrypoint();
  entrypoint->runScheduled(kj::UNIX_EPOCH, "* * * * *").wait(fixture.getWaitScope());
  auto queueEntrypoint = fixture.makeWorkerEntrypoint();
  queueEntrypoint
      ->customEvent(kj::refcounted<QueueCustomEvent>(QueueEvent::Params{
        .queueName = kj::str("oidc-test"),
        .messages = nullptr,
        .metadata = {},
      }))
      .wait(fixture.getWaitScope());

  KJ_ASSERT(requests.size() == 4);
  KJ_EXPECT(requests[0].audience == kj::none);
  KJ_EXPECT(KJ_ASSERT_NONNULL(requests[1].audience) == "urn:example:recipient");
  KJ_EXPECT(requests[2].audience == kj::none);
  KJ_EXPECT(requests[3].audience == kj::none);
  for (auto& request: requests) {
    KJ_EXPECT(request.subject == IoChannelFactory::OidcSubject::WORKER);
  }
}

KJ_TEST("OIDC chooses object identity by default and only accepts defined DO subject modes") {
  capnp::MallocMessageBuilder message;
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setWorkersOidc(true);
  kj::Vector<IoChannelFactory::OidcTokenRequest> requests;
  TestFixture fixture({
    .featureFlags = flags.asReader(),
    .actorId = Worker::Actor::Id(kj::str("oidc-test-object")),
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<RecordingOidcFactory>(timer, requests, 1);
  }),
  });
  for (auto subject: {""_kj, "object"_kj, "namespace"_kj}) {
    auto token = fixture.runInIoContext([&](const TestFixture::Environment& env) {
      auto oidc = env.js.alloc<OidcContext>();
      OidcContext::TokenOptions options;
      if (subject.size() > 0) options.subject = jsg::NonCoercible<kj::String>{kj::str(subject)};
      if (subject == "namespace")
        options.audience = jsg::NonCoercible<kj::String>{kj::str("https://api.example.com")};
      return env.context.awaitJs(env.js, oidc->getToken(env.js, kj::mv(options)));
    });
    KJ_EXPECT(token == "token-for-1");
  }
  KJ_ASSERT(requests.size() == 3);
  KJ_EXPECT(requests[0].subject == IoChannelFactory::OidcSubject::DURABLE_OBJECT);
  KJ_EXPECT(requests[1].subject == IoChannelFactory::OidcSubject::DURABLE_OBJECT);
  KJ_EXPECT(requests[2].subject == IoChannelFactory::OidcSubject::DURABLE_OBJECT_NAMESPACE);
  KJ_EXPECT(KJ_ASSERT_NONNULL(requests[2].audience) == "https://api.example.com");
}

KJ_TEST("OIDC validation rejects invalid options before calling the host") {
  kj::Vector<IoChannelFactory::OidcTokenRequest> requests;
  TestFixture fixture({
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<RecordingOidcFactory>(timer, requests, 1);
  }),
  });
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto oidc = env.js.alloc<OidcContext>();
    for (auto audience: {""_kj, " "_kj, "\xc2\xa0"_kj, "https://example.com/a b"_kj,
           "urn:bad:%zz"_kj, ":missing-scheme"_kj, "1bad:scheme"_kj, "\xc0\xaf"_kj,
           "\xed\xa0\x80"_kj, "\xf4\x90\x80\x80"_kj}) {
      KJ_EXPECT_THROW_MESSAGE("OIDC audience",
          oidc->getToken(env.js,
              OidcContext::TokenOptions{
                .audience = jsg::NonCoercible<kj::String>{kj::str(audience)}}));
    }
    for (auto subject: {"object"_kj, "namespace"_kj, "worker:forged"_kj, ""_kj}) {
      KJ_EXPECT_THROW_MESSAGE("OIDC subject",
          oidc->getToken(env.js,
              OidcContext::TokenOptions{
                .subject = jsg::NonCoercible<kj::String>{kj::str(subject)}}));
    }
    auto oversized = kj::heapArray<char>(2050);
    oversized.asPtr().fill('a');
    oversized[2049] = '\0';
    KJ_EXPECT_THROW_MESSAGE("OIDC audience",
        oidc->getToken(env.js,
            OidcContext::TokenOptions{
              .audience = jsg::NonCoercible<kj::String>{kj::String(kj::mv(oversized))}}));
  });
  KJ_EXPECT(requests.size() == 0);
}

KJ_TEST("OIDC preserves accepted StringOrURI audiences exactly on the host channel") {
  kj::Vector<IoChannelFactory::OidcTokenRequest> requests;
  TestFixture fixture({
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<RecordingOidcFactory>(timer, requests, 1);
  }),
  });
  for (auto audience: {" two words "_kj, "雪 🔐"_kj, "urn:example:recipient"_kj,
         "https://EXAMPLE.com:443/a/../b?x=%2f"_kj}) {
    fixture.runInIoContext([&](const TestFixture::Environment& env) {
      auto oidc = env.js.alloc<OidcContext>();
      return env.context
          .awaitJs(env.js,
              oidc->getToken(env.js,
                  OidcContext::TokenOptions{
                    .audience = jsg::NonCoercible<kj::String>{kj::str(audience)}}))
          .ignoreResult();
    });
    KJ_EXPECT(KJ_ASSERT_NONNULL(requests.back().audience) == audience);
  }
}

KJ_TEST("OIDC propagates signer rejection without falling back to fetch") {
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
    auto oidc = env.js.alloc<OidcContext>();
    return env.context.awaitJs(env.js, oidc->getToken(env.js, kj::none)).ignoreResult();
  }));
}

}  // namespace
}  // namespace workerd::api
