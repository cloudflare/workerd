// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor.h"
#include "global-scope.h"

#include <workerd/io/io-context.h>
#include <workerd/io/worker-interface.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

// Minimal WorkerInterface that answers every outgoing request() with an empty 200, draining the
// request body first so a streaming sender doesn't block on backpressure.
class MockFetchTarget final: public WorkerInterface {
 public:
  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    co_await requestBody.readAllBytes();
    // Build the response headers on the same HttpHeaderTable as the request headers; the runtime
    // reads the response with its own registered header IDs, so a fresh table would mismatch.
    auto responseHeaders = headers.cloneShallow();
    responseHeaders.clear();
    response.send(200, "OK"_kj, responseHeaders, static_cast<uint64_t>(0));
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
  kj::Promise<void> prewarm(kj::StringPtr url) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    return event->notSupported();
  }
};

class TestStreamSource final: public ReadableStreamSource {
 public:
  kj::Promise<size_t> tryRead(void*, size_t, size_t) override {
    return static_cast<size_t>(0);
  }
};

class RetryMetadataOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  RetryMetadataOutgoingFactory(bool& ordinaryDispatchCalled,
      kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata>& capturedMetadata)
      : ordinaryDispatchCalled(ordinaryDispatchCalled),
        capturedMetadata(capturedMetadata) {}

  kj::Own<WorkerInterface> newSingleUseClient(kj::Maybe<kj::String>) override {
    ordinaryDispatchCalled = true;
    return kj::heap<MockFetchTarget>();
  }

  bool supportsActorRetryMetadata() const override {
    return true;
  }

  kj::Own<WorkerInterface> newSingleUseClientWithActorRetryMetadata(kj::Maybe<kj::String>,
      kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> actorRetryRequestMetadata) override {
    capturedMetadata = kj::mv(actorRetryRequestMetadata);
    return kj::heap<MockFetchTarget>();
  }

 private:
  bool& ordinaryDispatchCalled;
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata>& capturedMetadata;
};

class UnsupportedOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  UnsupportedOutgoingFactory(bool& called): called(called) {}

  kj::Own<WorkerInterface> newSingleUseClient(kj::Maybe<kj::String>) override {
    called = true;
    return kj::heap<MockFetchTarget>();
  }

 private:
  bool& called;
};

class MockActorId final: public ActorIdFactory::ActorId {
 public:
  kj::String toString() const override {
    return kj::str("actor-id");
  }

  kj::Maybe<kj::StringPtr> getName() const override {
    return kj::none;
  }

  kj::Maybe<kj::StringPtr> getJurisdiction() const override {
    return kj::none;
  }

  bool equals(const ActorId& other) const override {
    return other.toString() == "actor-id";
  }

  kj::Own<ActorId> clone() const override {
    return kj::heap<MockActorId>();
  }
};

class RecordingActorChannel final: public IoChannelFactory::ActorChannel {
 public:
  RecordingActorChannel(kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata>& capturedMetadata)
      : capturedMetadata(capturedMetadata) {}

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    capturedMetadata = kj::mv(metadata.actorRetryRequestMetadata);
    return kj::heap<MockFetchTarget>();
  }

  void requireAllowsTransfer() override {
    KJ_UNIMPLEMENTED("not used in this test");
  }

  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }

 private:
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata>& capturedMetadata;
};

struct ActorIoChannelFactory final: public TestFixture::DummyIoChannelFactory {
  ActorIoChannelFactory(
      TimerChannel& timer, kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata>& capturedMetadata)
      : DummyIoChannelFactory(timer),
        capturedMetadata(capturedMetadata) {}

  kj::Own<ActorChannel> getGlobalActor(uint,
      const ActorIdFactory::ActorId&,
      kj::Maybe<kj::String>,
      ActorGetMode,
      bool,
      ActorRoutingMode,
      SpanParent,
      kj::Maybe<ActorVersion>,
      Persistent) override {
    return kj::refcounted<RecordingActorChannel>(capturedMetadata);
  }

  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata>& capturedMetadata;
};

KJ_TEST("fetch generates actor retry metadata for a supported outgoing factory") {
  bool ordinaryDispatchCalled = false;
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> capturedMetadata;
  kj::Date beforeFetch = kj::UNIX_EPOCH;
  kj::Date afterFetch = kj::UNIX_EPOCH;
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = env.js.alloc<Fetcher>(
        env.context.addObject<Fetcher::OutgoingFactory>(
            kj::heap<RetryMetadataOutgoingFactory>(ordinaryDispatchCalled, capturedMetadata)),
        Fetcher::RequiresHostAndProtocol::YES);
    beforeFetch = kj::systemCoarseCalendarClock().now();
    auto promise = fetcher->fetch(env.js, kj::str("http://example.com"), kj::none);
    afterFetch = kj::systemCoarseCalendarClock().now();
    return env.context.awaitJs(env.js, kj::mv(promise)).ignoreResult().attach(kj::mv(fetcher));
  });

  KJ_EXPECT(!ordinaryDispatchCalled);
  KJ_IF_SOME(metadata, capturedMetadata) {
    KJ_EXPECT(metadata.createdAt >= beforeFetch);
    KJ_EXPECT(metadata.createdAt <= afterFetch);
    KJ_EXPECT(metadata.isRetry == IsActorRetry::NO);
  } else {
    KJ_FAIL_EXPECT("supported fetch did not generate actor retry metadata");
  }
}

KJ_TEST("fetch omits actor retry metadata for a supported factory with a streaming body") {
  bool ordinaryDispatchCalled = false;
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> capturedMetadata;
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = env.js.alloc<Fetcher>(
        env.context.addObject<Fetcher::OutgoingFactory>(
            kj::heap<RetryMetadataOutgoingFactory>(ordinaryDispatchCalled, capturedMetadata)),
        Fetcher::RequiresHostAndProtocol::YES);
    RequestInitializerDict init;
    init.method = kj::str("POST");
    init.body = kj::Maybe<Body::Initializer>(
        JsReadableStream::create(env.js, env.context, kj::heap<TestStreamSource>()));
    auto promise = fetcher->fetch(env.js, kj::str("http://example.com"), kj::mv(init));
    return env.context.awaitJs(env.js, kj::mv(promise)).ignoreResult().attach(kj::mv(fetcher));
  });

  KJ_EXPECT(ordinaryDispatchCalled);
  KJ_EXPECT(capturedMetadata == kj::none);
}

KJ_TEST("fetch omits actor retry metadata for an unsupported outgoing factory") {
  bool ordinaryDispatchCalled = false;
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher =
        env.js.alloc<Fetcher>(env.context.addObject<Fetcher::OutgoingFactory>(
                                  kj::heap<UnsupportedOutgoingFactory>(ordinaryDispatchCalled)),
            Fetcher::RequiresHostAndProtocol::YES);
    auto promise = fetcher->fetch(env.js, kj::str("http://example.com"), kj::none);
    return env.context.awaitJs(env.js, kj::mv(promise)).ignoreResult().attach(kj::mv(fetcher));
  });

  KJ_EXPECT(ordinaryDispatchCalled);
}

// Isolate Fetcher's dispatch decision from actor routing: a metadata-aware factory must receive the
// caller's logical-call metadata unchanged.
KJ_TEST("Fetcher dispatches actor retry metadata through the metadata-aware factory hook") {
  bool ordinaryDispatchCalled = false;
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> capturedMetadata;
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    Fetcher fetcher(
        env.context.addObject<Fetcher::OutgoingFactory>(
            kj::heap<RetryMetadataOutgoingFactory>(ordinaryDispatchCalled, capturedMetadata)),
        Fetcher::RequiresHostAndProtocol::YES);

    auto client = fetcher.getClientWithTracing(env.context, kj::none, "fetch"_kjc,
        IoChannelFactory::ActorRetryRequestMetadata{
          .nonce = 0x123456789abcdef0,
          .createdAt = kj::UNIX_EPOCH + 123 * kj::MILLISECONDS,
          .isRetry = IsActorRetry::YES,
        });

    KJ_IF_SOME(metadata, capturedMetadata) {
      KJ_EXPECT(metadata.nonce == 0x123456789abcdef0);
      KJ_EXPECT(metadata.createdAt == kj::UNIX_EPOCH + 123 * kj::MILLISECONDS);
      KJ_EXPECT(metadata.isRetry == IsActorRetry::YES);
    } else {
      KJ_FAIL_EXPECT("actor retry metadata was not forwarded");
    }
  });
}

// Never fall back to ordinary dispatch when a factory cannot carry retry metadata. Doing so would
// silently replace the caller's logical-call token at a later seam.
KJ_TEST("Fetcher rejects actor retry metadata instead of using ordinary factory dispatch") {
  bool called = false;
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    Fetcher fetcher(env.context.addObject<Fetcher::OutgoingFactory>(
                        kj::heap<UnsupportedOutgoingFactory>(called)),
        Fetcher::RequiresHostAndProtocol::YES);

    KJ_EXPECT_THROW_MESSAGE("actor retry metadata supplied to an unsupported Fetcher",
        fetcher.getClientWithTracing(env.context, kj::none, "fetch"_kjc,
            IoChannelFactory::ActorRetryRequestMetadata{
              .nonce = 1,
              .createdAt = kj::UNIX_EPOCH,
              .isRetry = IsActorRetry::NO,
            }));
    KJ_EXPECT(!called);
  });
}

// A numeric subrequest channel has no metadata-aware factory hook, so reject before dispatch rather
// than silently dropping the logical-call metadata.
KJ_TEST("Fetcher rejects actor retry metadata before numeric subrequest-channel dispatch") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    Fetcher fetcher(static_cast<uint>(1), Fetcher::RequiresHostAndProtocol::YES);

    KJ_EXPECT_THROW_MESSAGE("actor retry metadata supplied to an unsupported Fetcher",
        fetcher.getClientWithTracing(env.context, kj::none, "fetch"_kjc,
            IoChannelFactory::ActorRetryRequestMetadata{
              .nonce = 1,
              .createdAt = kj::UNIX_EPOCH,
              .isRetry = IsActorRetry::NO,
            }));
  });
}

// Exercise the real implementation of the hook tested above: global actor factories must place the
// caller's metadata on the subrequest sent through the actor channel.
KJ_TEST("GlobalActorOutgoingFactory places actor retry metadata on the actor subrequest") {
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> capturedMetadata;
  TestFixture fixture(TestFixture::SetupParams{
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<ActorIoChannelFactory>(timer, capturedMetadata);
  }),
  });

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    GlobalActorOutgoingFactory factory(
        GlobalActorOutgoingFactory::ChannelIdOrFactory(static_cast<uint>(1)),
        env.js.alloc<DurableObjectId>(kj::heap<MockActorId>()), kj::none,
        ActorGetMode::GET_OR_CREATE, false, ActorRoutingMode::DEFAULT, kj::none, Persistent::NO);
    KJ_EXPECT(factory.supportsActorRetryMetadata());

    auto client = factory.newSingleUseClientWithActorRetryMetadata(kj::none,
        IoChannelFactory::ActorRetryRequestMetadata{
          .nonce = 0x123456789abcdef0,
          .createdAt = kj::UNIX_EPOCH + 123 * kj::MILLISECONDS,
          .isRetry = IsActorRetry::YES,
        });

    KJ_IF_SOME(metadata, capturedMetadata) {
      KJ_EXPECT(metadata.nonce == 0x123456789abcdef0);
      KJ_EXPECT(metadata.createdAt == kj::UNIX_EPOCH + 123 * kj::MILLISECONDS);
      KJ_EXPECT(metadata.isRetry == IsActorRetry::YES);
    } else {
      KJ_FAIL_EXPECT("actor retry metadata was not forwarded to the actor channel");
    }

  });
}

KJ_TEST("ReplicaActorOutgoingFactory places actor retry metadata on the actor subrequest") {
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> capturedMetadata;
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    ReplicaActorOutgoingFactory factory(
        kj::refcounted<RecordingActorChannel>(capturedMetadata), kj::str("actor-id"));
    KJ_EXPECT(factory.supportsActorRetryMetadata());

    auto client = factory.newSingleUseClientWithActorRetryMetadata(kj::none,
        IoChannelFactory::ActorRetryRequestMetadata{
          .nonce = 0x123456789abcdef0,
          .createdAt = kj::UNIX_EPOCH + 123 * kj::MILLISECONDS,
          .isRetry = IsActorRetry::YES,
        });

    KJ_IF_SOME(metadata, capturedMetadata) {
      KJ_EXPECT(metadata.nonce == 0x123456789abcdef0);
      KJ_EXPECT(metadata.createdAt == kj::UNIX_EPOCH + 123 * kj::MILLISECONDS);
      KJ_EXPECT(metadata.isRetry == IsActorRetry::YES);
    } else {
      KJ_FAIL_EXPECT("actor retry metadata was not forwarded to the actor channel");
    }

  });
}

}  // namespace
}  // namespace workerd::api
