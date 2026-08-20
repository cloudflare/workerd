// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "actor.h"
#include "global-scope.h"

#include <workerd/io/io-context.h>
#include <workerd/io/worker-interface.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/autogate.h>

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

  Result newSingleUseClient(kj::Maybe<kj::String>, MakeUserSpanParent) override {
    ordinaryDispatchCalled = true;
    return {.client = kj::heap<MockFetchTarget>(), .spanParents = kj::none};
  }

  bool supportsActorFetchRetries() const override {
    return true;
  }

  Result newSingleUseClientWithActorRetryMetadata(kj::Maybe<kj::String>,
      kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> actorRetryRequestMetadata,
      MakeUserSpanParent) override {
    capturedMetadata = kj::mv(actorRetryRequestMetadata);
    return {.client = kj::heap<MockFetchTarget>(), .spanParents = kj::none};
  }

 private:
  bool& ordinaryDispatchCalled;
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata>& capturedMetadata;
};

enum class ReplayFailure {
  AMBIGUOUS,
  NOT_DELIVERED,
  DELIVERED,
  CLAIM_REJECTED,
  HANG,
};

struct ReplayState {
  kj::Array<ReplayFailure> failures;
  bool acceptWebSocket = false;
  kj::Maybe<kj::Own<kj::WebSocket>> acceptedWebSocket;
  kj::Vector<IoChannelFactory::ActorRetryRequestMetadata> metadata;
  kj::Vector<kj::Array<kj::byte>> requestBodies;
  uint requestCount = 0;
  uint webSocketRequestCount = 0;
  uint retryCount = 0;
};

class ReplayFetchTarget final: public WorkerInterface {
 public:
  ReplayFetchTarget(ReplayState& state): state(state) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    auto attempt = state.requestCount++;
    if (headers.isWebSocket()) {
      ++state.webSocketRequestCount;
    }
    state.requestBodies.add(co_await requestBody.readAllBytes());
    if (attempt < state.failures.size()) {
      auto failure = state.failures[attempt];
      if (failure == ReplayFailure::HANG) {
        co_await kj::Promise<void>(kj::NEVER_DONE);
        KJ_UNREACHABLE;
      }
      auto exception = failure == ReplayFailure::CLAIM_REJECTED
          ? KJ_EXCEPTION(FAILED, "actor retry claim rejected")
          : KJ_EXCEPTION(DISCONNECTED, "actor fetch disconnected");
      if (failure == ReplayFailure::NOT_DELIVERED) {
        exception.setDetail(
            jsg::REQUEST_NOT_DELIVERED_TO_ACTOR_DETAIL_ID, kj::heapArray<kj::byte>(0));
      } else if (failure == ReplayFailure::DELIVERED) {
        exception.setDetail(jsg::REQUEST_DELIVERED_TO_ACTOR_DETAIL_ID, kj::heapArray<kj::byte>(0));
      } else if (failure == ReplayFailure::CLAIM_REJECTED) {
        exception.setDetail(jsg::ACTOR_RETRY_CLAIM_REJECTED_DETAIL_ID, kj::heapArray<kj::byte>(0));
      }
      kj::throwRecoverableException(kj::mv(exception));
    }

    auto responseHeaders = headers.cloneShallow();
    responseHeaders.clear();
    if (headers.isWebSocket() && state.acceptWebSocket) {
      state.acceptedWebSocket = response.acceptWebSocket(responseHeaders);
      co_return;
    }
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

 private:
  ReplayState& state;
};

class ReplayOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  ReplayOutgoingFactory(ReplayState& state): state(state) {}

  Result newSingleUseClient(kj::Maybe<kj::String>, MakeUserSpanParent) override {
    KJ_FAIL_ASSERT("replay tests should always supply actor retry metadata");
  }

  bool supportsActorFetchRetries() const override {
    return true;
  }

  void onActorFetchRetry() override {
    ++state.retryCount;
  }

  Result newSingleUseClientWithActorRetryMetadata(kj::Maybe<kj::String>,
      kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> actorRetryRequestMetadata,
      MakeUserSpanParent) override {
    state.metadata.add(KJ_REQUIRE_NONNULL(actorRetryRequestMetadata));
    return {.client = kj::heap<ReplayFetchTarget>(state), .spanParents = kj::none};
  }

 private:
  ReplayState& state;
};

enum class RetryEnforcement {
  DISABLED,
  ENABLED,
};

enum class ActorFetchKind {
  HTTP,
  WEB_SOCKET,
};

kj::Maybe<kj::Exception> runActorFetch(ReplayState& state,
    RetryEnforcement enforcement,
    kj::Maybe<kj::StringPtr> body,
    ActorFetchKind kind) {
  TestFixture fixture(TestFixture::SetupParams{
    .useRealTimers = enforcement == RetryEnforcement::ENABLED,
  });
  if (enforcement == RetryEnforcement::ENABLED) {
    util::Autogate::initAutogateNamesForTest(
        {"durable-object-retries-fetch"_kj, "durable-object-retries-fetch-retry-requests"_kj},
        util::IgnoreAllAutogatesEnv::YES);
  } else {
    util::Autogate::initAutogateNamesForTest(
        {"durable-object-retries-fetch"_kj}, util::IgnoreAllAutogatesEnv::YES);
  }
  kj::Maybe<kj::Exception> failure;

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = env.js.alloc<Fetcher>(
        env.context.addObject<Fetcher::OutgoingFactory>(kj::heap<ReplayOutgoingFactory>(state)),
        Fetcher::RequiresHostAndProtocol::YES);
    RequestInitializerDict init;
    KJ_IF_SOME(value, body) {
      init.method = kj::str("POST");
      init.body = kj::Maybe<Body::Initializer>(kj::str(value));
    }
    if (kind == ActorFetchKind::WEB_SOCKET) {
      jsg::Dict<kj::String, kj::String> headers;
      headers.fields = kj::heapArray<jsg::Dict<kj::String, kj::String>::Field>(1);
      headers.fields[0].name = kj::str("Upgrade");
      headers.fields[0].value = kj::str("websocket");
      init.headers = kj::mv(headers);
    }
    auto promise = fetcher->fetch(env.js, kj::str("http://example.com"), kj::mv(init));
    return env.context.awaitJs(env.js, kj::mv(promise))
        .ignoreResult()
        .catch_([&](kj::Exception&& exception) {
      failure.emplace(kj::mv(exception));
    }).attach(kj::mv(fetcher));
  });

  return failure;
}

class UnsupportedOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  UnsupportedOutgoingFactory(bool& called): called(called) {}

  Result newSingleUseClient(kj::Maybe<kj::String>, MakeUserSpanParent) override {
    called = true;
    return {.client = kj::heap<MockFetchTarget>(), .spanParents = kj::none};
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

class ReplayActorChannel final: public IoChannelFactory::ActorChannel {
 public:
  ReplayActorChannel(ReplayState& state): state(state) {}

  kj::Own<WorkerInterface> startRequest(IoChannelFactory::SubrequestMetadata metadata) override {
    KJ_IF_SOME(retryMetadata, metadata.actorRetryRequestMetadata) {
      state.metadata.add(kj::mv(retryMetadata));
    }
    return kj::heap<ReplayFetchTarget>(state);
  }

  void requireAllowsTransfer() override {
    KJ_UNIMPLEMENTED("not used in this test");
  }

  kj::OneOf<kj::Array<byte>, kj::Promise<kj::Array<byte>>> getTokenMaybeSync(
      IoChannelFactory::ChannelTokenUsage) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }

 private:
  ReplayState& state;
};

struct ActorIoChannelFactory final: public TestFixture::DummyIoChannelFactory {
  ActorIoChannelFactory(TimerChannel& timer,
      kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata>& capturedMetadata,
      uint& channelCount,
      kj::Vector<kj::String>& locationHints,
      kj::Vector<kj::String>& cohorts)
      : DummyIoChannelFactory(timer),
        capturedMetadata(capturedMetadata),
        channelCount(channelCount),
        locationHints(locationHints),
        cohorts(cohorts) {}

  kj::Own<ActorChannel> getGlobalActor(uint,
      const ActorIdFactory::ActorId&,
      kj::Maybe<kj::String> locationHint,
      ActorGetMode,
      bool,
      ActorRoutingMode,
      SpanParent,
      kj::Maybe<ActorVersion> version,
      Persistent) override {
    ++channelCount;
    KJ_IF_SOME(hint, locationHint) {
      locationHints.add(kj::mv(hint));
    }
    KJ_IF_SOME(v, version) {
      KJ_IF_SOME(cohort, v.cohort) {
        cohorts.add(kj::mv(cohort));
      }
    }
    return kj::refcounted<RecordingActorChannel>(capturedMetadata);
  }

  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata>& capturedMetadata;
  uint& channelCount;
  kj::Vector<kj::String>& locationHints;
  kj::Vector<kj::String>& cohorts;
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

KJ_TEST("actor fetch updates retry metadata and rewinds the body") {
  ReplayState state{.failures = kj::arr(ReplayFailure::NOT_DELIVERED, ReplayFailure::AMBIGUOUS,
                        ReplayFailure::NOT_DELIVERED)};
  KJ_EXPECT(runActorFetch(state, RetryEnforcement::ENABLED, "request body"_kj,
                ActorFetchKind::HTTP) == kj::none);

  KJ_ASSERT(state.metadata.size() == 4);
  KJ_EXPECT(state.requestCount == 4);
  KJ_EXPECT(state.retryCount == 3);
  KJ_EXPECT(state.metadata[0].nonce != state.metadata[1].nonce);
  KJ_EXPECT(state.metadata[1].nonce == state.metadata[2].nonce);
  KJ_EXPECT(state.metadata[1].nonce == state.metadata[3].nonce);
  KJ_EXPECT(state.metadata[0].isRetry == IsActorRetry::NO);
  KJ_EXPECT(state.metadata[1].isRetry == IsActorRetry::NO);
  KJ_EXPECT(state.metadata[2].isRetry == IsActorRetry::YES);
  KJ_EXPECT(state.metadata[3].isRetry == IsActorRetry::YES);
  KJ_ASSERT(state.requestBodies.size() == 4);
  for (auto& body: state.requestBodies) {
    KJ_EXPECT(body == "request body"_kj.asBytes());
  }
}

KJ_TEST("actor fetch does not retry when enforcement is disabled") {
  ReplayState state{.failures = kj::arr(ReplayFailure::AMBIGUOUS)};

  KJ_EXPECT(
      runActorFetch(state, RetryEnforcement::DISABLED, kj::none, ActorFetchKind::HTTP) != kj::none);
  KJ_EXPECT(state.requestCount == 1);
  KJ_EXPECT(state.retryCount == 0);
}

KJ_TEST("actor fetch does not retry a delivered disconnect") {
  ReplayState state{.failures = kj::arr(ReplayFailure::DELIVERED)};

  KJ_EXPECT(
      runActorFetch(state, RetryEnforcement::ENABLED, kj::none, ActorFetchKind::HTTP) != kj::none);
  KJ_EXPECT(state.requestCount == 1);
  KJ_EXPECT(state.retryCount == 0);
}

KJ_TEST("actor fetch stops after a retry claim rejection") {
  ReplayState state{
    .failures = kj::arr(ReplayFailure::AMBIGUOUS, ReplayFailure::CLAIM_REJECTED),
  };
  auto failure = KJ_REQUIRE_NONNULL(
      runActorFetch(state, RetryEnforcement::ENABLED, kj::none, ActorFetchKind::HTTP));

  KJ_EXPECT(failure.getType() == kj::Exception::Type::DISCONNECTED, failure);
  KJ_EXPECT(!failure.getDescription().contains("claim rejected"), failure);
  KJ_EXPECT(state.requestCount == 2);
  KJ_EXPECT(state.retryCount == 1);
}

KJ_TEST("actor fetch normalizes an initial retry claim rejection") {
  ReplayState state{.failures = kj::arr(ReplayFailure::CLAIM_REJECTED)};
  auto failure = KJ_REQUIRE_NONNULL(
      runActorFetch(state, RetryEnforcement::ENABLED, kj::none, ActorFetchKind::HTTP));

  KJ_EXPECT(failure.getType() == kj::Exception::Type::DISCONNECTED, failure);
  KJ_EXPECT(!failure.getDescription().contains("claim rejected"), failure);
  KJ_EXPECT(state.requestCount == 1);
  KJ_EXPECT(state.retryCount == 0);
}

KJ_TEST("actor WebSocket fetch retries a disconnected handshake") {
  ReplayState state{
    .failures = kj::arr(ReplayFailure::AMBIGUOUS),
    .acceptWebSocket = true,
  };

  KJ_EXPECT(runActorFetch(state, RetryEnforcement::ENABLED, kj::none, ActorFetchKind::WEB_SOCKET) ==
      kj::none);
  KJ_EXPECT(state.requestCount == 2);
  KJ_EXPECT(state.webSocketRequestCount == 2);
  KJ_EXPECT(state.retryCount == 1);
  KJ_EXPECT(state.acceptedWebSocket != kj::none);
}

KJ_TEST("actor fetch honors an abort before retrying") {
  ReplayState state{.failures = kj::arr(ReplayFailure::AMBIGUOUS)};
  kj::Maybe<kj::Exception> failure;
  TestFixture fixture(TestFixture::SetupParams{
    .autogates = kj::arr<kj::StringPtr>(
        "durable-object-retries-fetch"_kj, "durable-object-retries-fetch-retry-requests"_kj),
    .useRealTimers = true,
  });

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = env.js.alloc<Fetcher>(
        env.context.addObject<Fetcher::OutgoingFactory>(kj::heap<ReplayOutgoingFactory>(state)),
        Fetcher::RequiresHostAndProtocol::YES);
    auto controller = AbortController::constructor(env.js);
    RequestInitializerDict init;
    init.signal = kj::Maybe(controller->getSignal());
    auto promise = fetcher->fetch(env.js, kj::str("http://example.com"), kj::mv(init));
    controller->abort(env.js, kj::none);
    return env.context.awaitJs(env.js, kj::mv(promise))
        .ignoreResult()
        .catch_([&](kj::Exception&& exception) {
      failure.emplace(kj::mv(exception));
    }).attach(kj::mv(fetcher), kj::mv(controller));
  });

  auto& exception = KJ_REQUIRE_NONNULL(failure);
  KJ_EXPECT(exception.getDescription().contains("The operation was aborted"), exception);
  KJ_EXPECT(state.requestCount == 1);
  KJ_EXPECT(state.retryCount == 0);
}

KJ_TEST("actor fetch stops after five attempts") {
  ReplayState state{
    .failures = kj::arr(ReplayFailure::AMBIGUOUS, ReplayFailure::AMBIGUOUS,
        ReplayFailure::AMBIGUOUS, ReplayFailure::AMBIGUOUS, ReplayFailure::AMBIGUOUS),
  };

  KJ_EXPECT(
      runActorFetch(state, RetryEnforcement::ENABLED, kj::none, ActorFetchKind::HTTP) != kj::none);
  KJ_EXPECT(state.requestCount == 5);
  KJ_EXPECT(state.retryCount == 4);
}

KJ_TEST("actor fetch stops when the retry budget expires") {
  ReplayState state{
    .failures = kj::arr(ReplayFailure::AMBIGUOUS, ReplayFailure::HANG),
  };

  KJ_EXPECT(
      runActorFetch(state, RetryEnforcement::ENABLED, kj::none, ActorFetchKind::HTTP) != kj::none);
  KJ_EXPECT(state.requestCount == 2);
  KJ_EXPECT(state.retryCount == 1);
}

KJ_TEST("replica actor fetch does not retry a disconnected primary channel") {
  ReplayState state{.failures = kj::arr(ReplayFailure::NOT_DELIVERED)};
  uint checkedSubrequestCount = 0;
  kj::Maybe<kj::Exception> failure;
  TestFixture fixture(TestFixture::SetupParams{
    .useRealTimers = true,
    .checkedSubrequestCount = checkedSubrequestCount,
  });
  util::Autogate::initAutogateNamesForTest(
      {"durable-object-retries-fetch"_kj, "durable-object-retries-fetch-retry-requests"_kj},
      util::IgnoreAllAutogatesEnv::YES);

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = env.js.alloc<Fetcher>(
        env.context.addObject<Fetcher::OutgoingFactory>(kj::heap<ReplicaActorOutgoingFactory>(
            kj::refcounted<ReplayActorChannel>(state), kj::str("actor-id"))),
        Fetcher::RequiresHostAndProtocol::YES);
    auto promise = fetcher->fetch(env.js, kj::str("http://example.com"), kj::none);
    return env.context.awaitJs(env.js, kj::mv(promise))
        .ignoreResult()
        .catch_([&](kj::Exception&& exception) {
      failure.emplace(kj::mv(exception));
    }).attach(kj::mv(fetcher));
  });

  KJ_EXPECT(failure != kj::none);
  KJ_EXPECT(state.requestCount == 1);
  KJ_EXPECT(state.metadata.empty());
  KJ_EXPECT(checkedSubrequestCount == 1);
}

KJ_TEST("GlobalActorOutgoingFactory forwards metadata and recreates channels for retries") {
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> capturedMetadata;
  uint channelCount = 0;
  uint checkedSubrequestCount = 0;
  kj::Vector<kj::String> locationHints;
  kj::Vector<kj::String> cohorts;
  TestFixture fixture(TestFixture::SetupParams{
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<ActorIoChannelFactory>(
        timer, capturedMetadata, channelCount, locationHints, cohorts);
  }),
    .checkedSubrequestCount = checkedSubrequestCount,
  });

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    GlobalActorOutgoingFactory factory(
        GlobalActorOutgoingFactory::ChannelIdOrFactory(static_cast<uint>(1)),
        env.js.alloc<DurableObjectId>(kj::heap<MockActorId>()), kj::str("location"),
        ActorGetMode::GET_OR_CREATE, false, ActorRoutingMode::DEFAULT,
        ActorVersion{.cohort = kj::str("cohort")}, Persistent::NO);
    KJ_EXPECT(factory.supportsActorFetchRetries());

    auto client = factory.newSingleUseClientWithActorRetryMetadata(kj::none,
        IoChannelFactory::ActorRetryRequestMetadata{
          .nonce = 0x123456789abcdef0,
          .createdAt = kj::UNIX_EPOCH + 123 * kj::MILLISECONDS,
          .isRetry = IsActorRetry::YES,
        },
        [](TraceContext&) -> kj::Maybe<SpanParent> { return kj::none; });

    KJ_IF_SOME(metadata, capturedMetadata) {
      KJ_EXPECT(metadata.nonce == 0x123456789abcdef0);
      KJ_EXPECT(metadata.createdAt == kj::UNIX_EPOCH + 123 * kj::MILLISECONDS);
      KJ_EXPECT(metadata.isRetry == IsActorRetry::YES);
    } else {
      KJ_FAIL_EXPECT("actor retry metadata was not forwarded to the actor channel");
    }

    factory.onActorFetchRetry();
    auto retryClient = factory.newSingleUseClientWithActorRetryMetadata(kj::none,
        IoChannelFactory::ActorRetryRequestMetadata{
          .nonce = 0xfedcba9876543210,
          .createdAt = kj::UNIX_EPOCH + 456 * kj::MILLISECONDS,
          .isRetry = IsActorRetry::YES,
        },
        [](TraceContext&) -> kj::Maybe<SpanParent> { return kj::none; });
    KJ_EXPECT(checkedSubrequestCount == 2);
    KJ_EXPECT(channelCount == 2);
    KJ_ASSERT(locationHints.size() == 2);
    KJ_EXPECT(locationHints[0] == "location");
    KJ_EXPECT(locationHints[1] == "location");
    KJ_ASSERT(cohorts.size() == 2);
    KJ_EXPECT(cohorts[0] == "cohort");
    KJ_EXPECT(cohorts[1] == "cohort");
  });
}

}  // namespace
}  // namespace workerd::api
