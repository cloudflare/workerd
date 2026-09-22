// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "http.h"
#include "worker-rpc.h"

#include <workerd/jsg/script.h>
#include <workerd/tests/test-fixture.h>

#include <kj/async-io.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

enum class FailurePattern {
  AMBIGUOUS,
  NOT_DELIVERED,
  DELIVERED,
};

enum class ReplayReservation {
  ACCEPT,
  REJECT,
};

// What the attempt after the failing ones connects to.
enum class Replacement {
  RECEIVER,
  HANGING,
  DEFERRED,
};

// How a KJ DISCONNECTED exception surfaces to JS; its description is not exposed.
constexpr kj::StringPtr DISCONNECT_JS_MESSAGE = "Error: Network connection lost."_kj;

constexpr kj::StringPtr RECEIVER_SOURCE = R"JS(
  import { RpcTarget, WorkerEntrypoint } from "cloudflare:workers";

  class Child extends RpcTarget {
    async child() {
      await scheduler.wait(1);
      return 42;
    }
    async makeChild() {
      await scheduler.wait(1);
      return new Child();
    }
    answer() { return 42; }
  }

  export default class extends WorkerEntrypoint {
    echo(value) { return value; }
    makeChild() { return new Child(); }
    get value() { return 42; }
  }
)JS"_kj;

struct RetryTestState {
  kj::Vector<IoChannelFactory::ActorRetryRequestMetadata> metadata;
  kj::Vector<CountSubrequest> countSubrequests;
  kj::Vector<ActorRetryOutcome> outcomes;
  uint acceptedRetries = 0;
  uint observedRetries = 0;
  uint observedAttempts = 0;
  uint committedAttempts = 0;
  uint pendingFailureClassifications = 0;
  uint finalizedFailureClassifications = 0;
  uint reservationAttempts = 0;
  uint activeRequestObservers = 0;
  ReplayReservation replayReservation = ReplayReservation::ACCEPT;
  size_t replayMemoryBytes = 0;
  kj::Maybe<size_t> lastReservationBytes;
  // `replayMemoryBytes` when the retry outcome was recorded, the first step after reentry.
  kj::Maybe<size_t> replayMemoryBytesAtOutcome;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> replacementStarted;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<kj::Own<WorkerInterface>>>> replacementFulfiller;
};

class PausingTimerChannel final: public TimerChannel {
 public:
  explicit PausingTimerChannel(uint pauseAtBackoff = 0): pauseAtBackoff(pauseAtBackoff) {
    auto started = kj::newPromiseAndFulfiller<void>();
    backoffStarted = kj::mv(started.promise);
    backoffStartedFulfiller = kj::mv(started.fulfiller);
  }

  void syncTime() override {}

  kj::Date now(kj::Maybe<kj::Date>) override {
    return kj::UNIX_EPOCH;
  }

  kj::Promise<void> atTime(kj::Date) override {
    return kj::READY_NOW;
  }

  kj::Promise<void> afterLimitTimeout(kj::Duration) override {
    if (++backoffCount == pauseAtBackoff) {
      KJ_REQUIRE_NONNULL(backoffStartedFulfiller)->fulfill();
      backoffStartedFulfiller = kj::none;
      return kj::NEVER_DONE;
    }
    return kj::READY_NOW;
  }

  kj::TimePoint nowForLimitTimeout() override {
    return kj::origin<kj::TimePoint>();
  }

  kj::Promise<void> onBackoffStarted() {
    return kj::mv(backoffStarted);
  }

 private:
  kj::Promise<void> backoffStarted = nullptr;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> backoffStartedFulfiller;
  uint pauseAtBackoff;
  uint backoffCount = 0;
};

class HoldingTimerChannel final: public TimerChannel {
 public:
  HoldingTimerChannel() {
    auto paf = kj::newPromiseAndFulfiller<void>();
    timeout = kj::mv(paf.promise);
    timeoutFulfiller = kj::mv(paf.fulfiller);
  }

  void syncTime() override {}

  kj::Date now(kj::Maybe<kj::Date>) override {
    return kj::UNIX_EPOCH;
  }

  kj::Promise<void> atTime(kj::Date) override {
    KJ_REQUIRE(!timeoutClaimed);
    timeoutClaimed = true;
    return kj::mv(timeout);
  }

  kj::Promise<void> afterLimitTimeout(kj::Duration) override {
    return kj::READY_NOW;
  }

  kj::TimePoint nowForLimitTimeout() override {
    return kj::origin<kj::TimePoint>();
  }

  void release() {
    timeoutFulfiller->fulfill();
  }

 private:
  kj::Promise<void> timeout = nullptr;
  kj::Own<kj::PromiseFulfiller<void>> timeoutFulfiller;
  bool timeoutClaimed = false;
};

class RetryCallObserver final: public OutgoingActorCallObserver {
 public:
  explicit RetryCallObserver(RetryTestState& state): state(state) {}
  ~RetryCallObserver() noexcept(false) {
    if (classificationPending) {
      --state.pendingFailureClassifications;
      ++state.finalizedFailureClassifications;
    }
  }

  void markPipelineCommitted() override {
    if (!pipelineCommitted) {
      pipelineCommitted = true;
      ++state.committedAttempts;
    }
  }
  void recordFailureAwaitingRetryDecision(kj::Exception&) override {
    ++state.pendingFailureClassifications;
    classificationPending = true;
  }

 private:
  RetryTestState& state;
  bool pipelineCommitted = false;
  bool classificationPending = false;
};

class RetryObserver final: public RequestObserver {
 public:
  explicit RetryObserver(RetryTestState& state): state(state) {
    ++state.activeRequestObservers;
  }
  ~RetryObserver() noexcept(false) {
    --state.activeRequestObservers;
  }

  kj::Maybe<kj::Own<OutgoingActorCallObserver>> observeOutgoingActorRpcCall(
      ActorCallPayloadReplayable payloadReplayable, ActorCallTargetRetryable) override {
    KJ_EXPECT(payloadReplayable == ActorCallPayloadReplayable::YES);
    ++state.observedAttempts;
    return kj::heap<RetryCallObserver>(state);
  }

  void recordActorRetry(ActorRetryCallType callType) override {
    KJ_EXPECT(callType == ActorRetryCallType::JSRPC);
    ++state.observedRetries;
  }

  void recordActorRetryOutcome(
      ActorRetryCallType callType, ActorRetryOutcome outcome, kj::Duration) override {
    KJ_EXPECT(callType == ActorRetryCallType::JSRPC);
    state.outcomes.add(outcome);
    state.replayMemoryBytesAtOutcome = state.replayMemoryBytes;
  }

  kj::Own<void> trackActorCallReplayMemory(size_t bytes) override {
    return trackMemory(bytes);
  }

  kj::Maybe<kj::Own<void>> tryReserveActorCallReplayMemory(size_t bytes) override {
    ++state.reservationAttempts;
    state.lastReservationBytes = bytes;
    if (state.replayReservation == ReplayReservation::REJECT) return kj::none;
    return trackMemory(bytes);
  }

 private:
  kj::Own<void> trackMemory(size_t bytes) {
    state.replayMemoryBytes += bytes;
    return kj::heap(kj::defer([&state = state, bytes]() { state.replayMemoryBytes -= bytes; }));
  }

  RetryTestState& state;
};

// A session that fails before returning a call result.
kj::Own<WorkerInterface> newFailingSession(FailurePattern failurePattern) {
  auto exception = KJ_EXCEPTION(DISCONNECTED, "JSRPC session disconnected");
  if (failurePattern == FailurePattern::NOT_DELIVERED) {
    jsg::markActorRequestNotDelivered(exception);
  } else if (failurePattern == FailurePattern::DELIVERED) {
    exception.setDetail(jsg::REQUEST_DELIVERED_TO_ACTOR_DETAIL_ID, kj::heapArray<kj::byte>(0));
  }
  return newPromisedWorkerInterface(kj::mv(exception));
}

// A session that never delivers its event.
kj::Own<WorkerInterface> newHangingSession() {
  return newPromisedWorkerInterface(kj::NEVER_DONE);
}

class RetryOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  RetryOutgoingFactory(TestFixture& receiver,
      RetryTestState& state,
      FailurePattern failurePattern,
      uint failingAttempts,
      Replacement replacement)
      : receiver(receiver),
        state(state),
        failurePattern(failurePattern),
        failingAttempts(failingAttempts),
        replacement(replacement) {}

  Result newSingleUseClient(kj::Maybe<kj::String>, MakeUserSpanParent) override {
    KJ_FAIL_REQUIRE("retryable JSRPC call bypassed actor attempt plumbing");
  }

  kj::Maybe<ActorCallTargetRetryable> getActorTargetRetryability() const override {
    return ActorCallTargetRetryable::YES;
  }

  void onActorCallRetry() override {
    ++state.acceptedRetries;
  }

  Result newActorCallAttempt(
      kj::Maybe<kj::String>, ActorCallRetryState::Attempt attempt, MakeUserSpanParent) override {
    state.countSubrequests.add(attempt.getCountSubrequest());
    state.metadata.add(KJ_REQUIRE_NONNULL(attempt.takeMetadata()));
    if (state.metadata.size() <= failingAttempts) {
      return {.client = newFailingSession(failurePattern), .spanParents = kj::none};
    }
    kj::Maybe<kj::Promise<kj::Own<WorkerInterface>>> replacementPromise;
    if (replacement == Replacement::DEFERRED) {
      auto paf = kj::newPromiseAndFulfiller<kj::Own<WorkerInterface>>();
      replacementPromise = kj::mv(paf.promise);
      state.replacementFulfiller = kj::mv(paf.fulfiller);
    }
    KJ_IF_SOME(fulfiller, state.replacementStarted) {
      fulfiller->fulfill();
      state.replacementStarted = kj::none;
    }
    if (replacement == Replacement::HANGING) {
      return {.client = newHangingSession(), .spanParents = kj::none};
    }
    KJ_IF_SOME(promise, replacementPromise) {
      return {.client = newPromisedWorkerInterface(kj::mv(promise)), .spanParents = kj::none};
    }
    return {.client = receiver.makeWorkerEntrypoint(), .spanParents = kj::none};
  }

 private:
  TestFixture& receiver;
  RetryTestState& state;
  FailurePattern failurePattern;
  uint failingAttempts;
  Replacement replacement;
};

CompatibilityFlags::Reader makeRetryFlags(capnp::MallocMessageBuilder& message) {
  auto flags = message.initRoot<CompatibilityFlags>();
  flags.setFetcherRpc(true);
  return flags.asReader();
}

TestFixture::SetupParams makeReceiverParams(kj::WaitScope& waitScope) {
  return {
    .waitScope = waitScope,
    .mainModuleSource = RECEIVER_SOURCE,
    .useRealTimers = false,
  };
}

TestFixture::SetupParams makeReceiverParams(kj::WaitScope& waitScope, HoldingTimerChannel& timer) {
  auto params = makeReceiverParams(waitScope);
  params.ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
      [&timer](TimerChannel&) -> kj::Rc<IoChannelFactory> {
    return kj::rc<TestFixture::DummyIoChannelFactory>(timer);
  });
  return params;
}

TestFixture::SetupParams makeSenderParams(kj::WaitScope& waitScope,
    CompatibilityFlags::Reader flags,
    TimerChannel& timer,
    RetryTestState& state) {
  return {
    .waitScope = waitScope,
    .featureFlags = flags,
    .autogates =
        kj::arr("durable-object-retries-fetch"_kj, "durable-object-retries-fetch-retry-requests"_kj,
            "durable-object-retries-jsrpc"_kj, "durable-object-retries-jsrpc-retry-requests"_kj),
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&timer](TimerChannel&) -> kj::Rc<IoChannelFactory> {
    return kj::rc<TestFixture::DummyIoChannelFactory>(timer);
  }),
    .requestObserverFactory = kj::Function<kj::Own<RequestObserver>()>(
        [&state]() -> kj::Own<RequestObserver> { return kj::refcounted<RetryObserver>(state); }),
  };
}

jsg::Ref<Fetcher> makeRetryFetcher(const TestFixture::Environment& env,
    TestFixture& receiver,
    RetryTestState& state,
    FailurePattern failurePattern,
    uint failingAttempts,
    Replacement replacement = Replacement::RECEIVER) {
  return env.js.alloc<Fetcher>(
      env.context.addObject<Fetcher::OutgoingFactory>(kj::heap<RetryOutgoingFactory>(
          receiver, state, failurePattern, failingAttempts, replacement)),
      Fetcher::RequiresHostAndProtocol::YES);
}

// Pipelines a call to `child()` on `parent`, returning the child's result promise.
jsg::JsValue callChild(jsg::Lock& js, JsRpcPromise& parent) {
  auto childMethod = KJ_REQUIRE_NONNULL(parent.getProperty(js, kj::str("child")));
  auto& handler = KJ_REQUIRE_NONNULL(js.tryGetTypeHandler<jsg::Ref<JsRpcProperty>>());
  auto childFunction = KJ_REQUIRE_NONNULL(
      jsg::JsValue(handler.wrap(js, kj::mv(childMethod))).tryCast<jsg::JsFunction>());
  return childFunction.call(js, js.undefined());
}

// Resolves once `value` rejects with a disconnect, as the stored first-attempt failure is.
jsg::Promise<void> expectDisconnect(jsg::Lock& js, jsg::JsValue value) {
  return js.toPromise(value).then(js, [](jsg::Lock& js, jsg::Value) {
    KJ_FAIL_ASSERT("committed RPC attempt unexpectedly succeeded");
  }, [](jsg::Lock& js, jsg::Value error) {
    auto message = jsg::JsValue(error.getHandle(js)).toString(js);
    KJ_EXPECT(message == DISCONNECT_JS_MESSAGE, message);
  });
}

jsg::JsFunction getRpcFunction(jsg::Lock& js, Fetcher& fetcher, kj::StringPtr name) {
  auto method = KJ_REQUIRE_NONNULL(fetcher.getRpcMethodForTestOnly(js, kj::str(name)));
  auto& handler = KJ_REQUIRE_NONNULL(js.tryGetTypeHandler<jsg::Ref<JsRpcProperty>>());
  return KJ_REQUIRE_NONNULL(
      jsg::JsValue(handler.wrap(js, kj::mv(method))).tryCast<jsg::JsFunction>());
}

KJ_TEST("ambiguous actor RPC disconnect is retried") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  PausingTimerChannel timer;
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = makeRetryFetcher(env, receiver, state, FailurePattern::AMBIGUOUS, 1);
    auto function = getRpcFunction(env.js, *fetcher, "echo"_kj);
    auto result = function.call(env.js, env.js.undefined(), env.js.num(42));
    auto checked = env.js.toPromise(result).then(env.js, [](jsg::Lock& js, jsg::Value value) {
      KJ_EXPECT(jsg::JsValue(value.getHandle(js)).strictEquals(js.num(42)));
    });
    return env.context.awaitJs(env.js, kj::mv(checked)).attach(kj::mv(fetcher));
  });

  KJ_ASSERT(state.metadata.size() == 2);
  KJ_EXPECT(state.metadata[0].nonce == state.metadata[1].nonce);
  KJ_EXPECT(state.metadata[0].isRetry == IsActorRetry::NO);
  KJ_EXPECT(state.metadata[1].isRetry == IsActorRetry::YES);
  KJ_ASSERT(state.countSubrequests.size() == 2);
  KJ_EXPECT(state.countSubrequests[0] == CountSubrequest::YES);
  KJ_EXPECT(state.countSubrequests[1] == CountSubrequest::NO);
  KJ_EXPECT(state.acceptedRetries == 1);
  KJ_EXPECT(state.observedRetries == 1);
  KJ_EXPECT(state.observedAttempts == 2);
  KJ_EXPECT(state.finalizedFailureClassifications == 1);
  KJ_ASSERT(state.outcomes.size() == 1);
  KJ_EXPECT(state.outcomes[0] == ActorRetryOutcome::RECOVERED);
  // The payload is released at native success, before the call reenters the isolate.
  KJ_EXPECT(KJ_ASSERT_NONNULL(state.replayMemoryBytesAtOutcome) == 0);
  KJ_EXPECT(state.replayMemoryBytes == 0);
}

KJ_TEST("successful actor RPC keeps its first-attempt result pipeline alive") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  PausingTimerChannel timer;
  HoldingTimerChannel childTimer;
  TestFixture receiver(makeReceiverParams(io.waitScope, childTimer));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = makeRetryFetcher(env, receiver, state, FailurePattern::AMBIGUOUS, 0);
    auto function = getRpcFunction(env.js, *fetcher, "makeChild"_kj);
    auto parentValue = function.call(env.js, env.js.undefined());
    auto parentObject = KJ_REQUIRE_NONNULL(parentValue.tryCast<jsg::JsObject>());
    auto parent = KJ_REQUIRE_NONNULL(parentObject.tryUnwrapAs<JsRpcPromise>(env.js));
    auto childMethod = KJ_REQUIRE_NONNULL(parent->getProperty(env.js, kj::str("makeChild")));
    auto& handler = KJ_REQUIRE_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<JsRpcProperty>>());
    auto childFunction = KJ_REQUIRE_NONNULL(
        jsg::JsValue(handler.wrap(env.js, kj::mv(childMethod))).tryCast<jsg::JsFunction>());
    auto childValue = childFunction.call(env.js, env.js.undefined());

    auto parentSettled = env.context.awaitJs(
        env.js, env.js.toPromise(parentValue).then(env.js, [&childTimer](jsg::Lock&, jsg::Value) {
      childTimer.release();
    }));
    auto childSettled = env.context.awaitJs(
        env.js, env.js.toPromise(childValue).then(env.js, [](jsg::Lock& js, jsg::Value value) {
      auto stub = jsg::JsValue(value.getHandle(js));
      auto method = KJ_REQUIRE_NONNULL(KJ_REQUIRE_NONNULL(stub.tryCast<jsg::JsObject>())
                                           .get(js, "answer"_kj)
                                           .tryCast<jsg::JsFunction>());
      return js.toPromise(method.call(js, stub)).then(js, [](jsg::Lock& js, jsg::Value answer) {
        KJ_EXPECT(jsg::JsValue(answer.getHandle(js)).strictEquals(js.num(42)));
      });
    }));
    return kj::joinPromises(kj::arr(kj::mv(parentSettled), kj::mv(childSettled)))
        .attach(kj::mv(parent), kj::mv(fetcher));
  });

  KJ_EXPECT(state.acceptedRetries == 0);
  KJ_EXPECT(state.observedRetries == 0);
  KJ_EXPECT(state.replayMemoryBytes == 0);
}

KJ_TEST("successful actor RPC keeps its replacement result pipeline alive") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  PausingTimerChannel timer;
  HoldingTimerChannel childTimer;
  TestFixture receiver(makeReceiverParams(io.waitScope, childTimer));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  auto replacementStarted = kj::newPromiseAndFulfiller<void>();
  state.replacementStarted = kj::mv(replacementStarted.fulfiller);

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher =
        makeRetryFetcher(env, receiver, state, FailurePattern::AMBIGUOUS, 1, Replacement::DEFERRED);
    auto function = getRpcFunction(env.js, *fetcher, "makeChild"_kj);
    auto parentValue = function.call(env.js, env.js.undefined());
    auto parentObject = KJ_REQUIRE_NONNULL(parentValue.tryCast<jsg::JsObject>());
    auto parent = KJ_REQUIRE_NONNULL(parentObject.tryUnwrapAs<JsRpcPromise>(env.js));

    auto parentSettled = env.context.awaitJs(
        env.js, env.js.toPromise(parentValue).then(env.js, [&childTimer](jsg::Lock&, jsg::Value) {
      childTimer.release();
    }));
    auto commitReplacement = env.context.awaitIo(env.js, kj::mv(replacementStarted.promise),
        [parent = parent.addRef(), &receiver, &state](jsg::Lock& js) mutable {
      auto childValue = callChild(js, *parent);
      KJ_ASSERT_NONNULL(state.replacementFulfiller)->fulfill(receiver.makeWorkerEntrypoint());
      state.replacementFulfiller = kj::none;
      return js.toPromise(childValue).then(js, [](jsg::Lock& js, jsg::Value value) {
        KJ_EXPECT(jsg::JsValue(value.getHandle(js)).strictEquals(js.num(42)));
      });
    });
    auto childSettled = env.context.awaitJs(env.js, kj::mv(commitReplacement));
    return kj::joinPromises(kj::arr(kj::mv(parentSettled), kj::mv(childSettled)))
        .attach(kj::mv(parent), kj::mv(fetcher));
  });

  KJ_EXPECT(state.acceptedRetries == 1);
  KJ_EXPECT(state.observedRetries == 1);
  KJ_ASSERT(state.outcomes.size() == 1);
  KJ_EXPECT(state.outcomes[0] == ActorRetryOutcome::RECOVERED);
  KJ_EXPECT(state.replayMemoryBytes == 0);
}

KJ_TEST("not-delivered actor RPC disconnect retries with a new token") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  PausingTimerChannel timer;
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = makeRetryFetcher(env, receiver, state, FailurePattern::NOT_DELIVERED, 1);
    auto function = getRpcFunction(env.js, *fetcher, "echo"_kj);
    auto result = function.call(env.js, env.js.undefined(), env.js.num(42));
    auto checked = env.js.toPromise(result).then(env.js, [](jsg::Lock& js, jsg::Value value) {
      KJ_EXPECT(jsg::JsValue(value.getHandle(js)).strictEquals(js.num(42)));
    });
    return env.context.awaitJs(env.js, kj::mv(checked)).attach(kj::mv(fetcher));
  });

  KJ_ASSERT(state.metadata.size() == 2);
  KJ_EXPECT(state.metadata[0].nonce != state.metadata[1].nonce);
  KJ_EXPECT(state.metadata[0].isRetry == IsActorRetry::NO);
  KJ_EXPECT(state.metadata[1].isRetry == IsActorRetry::NO);
  KJ_EXPECT(state.acceptedRetries == 1);
  KJ_EXPECT(state.observedRetries == 1);
  KJ_EXPECT(state.observedAttempts == 2);
  KJ_ASSERT(state.outcomes.size() == 1);
  KJ_EXPECT(state.outcomes[0] == ActorRetryOutcome::RECOVERED);
  KJ_EXPECT(state.replayMemoryBytes == 0);
}

KJ_TEST("using the result pipeline before disconnect prevents an actor RPC retry") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  PausingTimerChannel timer;
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = makeRetryFetcher(env, receiver, state, FailurePattern::AMBIGUOUS, 1);
    auto function = getRpcFunction(env.js, *fetcher, "echo"_kj);
    auto parentValue = function.call(env.js, env.js.undefined(), env.js.num(42));
    auto parentObject = KJ_REQUIRE_NONNULL(parentValue.tryCast<jsg::JsObject>());
    auto parent = KJ_REQUIRE_NONNULL(parentObject.tryUnwrapAs<JsRpcPromise>(env.js));

    auto childValue = callChild(env.js, *parent);
    KJ_EXPECT(state.replayMemoryBytes == 0);

    auto expectRejected = [&](jsg::JsValue value) {
      return env.context.awaitJs(env.js, expectDisconnect(env.js, value));
    };
    return kj::joinPromises(kj::arr(expectRejected(parentValue), expectRejected(childValue)))
        .attach(kj::mv(parent), kj::mv(fetcher));
  });

  KJ_EXPECT(state.metadata.size() == 1);
  KJ_EXPECT(state.acceptedRetries == 0);
  KJ_EXPECT(state.observedRetries == 0);
  KJ_EXPECT(state.committedAttempts == 1);
  KJ_EXPECT(state.outcomes.empty());
  KJ_EXPECT(state.replayMemoryBytes == 0);
}

KJ_TEST("using the result pipeline during backoff prevents an actor RPC retry") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  PausingTimerChannel timer(1);
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = makeRetryFetcher(env, receiver, state, FailurePattern::AMBIGUOUS, 1);
    auto function = getRpcFunction(env.js, *fetcher, "echo"_kj);
    auto parentValue = function.call(env.js, env.js.undefined(), env.js.num(42));
    auto parentObject = KJ_REQUIRE_NONNULL(parentValue.tryCast<jsg::JsObject>());
    auto parent = KJ_REQUIRE_NONNULL(parentObject.tryUnwrapAs<JsRpcPromise>(env.js));

    auto parentRejected = env.context.awaitJs(env.js, expectDisconnect(env.js, parentValue));
    auto commitDuringBackoff = env.context.awaitIo(env.js, timer.onBackoffStarted(),
        [parent = parent.addRef(), &state](jsg::Lock& js) mutable {
      KJ_EXPECT(state.pendingFailureClassifications == 1);
      auto childValue = callChild(js, *parent);
      KJ_EXPECT(state.replayMemoryBytes == 0);
      return expectDisconnect(js, childValue);
    });
    auto childRejected = env.context.awaitJs(env.js, kj::mv(commitDuringBackoff));
    return kj::joinPromises(kj::arr(kj::mv(parentRejected), kj::mv(childRejected)))
        .attach(kj::mv(parent), kj::mv(fetcher));
  });

  KJ_EXPECT(state.metadata.size() == 1);
  KJ_EXPECT(state.acceptedRetries == 1);
  KJ_EXPECT(state.observedRetries == 0);
  KJ_EXPECT(state.committedAttempts == 1);
  KJ_EXPECT(state.finalizedFailureClassifications == 1);
  KJ_EXPECT(state.outcomes.empty());
  KJ_EXPECT(state.replayMemoryBytes == 0);
}

KJ_TEST("using the result pipeline after one retry records unable-to-retry") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  PausingTimerChannel timer(2);
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = makeRetryFetcher(env, receiver, state, FailurePattern::AMBIGUOUS, 2);
    auto function = getRpcFunction(env.js, *fetcher, "echo"_kj);
    auto parentValue = function.call(env.js, env.js.undefined(), env.js.num(42));
    auto parentObject = KJ_REQUIRE_NONNULL(parentValue.tryCast<jsg::JsObject>());
    auto parent = KJ_REQUIRE_NONNULL(parentObject.tryUnwrapAs<JsRpcPromise>(env.js));

    auto parentRejected = env.context.awaitJs(env.js, expectDisconnect(env.js, parentValue));
    auto commitDuringBackoff = env.context.awaitIo(
        env.js, timer.onBackoffStarted(), [parent = parent.addRef()](jsg::Lock& js) mutable {
      return expectDisconnect(js, callChild(js, *parent));
    });
    auto childRejected = env.context.awaitJs(env.js, kj::mv(commitDuringBackoff));
    return kj::joinPromises(kj::arr(kj::mv(parentRejected), kj::mv(childRejected)))
        .attach(kj::mv(parent), kj::mv(fetcher));
  });

  KJ_EXPECT(state.metadata.size() == 2);
  KJ_EXPECT(state.acceptedRetries == 2);
  KJ_EXPECT(state.observedRetries == 1);
  KJ_EXPECT(state.committedAttempts == 1);
  KJ_EXPECT(state.finalizedFailureClassifications == 2);
  KJ_ASSERT(state.outcomes.size() == 1);
  KJ_EXPECT(state.outcomes[0] == ActorRetryOutcome::UNABLE_TO_RETRY);
  KJ_EXPECT(state.replayMemoryBytes == 0);
}

KJ_TEST("claim rejection after pipeline commitment preserves the original disconnect") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  PausingTimerChannel timer;
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  auto replacementStarted = kj::newPromiseAndFulfiller<void>();
  state.replacementStarted = kj::mv(replacementStarted.fulfiller);

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher =
        makeRetryFetcher(env, receiver, state, FailurePattern::AMBIGUOUS, 1, Replacement::DEFERRED);
    auto function = getRpcFunction(env.js, *fetcher, "echo"_kj);
    auto parentValue = function.call(env.js, env.js.undefined(), env.js.num(42));
    auto parentObject = KJ_REQUIRE_NONNULL(parentValue.tryCast<jsg::JsObject>());
    auto parent = KJ_REQUIRE_NONNULL(parentObject.tryUnwrapAs<JsRpcPromise>(env.js));

    auto parentRejected = env.context.awaitJs(env.js, expectDisconnect(env.js, parentValue));
    auto commitReplacement = env.context.awaitIo(env.js, kj::mv(replacementStarted.promise),
        [parent = parent.addRef(), &state](jsg::Lock& js) mutable {
      auto childValue = callChild(js, *parent);
      auto rejection = KJ_EXCEPTION(FAILED, "retry claim rejected");
      rejection.setDetail(jsg::ACTOR_RETRY_CLAIM_REJECTED_DETAIL_ID, kj::heapArray<kj::byte>(0));
      KJ_ASSERT_NONNULL(state.replacementFulfiller)->reject(kj::mv(rejection));
      state.replacementFulfiller = kj::none;
      return expectDisconnect(js, childValue);
    });
    auto childRejected = env.context.awaitJs(env.js, kj::mv(commitReplacement));
    return kj::joinPromises(kj::arr(kj::mv(parentRejected), kj::mv(childRejected)))
        .attach(kj::mv(parent), kj::mv(fetcher));
  });

  KJ_EXPECT(state.metadata.size() == 2);
  KJ_EXPECT(state.acceptedRetries == 1);
  KJ_EXPECT(state.observedRetries == 1);
  KJ_EXPECT(state.committedAttempts == 1);
  KJ_ASSERT(state.outcomes.size() == 1);
  KJ_EXPECT(state.outcomes[0] == ActorRetryOutcome::CLAIM_REJECTED);
  KJ_EXPECT(state.replayMemoryBytes == 0);
}

KJ_TEST("claim rejection breaks session pipelines with a disconnect") {
  auto io = kj::setupAsyncIo();
  auto session = kj::heap<JsRpcSessionCustomEvent>(JsRpcSessionCustomEvent::WORKER_RPC_EVENT_TYPE);
  auto cap = session->getCap();
  auto request = cap.callRequest();
  request.setMethodName("echo");
  auto parent = request.send();
  auto childRequest = parent.getCallPipeline().callRequest();
  childRequest.setMethodName("child");
  auto child = childRequest.send();

  auto rejection = KJ_EXCEPTION(FAILED, "retry claim rejected");
  rejection.setDetail(jsg::ACTOR_RETRY_CLAIM_REJECTED_DETAIL_ID, kj::heapArray<kj::byte>(0));
  session->failed(rejection);

  auto childFailure =
      KJ_ASSERT_NONNULL(kj::runCatchingExceptions([&] { child.wait(io.waitScope); }));
  KJ_EXPECT(childFailure.getType() == kj::Exception::Type::DISCONNECTED, childFailure);
  KJ_EXPECT(childFailure.getDetail(jsg::ACTOR_RETRY_CLAIM_REJECTED_DETAIL_ID) != kj::none);
  auto parentFailure =
      KJ_ASSERT_NONNULL(kj::runCatchingExceptions([&] { parent.wait(io.waitScope); }));
  KJ_EXPECT(parentFailure.getType() == kj::Exception::Type::DISCONNECTED, parentFailure);
  KJ_EXPECT(parentFailure.getDetail(jsg::ACTOR_RETRY_CLAIM_REJECTED_DETAIL_ID) != kj::none);
}

KJ_TEST("dropping a pipeline-used replacement records actor RPC retry cancellation") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  PausingTimerChannel timer;
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  auto replacementStarted = kj::newPromiseAndFulfiller<void>();
  state.replacementStarted = kj::mv(replacementStarted.fulfiller);

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher =
        makeRetryFetcher(env, receiver, state, FailurePattern::AMBIGUOUS, 1, Replacement::HANGING);
    auto function = getRpcFunction(env.js, *fetcher, "echo"_kj);
    auto parentValue = function.call(env.js, env.js.undefined(), env.js.num(42));
    auto parentObject = KJ_REQUIRE_NONNULL(parentValue.tryCast<jsg::JsObject>());
    auto parent = KJ_REQUIRE_NONNULL(parentObject.tryUnwrapAs<JsRpcPromise>(env.js));

    // Commit to the hanging replacement attempt, then let the context end with the call in flight.
    auto commitToReplacement = env.context.awaitIo(env.js, kj::mv(replacementStarted.promise),
        [parent = parent.addRef()](jsg::Lock& js) mutable { callChild(js, *parent); });
    return env.context.awaitJs(env.js, kj::mv(commitToReplacement))
        .attach(kj::mv(parent), kj::mv(fetcher));
  });

  KJ_EXPECT(state.metadata.size() == 2);
  KJ_EXPECT(state.acceptedRetries == 1);
  KJ_EXPECT(state.observedRetries == 1);
  KJ_ASSERT(state.outcomes.size() == 1);
  KJ_EXPECT(state.outcomes[0] == ActorRetryOutcome::CANCELED);
  KJ_EXPECT(state.replayMemoryBytes == 0);
}

KJ_TEST("failed replay memory reservation disables actor RPC retries") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  state.replayReservation = ReplayReservation::REJECT;
  PausingTimerChannel timer;
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = makeRetryFetcher(env, receiver, state, FailurePattern::AMBIGUOUS, 1);
    auto function = getRpcFunction(env.js, *fetcher, "echo"_kj);
    auto rejected =
        expectDisconnect(env.js, function.call(env.js, env.js.undefined(), env.js.num(42)));
    return env.context.awaitJs(env.js, kj::mv(rejected)).attach(kj::mv(fetcher));
  });

  KJ_ASSERT(state.metadata.size() == 1);
  KJ_EXPECT(state.metadata[0].retryGateEnabled == ActorRetryGateEnabled::NO);
  KJ_EXPECT(state.reservationAttempts == 1);
  KJ_EXPECT(state.acceptedRetries == 0);
  KJ_EXPECT(state.observedRetries == 0);
  KJ_EXPECT(state.pendingFailureClassifications == 0);
  KJ_EXPECT(state.replayMemoryBytes == 0);
}

KJ_TEST("actor RPC does not retry a delivered disconnect") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  PausingTimerChannel timer;
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = makeRetryFetcher(env, receiver, state, FailurePattern::DELIVERED, 1);
    auto function = getRpcFunction(env.js, *fetcher, "echo"_kj);
    auto rejected =
        expectDisconnect(env.js, function.call(env.js, env.js.undefined(), env.js.num(42)));
    return env.context.awaitJs(env.js, kj::mv(rejected)).attach(kj::mv(fetcher));
  });

  KJ_EXPECT(state.metadata.size() == 1);
  KJ_EXPECT(state.acceptedRetries == 0);
  KJ_EXPECT(state.observedRetries == 0);
  KJ_EXPECT(state.outcomes.empty());
  KJ_EXPECT(state.pendingFailureClassifications == 0);
  KJ_EXPECT(state.finalizedFailureClassifications == 1);
  KJ_EXPECT(state.replayMemoryBytes == 0);
}

KJ_TEST("dropping an actor RPC promise during backoff releases retry state") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  PausingTimerChannel timer(1);
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = makeRetryFetcher(env, receiver, state, FailurePattern::AMBIGUOUS, 1);
    auto function = getRpcFunction(env.js, *fetcher, "echo"_kj);
    function.call(env.js, env.js.undefined(), env.js.num(42));
    auto backoffStarted = env.context.awaitIo(env.js, timer.onBackoffStarted());
    return env.context.awaitJs(env.js, kj::mv(backoffStarted)).attach(kj::mv(fetcher));
  });

  KJ_EXPECT(state.metadata.size() == 1);
  KJ_EXPECT(state.acceptedRetries == 1);
  KJ_EXPECT(state.observedRetries == 0);
  KJ_EXPECT(state.outcomes.empty());
  KJ_EXPECT(state.pendingFailureClassifications == 0);
  KJ_EXPECT(state.finalizedFailureClassifications == 1);
  KJ_EXPECT(state.replayMemoryBytes == 0);
}

KJ_TEST("actor RPC property reads retry on a fresh session") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  PausingTimerChannel timer;
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = makeRetryFetcher(env, receiver, state, FailurePattern::AMBIGUOUS, 1);
    auto property = KJ_REQUIRE_NONNULL(fetcher->getRpcMethodForTestOnly(env.js, kj::str("value")));
    auto& handler = KJ_REQUIRE_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<JsRpcProperty>>());
    auto value = jsg::JsValue(handler.wrap(env.js, kj::mv(property)));
    auto checked = env.js.toPromise(value).then(env.js, [](jsg::Lock& js, jsg::Value value) {
      KJ_EXPECT(jsg::JsValue(value.getHandle(js)).strictEquals(js.num(42)));
    });
    return env.context.awaitJs(env.js, kj::mv(checked)).attach(kj::mv(fetcher));
  });

  KJ_ASSERT(state.metadata.size() == 2);
  KJ_EXPECT(state.metadata[0].nonce == state.metadata[1].nonce);
  KJ_EXPECT(state.metadata[1].isRetry == IsActorRetry::YES);
  KJ_EXPECT(state.acceptedRetries == 1);
  KJ_EXPECT(state.observedRetries == 1);
  KJ_ASSERT(state.outcomes.size() == 1);
  KJ_EXPECT(state.outcomes[0] == ActorRetryOutcome::RECOVERED);
  KJ_EXPECT(state.replayMemoryBytes == 0);
}

KJ_TEST("argument-free actor RPC reserves memory for retained metadata") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  state.replayReservation = ReplayReservation::REJECT;
  PausingTimerChannel timer;
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));
  constexpr auto METHOD_NAME =
      "a_long_actor_rpc_method_name_that_must_be_included_in_the_replay_memory_reservation"_kj;

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = makeRetryFetcher(env, receiver, state, FailurePattern::AMBIGUOUS, 1);
    auto function = getRpcFunction(env.js, *fetcher, METHOD_NAME);
    auto rejected = expectDisconnect(env.js, function.call(env.js, env.js.undefined()));
    return env.context.awaitJs(env.js, kj::mv(rejected)).attach(kj::mv(fetcher));
  });

  KJ_EXPECT(state.reservationAttempts == 1);
  KJ_EXPECT(KJ_ASSERT_NONNULL(state.lastReservationBytes) > METHOD_NAME.size());
  KJ_EXPECT(state.replayMemoryBytes == 0);
}

KJ_TEST("rejected actor RPC promise does not retain its request observer") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  PausingTimerChannel timer;
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = makeRetryFetcher(env, receiver, state, FailurePattern::DELIVERED, 1);
    auto global = jsg::JsObject(env.js.v8Context()->Global());
    auto& handler = KJ_REQUIRE_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<Fetcher>>());
    global.set(env.js, "fetcher"_kj, jsg::JsValue(handler.wrap(env.js, fetcher.addRef())));
    KJ_DEFER(global.delete_(env.js, "fetcher"_kj));
    auto rejected = jsg::NonModuleScript::compile(env.js,
        "globalThis.retainedRejectedRpc = fetcher.echo(42); retainedRejectedRpc"_kj,
        "jsrpc-call-retry-test.js"_kj)
                        .runAndReturn(env.js);
    return env.context.awaitJs(env.js, expectDisconnect(env.js, rejected)).attach(kj::mv(fetcher));
  });

  KJ_EXPECT(state.activeRequestObservers == 0);
}

KJ_TEST("actor RPC retry failures preserve the asynchronous caller stack") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  PausingTimerChannel timer;
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = makeRetryFetcher(env, receiver, state, FailurePattern::DELIVERED, 1);
    auto global = jsg::JsObject(env.js.v8Context()->Global());
    auto& handler = KJ_REQUIRE_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<Fetcher>>());
    global.set(env.js, "fetcher"_kj, jsg::JsValue(handler.wrap(env.js, fetcher.addRef())));
    KJ_DEFER(global.delete_(env.js, "fetcher"_kj));
    auto result = jsg::NonModuleScript::compile(env.js, R"JS(
      async function rpcCaller() {
        await fetcher.echo(42);
      }
      rpcCaller().catch(error => {
        if (!error.stack.includes("rpcCaller")) throw new Error(error.stack);
      });
    )JS"_kj,
        "jsrpc-call-retry-stack-test.js"_kj)
                      .runAndReturn(env.js);
    return env.context.awaitJs(env.js, env.js.toPromise(result)).attach(kj::mv(fetcher));
  });
}

}  // namespace
}  // namespace workerd::api
