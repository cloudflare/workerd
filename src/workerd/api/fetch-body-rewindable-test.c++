// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "global-scope.h"

#include "actor.h"

#include <workerd/io/io-context.h>
#include <workerd/io/observer.h>
#include <workerd/io/worker-interface.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

#include <set>

namespace workerd::api {
namespace {

// Records, in call order, every value passed to setNextSubrequestBodyRewindable().
class RecordingRequestObserver final: public RequestObserver {
 public:
  RecordingRequestObserver(kj::Vector<bool>& calls): calls(calls) {}

  void setNextSubrequestBodyRewindable(SubrequestBodyRewindable bodyRewindable) override {
    calls.add(bodyRewindable.toBool());
  }

 private:
  kj::Vector<bool>& calls;
};

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

class RetryMetadataOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  RetryMetadataOutgoingFactory(ActorRetryEligibility& capturedEligibility,
      kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata>& capturedMetadata)
      : capturedEligibility(capturedEligibility), capturedMetadata(capturedMetadata) {}

  kj::Own<WorkerInterface> newSingleUseClient(kj::Maybe<kj::String>,
      ActorRetryEligibility actorRetryEligibility,
      kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> actorRetryRequestMetadata) override {
    capturedEligibility = actorRetryEligibility;
    capturedMetadata = kj::mv(actorRetryRequestMetadata);
    return kj::heap<MockFetchTarget>();
  }

 private:
  ActorRetryEligibility& capturedEligibility;
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata>& capturedMetadata;
};

class UnsupportedOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  UnsupportedOutgoingFactory(bool& called): called(called) {}

  kj::Own<WorkerInterface> newSingleUseClient(kj::Maybe<kj::String>,
      ActorRetryEligibility actorRetryEligibility,
      kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> actorRetryRequestMetadata) override {
    KJ_REQUIRE(actorRetryEligibility == ActorRetryEligibility::INELIGIBLE,
        "actor retry eligibility supplied to an unsupported Fetcher");
    KJ_REQUIRE(actorRetryRequestMetadata == kj::none,
        "actor retry metadata supplied to an unsupported Fetcher");
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
  RecordingActorChannel(kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata>& capturedMetadata,
      ActorRetryEligibility& capturedEligibility)
      : capturedMetadata(capturedMetadata), capturedEligibility(capturedEligibility) {}

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    capturedMetadata = kj::mv(metadata.actorRetryRequestMetadata);
    capturedEligibility = metadata.actorRetryEligibility;
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
  ActorRetryEligibility& capturedEligibility;
};

struct FetchTargetIoChannelFactory final: public TestFixture::DummyIoChannelFactory {
  FetchTargetIoChannelFactory(TimerChannel& timer): DummyIoChannelFactory(timer) {}

  kj::Own<WorkerInterface> startSubrequest(uint channel, SubrequestMetadata metadata) override {
    return kj::heap<MockFetchTarget>();
  }
};

struct ActorIoChannelFactory final: public TestFixture::DummyIoChannelFactory {
  ActorIoChannelFactory(TimerChannel& timer,
      kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata>& capturedMetadata,
      ActorRetryEligibility& capturedEligibility)
      : DummyIoChannelFactory(timer),
        capturedMetadata(capturedMetadata),
        capturedEligibility(capturedEligibility) {}

  kj::Own<ActorChannel> getGlobalActor(uint,
      const ActorIdFactory::ActorId&,
      kj::Maybe<kj::String>,
      ActorGetMode,
      bool,
      ActorRoutingMode,
      SpanParent,
      kj::Maybe<ActorVersion>,
      Persistent) override {
    return kj::refcounted<RecordingActorChannel>(capturedMetadata, capturedEligibility);
  }

  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata>& capturedMetadata;
  ActorRetryEligibility& capturedEligibility;
};

// fetchImplNoOutputLock forwards Request::canRewindBody() to RequestObserver so that, downstream,
// edgeworker can classify retry eligibility for disconnected outgoing actor calls. The subtle
// property here is that the stashed signal is per-call, not sticky: a single RequestObserver is
// shared across every outgoing subrequest in an IoContext, so the value set for one call must not
// carry over into the next. We issue two fetches in one invocation -- a rewindable (buffered) body
// then a non-rewindable (stream) body -- to exercise that shared observer across consecutive calls
// and verify the per-body mapping, the per-call sequencing, and the absence of stale attribution all
// at once (the no-staleness behaviour can only be observed across more than one fetch).
KJ_TEST("fetch reports each outgoing body's rewindability per-call without staleness") {
  kj::Vector<bool> bodyRewindableCalls;

  TestFixture fixture(TestFixture::SetupParams{
    .mainModuleSource = R"SCRIPT(
        export default {
          async fetch(request) {
            // Buffered (string) body: rewindable.
            await fetch("http://example.com/buffered", { method: "POST", body: "hello" });

            // The incoming request body is a (non-buffer-backed) stream, so forwarding it yields a
            // non-rewindable body.
            await fetch("http://example.com/stream",
                { method: "POST", body: request.body, duplex: "half" });

            return new Response("OK");
          },
        };
      )SCRIPT"_kj,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<FetchTargetIoChannelFactory>(timer);
  }),
    .requestObserverFactory =
        kj::Function<kj::Own<RequestObserver>()>([&]() -> kj::Own<RequestObserver> {
    return kj::refcounted<RecordingRequestObserver>(bodyRewindableCalls);
  }),
  });

  auto result =
      fixture.runRequest(kj::HttpMethod::POST, "http://www.example.com"_kj, "incoming-body"_kj);
  KJ_EXPECT(result.statusCode == 200);

  KJ_ASSERT(bodyRewindableCalls.size() == 2,
      "expected exactly one rewindability signal per outgoing fetch");
  KJ_EXPECT(bodyRewindableCalls[0] == true, "buffered request body should be rewindable");
  KJ_EXPECT(bodyRewindableCalls[1] == false,
      "streamed request body should not be rewindable (no carryover)");
}

KJ_TEST("Fetcher forwards actor retry metadata to an opted-in outgoing factory") {
  ActorRetryEligibility capturedEligibility = ActorRetryEligibility::INELIGIBLE;
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> capturedMetadata;
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    Fetcher fetcher(
        env.context.addObject<Fetcher::OutgoingFactory>(
            kj::heap<RetryMetadataOutgoingFactory>(capturedEligibility, capturedMetadata)),
        Fetcher::RequiresHostAndProtocol::YES, ActorRetryEligibility::ELIGIBLE);

    auto client = fetcher.getClientWithTracing(env.context, kj::none, "fetch"_kjc,
        ActorRetryEligibility::ELIGIBLE,
        IoChannelFactory::ActorRetryRequestMetadata{
          .nonce = 0x123456789abcdef0,
          .createdAt = kj::UNIX_EPOCH + 123 * kj::MILLISECONDS,
          .isRetry = IsActorRetry::YES,
        });

    KJ_IF_SOME(metadata, capturedMetadata) {
      KJ_EXPECT(capturedEligibility == ActorRetryEligibility::ELIGIBLE);
      KJ_EXPECT(metadata.nonce == 0x123456789abcdef0);
      KJ_EXPECT(metadata.createdAt == kj::UNIX_EPOCH + 123 * kj::MILLISECONDS);
      KJ_EXPECT(metadata.isRetry == IsActorRetry::YES);
    } else {
      KJ_FAIL_EXPECT("actor retry metadata was not forwarded");
    }
  });
}

KJ_TEST("Fetcher preserves eligible classification when retry metadata is absent") {
  ActorRetryEligibility capturedEligibility = ActorRetryEligibility::INELIGIBLE;
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> capturedMetadata;
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    Fetcher fetcher(
        env.context.addObject<Fetcher::OutgoingFactory>(
            kj::heap<RetryMetadataOutgoingFactory>(capturedEligibility, capturedMetadata)),
        Fetcher::RequiresHostAndProtocol::YES, ActorRetryEligibility::ELIGIBLE);

    auto client = fetcher.getClientWithTracing(env.context, kj::none, "fetch"_kjc,
        ActorRetryEligibility::ELIGIBLE, kj::none);

    KJ_EXPECT(capturedEligibility == ActorRetryEligibility::ELIGIBLE);
    KJ_EXPECT(capturedMetadata == kj::none);
  });
}

KJ_TEST("Fetcher rejects retry metadata on an ineligible request") {
  bool called = false;
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    Fetcher fetcher(
        env.context.addObject<Fetcher::OutgoingFactory>(
            kj::heap<UnsupportedOutgoingFactory>(called)),
        Fetcher::RequiresHostAndProtocol::YES, ActorRetryEligibility::INELIGIBLE);

    KJ_EXPECT_THROW_MESSAGE("retry-ineligible actor request must not carry retry metadata",
        fetcher.getClientWithTracing(env.context, kj::none, "fetch"_kjc,
            ActorRetryEligibility::INELIGIBLE,
            IoChannelFactory::ActorRetryRequestMetadata{
              .nonce = 1,
              .createdAt = kj::UNIX_EPOCH,
              .isRetry = IsActorRetry::NO,
            }));
    KJ_EXPECT(!called);
  });
}

KJ_TEST("Fetcher rejects actor retry metadata for a channel-backed Fetcher") {
  TestFixture fixture;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    Fetcher fetcher(uint(1), Fetcher::RequiresHostAndProtocol::YES);

    KJ_EXPECT_THROW_MESSAGE("actor retry eligibility supplied to a retry-ineligible Fetcher",
        fetcher.getClientWithTracing(env.context, kj::none, "fetch"_kjc,
            ActorRetryEligibility::ELIGIBLE, kj::none));
  });
}

KJ_TEST("GlobalActorOutgoingFactory forwards actor retry metadata to the actor channel") {
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> capturedMetadata;
  ActorRetryEligibility capturedEligibility = ActorRetryEligibility::INELIGIBLE;
  TestFixture fixture(TestFixture::SetupParams{
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
      return kj::rc<ActorIoChannelFactory>(timer, capturedMetadata, capturedEligibility);
    }),
  });

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    GlobalActorOutgoingFactory factory(GlobalActorOutgoingFactory::ChannelIdOrFactory(uint(1)),
        env.js.alloc<DurableObjectId>(kj::heap<MockActorId>()), kj::none,
        ActorGetMode::GET_OR_CREATE, false, ActorRoutingMode::DEFAULT, kj::none, Persistent::NO);

    auto client = factory.newSingleUseClient(kj::none, ActorRetryEligibility::ELIGIBLE,
        IoChannelFactory::ActorRetryRequestMetadata{
          .nonce = 0x123456789abcdef0,
          .createdAt = kj::UNIX_EPOCH + 123 * kj::MILLISECONDS,
          .isRetry = IsActorRetry::YES,
        });

    KJ_IF_SOME(metadata, capturedMetadata) {
      KJ_EXPECT(metadata.nonce == 0x123456789abcdef0);
      KJ_EXPECT(metadata.createdAt == kj::UNIX_EPOCH + 123 * kj::MILLISECONDS);
      KJ_EXPECT(metadata.isRetry == IsActorRetry::YES);
      KJ_EXPECT(capturedEligibility == ActorRetryEligibility::ELIGIBLE);
    } else {
      KJ_FAIL_EXPECT("actor retry metadata was not forwarded to the actor channel");
    }
  });
}

KJ_TEST("GlobalActorOutgoingFactory preserves explicit eligibility without retry metadata") {
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> capturedMetadata;
  ActorRetryEligibility capturedEligibility = ActorRetryEligibility::ELIGIBLE;
  TestFixture fixture(TestFixture::SetupParams{
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
      return kj::rc<ActorIoChannelFactory>(timer, capturedMetadata, capturedEligibility);
    }),
  });

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    GlobalActorOutgoingFactory factory(GlobalActorOutgoingFactory::ChannelIdOrFactory(uint(1)),
        env.js.alloc<DurableObjectId>(kj::heap<MockActorId>()), kj::none,
        ActorGetMode::GET_OR_CREATE, false, ActorRoutingMode::DEFAULT, kj::none, Persistent::NO);

    auto eligibleClient =
        factory.newSingleUseClient(kj::none, ActorRetryEligibility::ELIGIBLE, kj::none);

    KJ_EXPECT(capturedMetadata == kj::none);
    KJ_EXPECT(capturedEligibility == ActorRetryEligibility::ELIGIBLE);

    auto ineligibleClient =
        factory.newSingleUseClient(kj::none, ActorRetryEligibility::INELIGIBLE, kj::none);

    KJ_EXPECT(capturedMetadata == kj::none);
    KJ_EXPECT(capturedEligibility == ActorRetryEligibility::INELIGIBLE);
  });
}

KJ_TEST("generateActorRetryRequestMetadata creates distinct first-attempt tokens") {
  constexpr auto createdAt = kj::UNIX_EPOCH + 123 * kj::MILLISECONDS;
  auto first = generateActorRetryRequestMetadata(createdAt);
  auto second = generateActorRetryRequestMetadata(createdAt);

  KJ_EXPECT(first.nonce != second.nonce);
  KJ_EXPECT(first.createdAt == createdAt);
  KJ_EXPECT(first.isRetry == IsActorRetry::NO);
}

KJ_TEST("generateActorRetryRequestMetadata nonces do not collide in practice") {
  constexpr size_t count = 100'000;
  std::set<uint64_t> seen;
  for (size_t i = 0; i < count; ++i) {
    auto metadata = generateActorRetryRequestMetadata(kj::UNIX_EPOCH);
    KJ_EXPECT(seen.insert(metadata.nonce).second, "nonce collided", metadata.nonce);
  }
}

}  // namespace
}  // namespace workerd::api
