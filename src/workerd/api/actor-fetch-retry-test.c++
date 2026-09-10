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

class RecordingRequestObserver final: public RequestObserver {
 public:
  explicit RecordingRequestObserver(kj::Vector<CountSubrequest>& countSubrequests)
      : countSubrequests(countSubrequests) {}

  kj::Own<WorkerInterface> wrapSubrequestClient(
      kj::Own<WorkerInterface> client, CountSubrequest countSubrequest) override {
    countSubrequests.add(countSubrequest);
    return kj::mv(client);
  }

 private:
  kj::Vector<CountSubrequest>& countSubrequests;
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
      CountSubrequest,
      MakeUserSpanParent) override {
    capturedMetadata = kj::mv(actorRetryRequestMetadata);
    return {.client = kj::heap<MockFetchTarget>(), .spanParents = kj::none};
  }

 private:
  bool& ordinaryDispatchCalled;
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata>& capturedMetadata;
};

enum class ReplayAction {
  AMBIGUOUS,
  NOT_DELIVERED,
  DELIVERED,
  CLAIM_REJECTED,
  PREDECESSOR_REJECTED,
  NON_RETRYABLE_FAILURE,
  SLOW_RESPONSE,
  PAUSE_RESPONSE,
  PAUSE_NEXT_RETRY,
  RETRY_DELAY_EXCEEDS_BUDGET,
  CLIENT_CREATION_FAILURE,
  CLIENT_CREATION_DISCONNECT,
};

kj::Exception makeReplayFailure(ReplayAction action) {
  auto exception = [&]() -> kj::Exception {
    switch (action) {
      case ReplayAction::CLAIM_REJECTED:
        return KJ_EXCEPTION(FAILED, "actor retry claim rejected");
      case ReplayAction::NON_RETRYABLE_FAILURE:
        return JSG_KJ_EXCEPTION(FAILED, Error, "actor fetch failed");
      default:
        return KJ_EXCEPTION(DISCONNECTED, "actor fetch disconnected");
    }
  }();

  switch (action) {
    case ReplayAction::NOT_DELIVERED:
      jsg::markActorRequestNotDelivered(exception);
      break;
    case ReplayAction::DELIVERED:
      exception.setDetail(jsg::REQUEST_DELIVERED_TO_ACTOR_DETAIL_ID, kj::heapArray<kj::byte>(0));
      break;
    case ReplayAction::CLAIM_REJECTED:
      exception.setDetail(jsg::ACTOR_RETRY_CLAIM_REJECTED_DETAIL_ID, kj::heapArray<kj::byte>(0));
      break;
    case ReplayAction::PREDECESSOR_REJECTED:
      jsg::markActorRequestNotDelivered(exception);
      exception.setDetail(jsg::ACTOR_PREDECESSOR_REJECTED_DETAIL_ID, kj::heapArray<kj::byte>(0));
      break;
    default:
      break;
  }
  return exception;
}

class DeterministicTimerChannel final: public TimerChannel {
 public:
  explicit DeterministicTimerChannel(kj::TimerImpl& timer): timer(timer) {}

  void syncTime() override {}

  kj::Date now(kj::Maybe<kj::Date>) override {
    return kj::UNIX_EPOCH + (timer.now() - kj::origin<kj::TimePoint>());
  }

  kj::Promise<void> atTime(kj::Date when) override {
    auto target = kj::origin<kj::TimePoint>() + (when - kj::UNIX_EPOCH);
    if (target <= timer.now()) {
      return kj::READY_NOW;
    }
    auto promise = timer.atTime(target);
    timer.advanceTo(target);
    return promise;
  }

  kj::Promise<void> afterLimitTimeout(kj::Duration delay) override {
    auto promise = timer.afterDelay(delay);
    auto startedFulfiller = kj::mv(nextTimeoutStartedFulfiller);
    KJ_IF_SOME(f, startedFulfiller) {
      f->fulfill();
      return kj::NEVER_DONE;
    }
    timer.advanceTo(timer.now() + nextDelay.orDefault(delay));
    nextDelay = kj::none;
    return promise;
  }

  void delayNextTimeoutBy(kj::Duration delay) {
    nextDelay = delay;
  }

  void pauseNextTimeout(kj::Own<kj::PromiseFulfiller<void>> startedFulfiller) {
    nextTimeoutStartedFulfiller = kj::mv(startedFulfiller);
  }

  kj::TimePoint nowForLimitTimeout() override {
    return timer.now();
  }

 private:
  kj::TimerImpl& timer;
  kj::Maybe<kj::Duration> nextDelay;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> nextTimeoutStartedFulfiller;
};

struct ReplayState {
  kj::Array<ReplayAction> actions;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> pauseStartedFulfiller;
  bool acceptWebSocket = false;
  kj::Maybe<kj::Own<kj::WebSocket>> acceptedWebSocket;
  kj::Vector<IoChannelFactory::ActorRetryRequestMetadata> metadata;
  kj::Vector<kj::Array<kj::byte>> requestBodies;
  kj::Vector<CountSubrequest> countSubrequests;
  uint requestCount = 0;
  uint webSocketRequestCount = 0;
  uint retryCount = 0;
  uint observedRetryCount = 0;
  kj::Vector<ActorRetryOutcome> outcomes;
  kj::Maybe<DeterministicTimerChannel&> timerChannel;
};

class RetryRecordingObserver final: public RequestObserver {
 public:
  explicit RetryRecordingObserver(ReplayState& state): state(state) {}

  void recordActorRetry(ActorRetryCallType callType) override {
    KJ_EXPECT(callType == ActorRetryCallType::FETCH);
    ++state.observedRetryCount;
  }

  void recordActorRetryOutcome(
      ActorRetryCallType callType, ActorRetryOutcome outcome, kj::Duration) override {
    KJ_EXPECT(callType == ActorRetryCallType::FETCH);
    state.outcomes.add(outcome);
  }

 private:
  ReplayState& state;
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
    kj::Maybe<kj::Promise<void>> slowResponseDelay;
    if (attempt < state.actions.size() && state.actions[attempt] == ReplayAction::SLOW_RESPONSE) {
      slowResponseDelay = IoContext::current().afterLimitTimeout(11 * kj::SECONDS);
    }
    if (headers.isWebSocket()) {
      ++state.webSocketRequestCount;
    }
    state.requestBodies.add(co_await requestBody.readAllBytes());
    if (attempt < state.actions.size()) {
      auto action = state.actions[attempt];
      switch (action) {
        case ReplayAction::SLOW_RESPONSE:
          co_await kj::mv(KJ_ASSERT_NONNULL(slowResponseDelay));
          break;
        case ReplayAction::PAUSE_RESPONSE:
          KJ_REQUIRE_NONNULL(state.pauseStartedFulfiller)->fulfill();
          co_await kj::Promise<void>(kj::NEVER_DONE);
          break;
        case ReplayAction::PAUSE_NEXT_RETRY:
          KJ_REQUIRE_NONNULL(state.timerChannel)
              .pauseNextTimeout(kj::mv(KJ_REQUIRE_NONNULL(state.pauseStartedFulfiller)));
          [[fallthrough]];
        case ReplayAction::AMBIGUOUS:
        case ReplayAction::NOT_DELIVERED:
        case ReplayAction::DELIVERED:
        case ReplayAction::CLAIM_REJECTED:
        case ReplayAction::PREDECESSOR_REJECTED:
        case ReplayAction::NON_RETRYABLE_FAILURE:
        case ReplayAction::CLIENT_CREATION_FAILURE:
        case ReplayAction::CLIENT_CREATION_DISCONNECT:
          kj::throwRecoverableException(makeReplayFailure(action));
          break;
        case ReplayAction::RETRY_DELAY_EXCEEDS_BUDGET:
          KJ_REQUIRE_NONNULL(state.timerChannel).delayNextTimeoutBy(11 * kj::SECONDS);
          kj::throwRecoverableException(makeReplayFailure(action));
          break;
      }
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
    return {.client = kj::heap<ReplayFetchTarget>(state), .spanParents = kj::none};
  }

  bool supportsActorFetchRetries() const override {
    return true;
  }

  void onActorFetchRetry() override {
    ++state.retryCount;
  }

  Result newSingleUseClientWithActorRetryMetadata(kj::Maybe<kj::String>,
      kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> actorRetryRequestMetadata,
      CountSubrequest countSubrequest,
      MakeUserSpanParent) override {
    state.metadata.add(KJ_REQUIRE_NONNULL(actorRetryRequestMetadata));
    state.countSubrequests.add(countSubrequest);
    if (state.requestCount < state.actions.size()) {
      auto action = state.actions[state.requestCount];
      if (action == ReplayAction::CLIENT_CREATION_FAILURE) {
        kj::throwRecoverableException(
            JSG_KJ_EXCEPTION(FAILED, Error, "actor client creation failed"));
      } else if (action == ReplayAction::CLIENT_CREATION_DISCONNECT) {
        kj::throwRecoverableException(KJ_EXCEPTION(DISCONNECTED, "actor client creation failed"));
      }
    }
    return {.client = kj::heap<ReplayFetchTarget>(state), .spanParents = kj::none};
  }

 private:
  ReplayState& state;
};

enum class ActorFetchKind {
  HTTP,
  WEB_SOCKET,
};

enum class ActorFetchBodyKind {
  NULL_OR_BUFFERED,
  STREAMING,
};

kj::Maybe<kj::Exception> runActorFetch(ReplayState& state,
    ActorRetryGateEnabled retryGateEnabled,
    kj::Maybe<kj::StringPtr> body,
    ActorFetchKind kind,
    ActorFetchBodyKind bodyKind = ActorFetchBodyKind::NULL_OR_BUFFERED) {
  kj::TimerImpl timer(kj::origin<kj::TimePoint>());
  DeterministicTimerChannel timerChannel(timer);
  state.timerChannel = timerChannel;
  TestFixture fixture(TestFixture::SetupParams{
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel&) -> kj::Rc<IoChannelFactory> {
    return kj::rc<TestFixture::DummyIoChannelFactory>(timerChannel);
  }),
    .requestObserverFactory =
        kj::Function<kj::Own<RequestObserver>()>([&]() -> kj::Own<RequestObserver> {
    return kj::refcounted<RetryRecordingObserver>(state);
  }),
  });
  if (retryGateEnabled.toBool()) {
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
    if (bodyKind == ActorFetchBodyKind::STREAMING) {
      init.method = kj::str("POST");
      init.body = kj::Maybe<Body::Initializer>(
          JsReadableStream::create(env.js, env.context, kj::heap<TestStreamSource>()));
    } else KJ_IF_SOME(value, body) {
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

void runActorFetchUntilCanceled(ReplayState& state, kj::Promise<void> pauseStarted) {
  kj::TimerImpl timer(kj::origin<kj::TimePoint>());
  DeterministicTimerChannel timerChannel(timer);
  state.timerChannel = timerChannel;
  TestFixture fixture(TestFixture::SetupParams{
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel&) -> kj::Rc<IoChannelFactory> {
    return kj::rc<TestFixture::DummyIoChannelFactory>(timerChannel);
  }),
    .requestObserverFactory =
        kj::Function<kj::Own<RequestObserver>()>([&]() -> kj::Own<RequestObserver> {
    return kj::refcounted<RetryRecordingObserver>(state);
  }),
  });
  util::Autogate::initAutogateNamesForTest(
      {"durable-object-retries-fetch"_kj, "durable-object-retries-fetch-retry-requests"_kj},
      util::IgnoreAllAutogatesEnv::YES);

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = env.js.alloc<Fetcher>(
        env.context.addObject<Fetcher::OutgoingFactory>(kj::heap<ReplayOutgoingFactory>(state)),
        Fetcher::RequiresHostAndProtocol::YES);
    auto controller = AbortController::constructor(env.js);
    auto abortPromise = env.context.awaitJs(env.js,
        env.context.awaitIo(env.js, kj::mv(pauseStarted))
            .then(env.js, [controller = controller.addRef()](jsg::Lock& js) mutable {
      controller->abort(js, kj::none);
    }));
    RequestInitializerDict init;
    init.signal = kj::Maybe(controller->getSignal());
    auto promise = fetcher->fetch(env.js, kj::str("http://example.com"), kj::mv(init));
    auto fetchPromise =
        env.context.awaitJs(env.js, kj::mv(promise)).ignoreResult().catch_([](kj::Exception&&) {});
    return kj::joinPromises(kj::arr(kj::mv(fetchPromise), kj::mv(abortPromise)))
        .attach(kj::mv(fetcher), kj::mv(controller));
  });
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
  util::Autogate::initAutogateNamesForTest(
      {"durable-object-retries-fetch"_kj}, util::IgnoreAllAutogatesEnv::YES);

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
    KJ_EXPECT(metadata.retryGateEnabled == ActorRetryGateEnabled::NO);
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
  ReplayState state{.actions = kj::arr(ReplayAction::NOT_DELIVERED, ReplayAction::AMBIGUOUS,
                        ReplayAction::NOT_DELIVERED)};
  KJ_EXPECT(runActorFetch(state, ActorRetryGateEnabled::YES, "request body"_kj,
                ActorFetchKind::HTTP) == kj::none);

  KJ_ASSERT(state.metadata.size() == 4);
  KJ_EXPECT(state.requestCount == 4);
  KJ_EXPECT(state.retryCount == 3);
  KJ_EXPECT(state.observedRetryCount == 3);
  KJ_ASSERT(state.outcomes.size() == 1);
  KJ_EXPECT(state.outcomes[0] == ActorRetryOutcome::RECOVERED);
  KJ_EXPECT(state.metadata[0].nonce != state.metadata[1].nonce);
  KJ_EXPECT(state.metadata[1].nonce == state.metadata[2].nonce);
  KJ_EXPECT(state.metadata[1].nonce == state.metadata[3].nonce);
  KJ_EXPECT(state.metadata[0].isRetry == IsActorRetry::NO);
  KJ_EXPECT(state.metadata[1].isRetry == IsActorRetry::NO);
  KJ_EXPECT(state.metadata[2].isRetry == IsActorRetry::YES);
  KJ_EXPECT(state.metadata[3].isRetry == IsActorRetry::YES);
  for (auto& metadata: state.metadata) {
    KJ_EXPECT(metadata.retryGateEnabled == ActorRetryGateEnabled::YES);
  }
  KJ_ASSERT(state.countSubrequests.size() == 4);
  KJ_EXPECT(state.countSubrequests[0] == CountSubrequest::YES);
  KJ_EXPECT(state.countSubrequests[1] == CountSubrequest::NO);
  KJ_EXPECT(state.countSubrequests[2] == CountSubrequest::NO);
  KJ_EXPECT(state.countSubrequests[3] == CountSubrequest::NO);
  KJ_ASSERT(state.requestBodies.size() == 4);
  for (auto& body: state.requestBodies) {
    KJ_EXPECT(body == "request body"_kj.asBytes());
  }
}

KJ_TEST("actor fetch retries a predecessor rejection as a fresh attempt") {
  ReplayState state{.actions = kj::arr(ReplayAction::PREDECESSOR_REJECTED)};
  KJ_EXPECT(runActorFetch(state, ActorRetryGateEnabled::YES, "request body"_kj,
                ActorFetchKind::HTTP) == kj::none);

  KJ_EXPECT(state.requestCount == 2);
  KJ_EXPECT(state.retryCount == 1);
  KJ_ASSERT(state.metadata.size() == 2);
  KJ_EXPECT(state.metadata[0].nonce != state.metadata[1].nonce);
  KJ_EXPECT(state.metadata[0].isRetry == IsActorRetry::NO);
  KJ_EXPECT(state.metadata[1].isRetry == IsActorRetry::NO);
  KJ_ASSERT(state.countSubrequests.size() == 2);
  KJ_EXPECT(state.countSubrequests[0] == CountSubrequest::YES);
  KJ_EXPECT(state.countSubrequests[1] == CountSubrequest::NO);
  KJ_ASSERT(state.requestBodies.size() == 2);
  KJ_EXPECT(state.requestBodies[0] == "request body"_kj.asBytes());
  KJ_EXPECT(state.requestBodies[1] == "request body"_kj.asBytes());
}

KJ_TEST("actor fetch does not retry when the enforce gate is disabled") {
  ReplayState state{.actions = kj::arr(ReplayAction::AMBIGUOUS)};

  KJ_EXPECT(
      runActorFetch(state, ActorRetryGateEnabled::NO, kj::none, ActorFetchKind::HTTP) != kj::none);
  KJ_EXPECT(state.requestCount == 1);
  KJ_EXPECT(state.retryCount == 0);
  KJ_ASSERT(state.metadata.size() == 1);
  KJ_EXPECT(state.metadata[0].retryGateEnabled == ActorRetryGateEnabled::NO);
  KJ_EXPECT(state.observedRetryCount == 0);
  KJ_EXPECT(state.outcomes.size() == 0);
}

KJ_TEST("actor fetch does not report an ordinary failure when retries are disabled") {
  ReplayState state{.actions = kj::arr(ReplayAction::NON_RETRYABLE_FAILURE)};

  KJ_EXPECT(
      runActorFetch(state, ActorRetryGateEnabled::NO, kj::none, ActorFetchKind::HTTP) != kj::none);
  KJ_EXPECT(state.observedRetryCount == 0);
  KJ_EXPECT(state.outcomes.size() == 0);
}

KJ_TEST("actor fetch does not report an outcome when a streaming body cannot retry") {
  ReplayState state{.actions = kj::arr(ReplayAction::AMBIGUOUS)};

  KJ_EXPECT(runActorFetch(state, ActorRetryGateEnabled::YES, kj::none, ActorFetchKind::HTTP,
                ActorFetchBodyKind::STREAMING) != kj::none);
  KJ_EXPECT(state.observedRetryCount == 0);
  KJ_EXPECT(state.outcomes.size() == 0);
}

KJ_TEST("actor fetch reports unable to retry after an ordinary failure") {
  ReplayState state{
    .actions = kj::arr(ReplayAction::AMBIGUOUS, ReplayAction::NON_RETRYABLE_FAILURE),
  };

  KJ_EXPECT(
      runActorFetch(state, ActorRetryGateEnabled::YES, kj::none, ActorFetchKind::HTTP) != kj::none);
  KJ_EXPECT(state.requestCount == 2);
  KJ_EXPECT(state.retryCount == 1);
  KJ_EXPECT(state.observedRetryCount == 1);
  KJ_ASSERT(state.outcomes.size() == 1);
  KJ_EXPECT(state.outcomes[0] == ActorRetryOutcome::UNABLE_TO_RETRY);
}

KJ_TEST("actor fetch reports a client creation failure after retrying") {
  ReplayState state{
    .actions = kj::arr(ReplayAction::AMBIGUOUS, ReplayAction::CLIENT_CREATION_FAILURE),
  };

  auto failure = KJ_REQUIRE_NONNULL(
      runActorFetch(state, ActorRetryGateEnabled::YES, kj::none, ActorFetchKind::HTTP));
  KJ_EXPECT(failure.getDescription().contains("actor client creation failed"), failure);
  KJ_EXPECT(state.requestCount == 1);
  KJ_EXPECT(state.retryCount == 1);
  KJ_EXPECT(state.observedRetryCount == 1);
  KJ_ASSERT(state.outcomes.size() == 1);
  KJ_EXPECT(state.outcomes[0] == ActorRetryOutcome::OTHER);
}

KJ_TEST("actor fetch does not retry an initial client creation disconnect") {
  ReplayState state{
    .actions = kj::arr(ReplayAction::CLIENT_CREATION_DISCONNECT),
  };

  auto failure = KJ_REQUIRE_NONNULL(
      runActorFetch(state, ActorRetryGateEnabled::YES, kj::none, ActorFetchKind::HTTP));
  KJ_EXPECT(failure.getType() == kj::Exception::Type::DISCONNECTED, failure);
  KJ_EXPECT(state.requestCount == 0);
  KJ_EXPECT(state.retryCount == 0);
  KJ_EXPECT(state.observedRetryCount == 0);
  KJ_EXPECT(state.outcomes.size() == 0);
}

KJ_TEST("actor fetch does not retry a delivered disconnect") {
  ReplayState state{.actions = kj::arr(ReplayAction::DELIVERED)};

  KJ_EXPECT(
      runActorFetch(state, ActorRetryGateEnabled::YES, kj::none, ActorFetchKind::HTTP) != kj::none);
  KJ_EXPECT(state.requestCount == 1);
  KJ_EXPECT(state.retryCount == 0);
  KJ_EXPECT(state.outcomes.size() == 0);
}

KJ_TEST("actor fetch reports unable to retry after a delivered retry disconnect") {
  ReplayState state{
    .actions = kj::arr(ReplayAction::AMBIGUOUS, ReplayAction::DELIVERED),
  };

  KJ_EXPECT(
      runActorFetch(state, ActorRetryGateEnabled::YES, kj::none, ActorFetchKind::HTTP) != kj::none);
  KJ_EXPECT(state.requestCount == 2);
  KJ_EXPECT(state.retryCount == 1);
  KJ_EXPECT(state.observedRetryCount == 1);
  KJ_ASSERT(state.outcomes.size() == 1);
  KJ_EXPECT(state.outcomes[0] == ActorRetryOutcome::UNABLE_TO_RETRY);
}

KJ_TEST("actor fetch stops after a retry claim rejection") {
  ReplayState state{
    .actions = kj::arr(ReplayAction::AMBIGUOUS, ReplayAction::CLAIM_REJECTED),
  };
  auto failure = KJ_REQUIRE_NONNULL(
      runActorFetch(state, ActorRetryGateEnabled::YES, kj::none, ActorFetchKind::HTTP));

  KJ_EXPECT(failure.getType() == kj::Exception::Type::DISCONNECTED, failure);
  KJ_EXPECT(!failure.getDescription().contains("claim rejected"), failure);
  KJ_EXPECT(state.requestCount == 2);
  KJ_EXPECT(state.retryCount == 1);
  KJ_EXPECT(state.observedRetryCount == 1);
  KJ_ASSERT(state.outcomes.size() == 1);
  KJ_EXPECT(state.outcomes[0] == ActorRetryOutcome::CLAIM_REJECTED);
}

KJ_TEST("actor fetch normalizes an initial retry claim rejection") {
  ReplayState state{.actions = kj::arr(ReplayAction::CLAIM_REJECTED)};
  auto failure = KJ_REQUIRE_NONNULL(
      runActorFetch(state, ActorRetryGateEnabled::YES, kj::none, ActorFetchKind::HTTP));

  KJ_EXPECT(failure.getType() == kj::Exception::Type::DISCONNECTED, failure);
  KJ_EXPECT(!failure.getDescription().contains("claim rejected"), failure);
  KJ_EXPECT(state.requestCount == 1);
  KJ_EXPECT(state.retryCount == 0);
  KJ_EXPECT(state.outcomes.size() == 0);
}

KJ_TEST("actor WebSocket fetch retries a disconnected handshake") {
  ReplayState state{
    .actions = kj::arr(ReplayAction::AMBIGUOUS),
    .acceptWebSocket = true,
  };

  KJ_EXPECT(runActorFetch(state, ActorRetryGateEnabled::YES, kj::none,
                ActorFetchKind::WEB_SOCKET) == kj::none);
  KJ_EXPECT(state.requestCount == 2);
  KJ_EXPECT(state.webSocketRequestCount == 2);
  KJ_EXPECT(state.retryCount == 1);
  KJ_EXPECT(state.observedRetryCount == 1);
  KJ_ASSERT(state.outcomes.size() == 1);
  KJ_EXPECT(state.outcomes[0] == ActorRetryOutcome::RECOVERED);
  KJ_EXPECT(state.acceptedWebSocket != kj::none);
}

KJ_TEST("actor fetch abort before an actor failure does not report retry telemetry") {
  ReplayState state{.actions = kj::arr(ReplayAction::AMBIGUOUS)};
  kj::Maybe<kj::Exception> failure;
  TestFixture fixture(TestFixture::SetupParams{
    .autogates = kj::arr<kj::StringPtr>(
        "durable-object-retries-fetch"_kj, "durable-object-retries-fetch-retry-requests"_kj),
    .useRealTimers = true,
    .requestObserverFactory =
        kj::Function<kj::Own<RequestObserver>()>([&]() -> kj::Own<RequestObserver> {
    return kj::refcounted<RetryRecordingObserver>(state);
  }),
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
  KJ_EXPECT(state.observedRetryCount == 0);
  KJ_EXPECT(state.outcomes.size() == 0);
}

KJ_TEST("actor fetch abort before the first retry does not report an outcome") {
  ReplayState state{.actions = kj::arr(ReplayAction::AMBIGUOUS)};
  kj::TimerImpl timer(kj::origin<kj::TimePoint>());
  DeterministicTimerChannel timerChannel(timer);
  state.timerChannel = timerChannel;
  auto retryDelayStarted = kj::newPromiseAndFulfiller<void>();
  timerChannel.pauseNextTimeout(kj::mv(retryDelayStarted.fulfiller));
  TestFixture fixture(TestFixture::SetupParams{
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel&) -> kj::Rc<IoChannelFactory> {
    return kj::rc<TestFixture::DummyIoChannelFactory>(timerChannel);
  }),
    .requestObserverFactory =
        kj::Function<kj::Own<RequestObserver>()>([&]() -> kj::Own<RequestObserver> {
    return kj::refcounted<RetryRecordingObserver>(state);
  }),
  });
  util::Autogate::initAutogateNamesForTest(
      {"durable-object-retries-fetch"_kj, "durable-object-retries-fetch-retry-requests"_kj},
      util::IgnoreAllAutogatesEnv::YES);

  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = env.js.alloc<Fetcher>(
        env.context.addObject<Fetcher::OutgoingFactory>(kj::heap<ReplayOutgoingFactory>(state)),
        Fetcher::RequiresHostAndProtocol::YES);
    auto controller = AbortController::constructor(env.js);
    auto abortPromise = env.context.awaitJs(env.js,
        env.context.awaitIo(env.js, kj::mv(retryDelayStarted.promise))
            .then(env.js, [controller = controller.addRef()](jsg::Lock& js) mutable {
      controller->abort(js, kj::none);
    }));
    RequestInitializerDict init;
    init.signal = kj::Maybe(controller->getSignal());
    auto promise = fetcher->fetch(env.js, kj::str("http://example.com"), kj::mv(init));
    auto fetchPromise =
        env.context.awaitJs(env.js, kj::mv(promise)).ignoreResult().catch_([](kj::Exception&&) {});
    return kj::joinPromises(kj::arr(kj::mv(fetchPromise), kj::mv(abortPromise)))
        .attach(kj::mv(fetcher), kj::mv(controller));
  });

  KJ_EXPECT(state.requestCount == 1);
  KJ_EXPECT(state.observedRetryCount == 0);
  KJ_EXPECT(state.outcomes.size() == 0);
}

KJ_TEST("actor fetch abort during an active retry reports canceled") {
  auto retryStarted = kj::newPromiseAndFulfiller<void>();
  ReplayState state{
    .actions = kj::arr(ReplayAction::AMBIGUOUS, ReplayAction::PAUSE_RESPONSE),
    .pauseStartedFulfiller = kj::mv(retryStarted.fulfiller),
  };
  runActorFetchUntilCanceled(state, kj::mv(retryStarted.promise));

  KJ_EXPECT(state.requestCount == 2);
  KJ_EXPECT(state.observedRetryCount == 1);
  KJ_ASSERT(state.outcomes.size() == 1);
  KJ_EXPECT(state.outcomes[0] == ActorRetryOutcome::CANCELED);
}

KJ_TEST("actor fetch abort during a later backoff reports canceled") {
  auto backoffStarted = kj::newPromiseAndFulfiller<void>();
  ReplayState state{
    .actions = kj::arr(ReplayAction::AMBIGUOUS, ReplayAction::PAUSE_NEXT_RETRY),
    .pauseStartedFulfiller = kj::mv(backoffStarted.fulfiller),
  };
  runActorFetchUntilCanceled(state, kj::mv(backoffStarted.promise));

  KJ_EXPECT(state.requestCount == 2);
  KJ_EXPECT(state.observedRetryCount == 1);
  KJ_ASSERT(state.outcomes.size() == 1);
  KJ_EXPECT(state.outcomes[0] == ActorRetryOutcome::CANCELED);
}

KJ_TEST("dropping actor fetch during a later backoff reports other") {
  auto backoffStarted = kj::newPromiseAndFulfiller<void>();
  ReplayState state{
    .actions = kj::arr(ReplayAction::AMBIGUOUS, ReplayAction::PAUSE_NEXT_RETRY),
    .pauseStartedFulfiller = kj::mv(backoffStarted.fulfiller),
  };
  kj::TimerImpl timer(kj::origin<kj::TimePoint>());
  DeterministicTimerChannel timerChannel(timer);
  state.timerChannel = timerChannel;
  TestFixture fixture(TestFixture::SetupParams{
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel&) -> kj::Rc<IoChannelFactory> {
    return kj::rc<TestFixture::DummyIoChannelFactory>(timerChannel);
  }),
    .requestObserverFactory =
        kj::Function<kj::Own<RequestObserver>()>([&]() -> kj::Own<RequestObserver> {
    return kj::refcounted<RetryRecordingObserver>(state);
  }),
  });
  util::Autogate::initAutogateNamesForTest(
      {"durable-object-retries-fetch"_kj, "durable-object-retries-fetch-retry-requests"_kj},
      util::IgnoreAllAutogatesEnv::YES);

  fixture.runInIoContext([&](const TestFixture::Environment& env) mutable {
    auto fetcher = env.js.alloc<Fetcher>(
        env.context.addObject<Fetcher::OutgoingFactory>(kj::heap<ReplayOutgoingFactory>(state)),
        Fetcher::RequiresHostAndProtocol::YES);
    auto promise = fetcher->fetch(env.js, kj::str("http://example.com"), kj::none);
    auto fetchPromise =
        env.context.awaitJs(env.js, kj::mv(promise)).ignoreResult().attach(kj::mv(fetcher));
    return kj::mv(backoffStarted.promise).then([fetchPromise = kj::mv(fetchPromise)]() mutable {});
  });

  KJ_EXPECT(state.requestCount == 2);
  KJ_EXPECT(state.observedRetryCount == 1);
  KJ_ASSERT(state.outcomes.size() == 1);
  KJ_EXPECT(state.outcomes[0] == ActorRetryOutcome::OTHER);
}

KJ_TEST("actor fetch stops after five attempts") {
  ReplayState state{
    .actions = kj::arr(ReplayAction::AMBIGUOUS, ReplayAction::AMBIGUOUS, ReplayAction::AMBIGUOUS,
        ReplayAction::AMBIGUOUS, ReplayAction::AMBIGUOUS),
  };

  KJ_EXPECT(
      runActorFetch(state, ActorRetryGateEnabled::YES, kj::none, ActorFetchKind::HTTP) != kj::none);
  KJ_EXPECT(state.requestCount == 5);
  KJ_EXPECT(state.retryCount == 4);
  KJ_EXPECT(state.observedRetryCount == 4);
  KJ_ASSERT(state.outcomes.size() == 1);
  KJ_EXPECT(state.outcomes[0] == ActorRetryOutcome::RETRIES_EXHAUSTED);
}

KJ_TEST("actor fetch allows an in-flight retry to finish after the start budget") {
  ReplayState state{
    .actions = kj::arr(ReplayAction::AMBIGUOUS, ReplayAction::SLOW_RESPONSE),
  };

  KJ_EXPECT(
      runActorFetch(state, ActorRetryGateEnabled::YES, kj::none, ActorFetchKind::HTTP) == kj::none);
  KJ_EXPECT(state.requestCount == 2);
  KJ_EXPECT(state.retryCount == 1);
  KJ_EXPECT(state.observedRetryCount == 1);
  KJ_ASSERT(state.outcomes.size() == 1);
  KJ_EXPECT(state.outcomes[0] == ActorRetryOutcome::RECOVERED);
}

KJ_TEST("actor fetch does not start a retry after the start budget") {
  ReplayState state{
    .actions = kj::arr(ReplayAction::RETRY_DELAY_EXCEEDS_BUDGET),
  };

  KJ_EXPECT(
      runActorFetch(state, ActorRetryGateEnabled::YES, kj::none, ActorFetchKind::HTTP) != kj::none);
  KJ_EXPECT(state.requestCount == 1);
  KJ_EXPECT(state.retryCount == 1);
  KJ_EXPECT(state.observedRetryCount == 0);
  KJ_EXPECT(state.outcomes.size() == 0);
}

KJ_TEST("replica actor fetch retries a request-level disconnect on its primary channel") {
  ReplayState state{.actions = kj::arr(ReplayAction::AMBIGUOUS)};
  kj::TimerImpl timer(kj::origin<kj::TimePoint>());
  DeterministicTimerChannel timerChannel(timer);
  state.timerChannel = timerChannel;
  uint checkedSubrequestCount = 0;
  TestFixture fixture(TestFixture::SetupParams{
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel&) -> kj::Rc<IoChannelFactory> {
    return kj::rc<TestFixture::DummyIoChannelFactory>(timerChannel);
  }),
    .requestObserverFactory =
        kj::Function<kj::Own<RequestObserver>()>([&]() -> kj::Own<RequestObserver> {
    return kj::refcounted<RetryRecordingObserver>(state);
  }),
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
    return env.context.awaitJs(env.js, kj::mv(promise)).ignoreResult().attach(kj::mv(fetcher));
  });

  KJ_EXPECT(state.requestCount == 2);
  KJ_ASSERT(state.metadata.size() == 2);
  KJ_EXPECT(state.metadata[0].nonce == state.metadata[1].nonce);
  KJ_EXPECT(state.metadata[0].isRetry == IsActorRetry::NO);
  KJ_EXPECT(state.metadata[1].isRetry == IsActorRetry::YES);
  KJ_EXPECT(checkedSubrequestCount == 1);
  KJ_ASSERT(state.outcomes.size() == 1);
}

KJ_TEST("GlobalActorOutgoingFactory forwards metadata and recreates channels for retries") {
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> capturedMetadata;
  uint channelCount = 0;
  uint checkedSubrequestCount = 0;
  kj::Vector<kj::String> locationHints;
  kj::Vector<kj::String> cohorts;
  kj::Vector<CountSubrequest> countSubrequests;
  TestFixture fixture(TestFixture::SetupParams{
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<ActorIoChannelFactory>(
        timer, capturedMetadata, channelCount, locationHints, cohorts);
  }),
    .requestObserverFactory = kj::Function<kj::Own<RequestObserver>()>(
        [&]() { return kj::refcounted<RecordingRequestObserver>(countSubrequests); }),
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
          .retryGateEnabled = ActorRetryGateEnabled::NO,
        },
        CountSubrequest::YES, [](TraceContext&) -> kj::Maybe<SpanParent> { return kj::none; });

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
          .retryGateEnabled = ActorRetryGateEnabled::NO,
        },
        CountSubrequest::NO, [](TraceContext&) -> kj::Maybe<SpanParent> { return kj::none; });
    KJ_EXPECT(checkedSubrequestCount == 1);
    KJ_EXPECT(channelCount == 2);
    KJ_ASSERT(locationHints.size() == 2);
    KJ_EXPECT(locationHints[0] == "location");
    KJ_EXPECT(locationHints[1] == "location");
    KJ_ASSERT(cohorts.size() == 2);
    KJ_EXPECT(cohorts[0] == "cohort");
    KJ_EXPECT(cohorts[1] == "cohort");
    KJ_ASSERT(countSubrequests.size() == 2);
    KJ_EXPECT(countSubrequests[0] == CountSubrequest::YES);
    KJ_EXPECT(countSubrequests[1] == CountSubrequest::NO);
  });
}

}  // namespace
}  // namespace workerd::api
