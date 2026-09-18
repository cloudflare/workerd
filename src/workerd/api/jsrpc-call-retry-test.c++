// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "http.h"
#include "worker-rpc.h"

#include <workerd/tests/test-fixture.h>

#include <kj/async-io.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

enum class FirstFailure {
  AMBIGUOUS,
  NOT_DELIVERED,
};

// What the attempt after the failing ones connects to.
enum class Replacement {
  RECEIVER,
  HANGING,
};

// How a KJ DISCONNECTED exception surfaces to JS; its description is not exposed.
constexpr kj::StringPtr DISCONNECT_JS_MESSAGE = "Error: Network connection lost."_kj;

constexpr kj::StringPtr RECEIVER_SOURCE = R"JS(
  import { WorkerEntrypoint } from "cloudflare:workers";

  export default class extends WorkerEntrypoint {
    echo(value) { return value; }
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
  size_t replayMemoryBytes = 0;
  // `replayMemoryBytes` when the retry outcome was recorded, the first step after reentry.
  kj::Maybe<size_t> replayMemoryBytesAtOutcome;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> replacementStarted;
};

class ImmediateTimerChannel final: public TimerChannel {
 public:
  void syncTime() override {}

  kj::Date now(kj::Maybe<kj::Date>) override {
    return kj::UNIX_EPOCH;
  }

  kj::Promise<void> atTime(kj::Date) override {
    return kj::READY_NOW;
  }

  kj::Promise<void> afterLimitTimeout(kj::Duration) override {
    return kj::READY_NOW;
  }

  kj::TimePoint nowForLimitTimeout() override {
    return kj::origin<kj::TimePoint>();
  }
};

class PausingTimerChannel final: public TimerChannel {
 public:
  explicit PausingTimerChannel(uint pauseAtBackoff): pauseAtBackoff(pauseAtBackoff) {
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

class RetryCallObserver final: public OutgoingActorCallObserver {
 public:
  explicit RetryCallObserver(RetryTestState& state): state(state) {}

  void markPipelineCommitted() override {
    if (!pipelineCommitted) {
      pipelineCommitted = true;
      ++state.committedAttempts;
    }
  }

 private:
  RetryTestState& state;
  bool pipelineCommitted = false;
};

class RetryObserver final: public RequestObserver {
 public:
  explicit RetryObserver(RetryTestState& state): state(state) {}

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
    return trackMemory(bytes);
  }

 private:
  kj::Own<void> trackMemory(size_t bytes) {
    state.replayMemoryBytes += bytes;
    return kj::heap(kj::defer([&state = state, bytes]() { state.replayMemoryBytes -= bytes; }));
  }

  RetryTestState& state;
};

// A session that fails before delivering its event with the given disconnect.
kj::Own<WorkerInterface> newFailingSession(FirstFailure firstFailure) {
  auto exception = KJ_EXCEPTION(DISCONNECTED, "ambiguous JSRPC session failure");
  if (firstFailure == FirstFailure::NOT_DELIVERED) {
    jsg::markActorRequestNotDelivered(exception);
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
      FirstFailure firstFailure,
      uint failingAttempts,
      Replacement replacement)
      : receiver(receiver),
        state(state),
        firstFailure(firstFailure),
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
      return {.client = newFailingSession(firstFailure), .spanParents = kj::none};
    }
    KJ_IF_SOME(fulfiller, state.replacementStarted) {
      fulfiller->fulfill();
      state.replacementStarted = kj::none;
    }
    if (replacement == Replacement::HANGING) {
      return {.client = newHangingSession(), .spanParents = kj::none};
    }
    return {.client = receiver.makeWorkerEntrypoint(), .spanParents = kj::none};
  }

 private:
  TestFixture& receiver;
  RetryTestState& state;
  FirstFailure firstFailure;
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

TestFixture::SetupParams makeSenderParams(kj::WaitScope& waitScope,
    CompatibilityFlags::Reader flags,
    TimerChannel& timer,
    RetryTestState& state) {
  return {
    .waitScope = waitScope,
    .featureFlags = flags,
    .useRealTimers = false,
    .autogates =
        kj::arr("durable-object-retries-fetch"_kj, "durable-object-retries-fetch-retry-requests"_kj,
            "durable-object-retries-jsrpc"_kj, "durable-object-retries-jsrpc-retry-requests"_kj),
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
    FirstFailure firstFailure,
    uint failingAttempts,
    Replacement replacement = Replacement::RECEIVER) {
  return env.js.alloc<Fetcher>(
      env.context.addObject<Fetcher::OutgoingFactory>(kj::heap<RetryOutgoingFactory>(
          receiver, state, firstFailure, failingAttempts, replacement)),
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

KJ_TEST("replayable actor RPC retries an ambiguous session failure") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  ImmediateTimerChannel timer;
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = makeRetryFetcher(env, receiver, state, FirstFailure::AMBIGUOUS, 1);
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
  KJ_ASSERT(state.outcomes.size() == 1);
  KJ_EXPECT(state.outcomes[0] == ActorRetryOutcome::RECOVERED);
  // The payload is released at native success, before the call reenters the isolate.
  KJ_EXPECT(KJ_ASSERT_NONNULL(state.replayMemoryBytesAtOutcome) == 0);
  KJ_EXPECT(state.replayMemoryBytes == 0);
}

KJ_TEST("actor RPC retries a not-delivered failure with a fresh token") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  ImmediateTimerChannel timer;
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = makeRetryFetcher(env, receiver, state, FirstFailure::NOT_DELIVERED, 1);
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

KJ_TEST("pipelining before failure commits the actor RPC to its first attempt") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  ImmediateTimerChannel timer;
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = makeRetryFetcher(env, receiver, state, FirstFailure::AMBIGUOUS, 1);
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

KJ_TEST("pipelining during backoff prevents a replacement actor RPC attempt") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  PausingTimerChannel timer(1);
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = makeRetryFetcher(env, receiver, state, FirstFailure::AMBIGUOUS, 1);
    auto function = getRpcFunction(env.js, *fetcher, "echo"_kj);
    auto parentValue = function.call(env.js, env.js.undefined(), env.js.num(42));
    auto parentObject = KJ_REQUIRE_NONNULL(parentValue.tryCast<jsg::JsObject>());
    auto parent = KJ_REQUIRE_NONNULL(parentObject.tryUnwrapAs<JsRpcPromise>(env.js));

    auto parentRejected = env.context.awaitJs(env.js, expectDisconnect(env.js, parentValue));
    auto commitDuringBackoff = env.context.awaitIo(env.js, timer.onBackoffStarted(),
        [parent = parent.addRef(), &state](jsg::Lock& js) mutable {
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
  KJ_EXPECT(state.outcomes.empty());
  KJ_EXPECT(state.replayMemoryBytes == 0);
}

KJ_TEST("pipelining during a later backoff records a terminal actor RPC retry outcome") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  PausingTimerChannel timer(2);
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = makeRetryFetcher(env, receiver, state, FirstFailure::AMBIGUOUS, 2);
    auto function = getRpcFunction(env.js, *fetcher, "echo"_kj);
    auto parentValue = function.call(env.js, env.js.undefined(), env.js.num(42));
    auto parentObject = KJ_REQUIRE_NONNULL(parentValue.tryCast<jsg::JsObject>());
    auto parent = KJ_REQUIRE_NONNULL(parentObject.tryUnwrapAs<JsRpcPromise>(env.js));

    auto parentRejected = env.context.awaitJs(env.js, expectDisconnect(env.js, parentValue));
    auto commitDuringBackoff = env.context.awaitIo(env.js, timer.onBackoffStarted(),
        [parent = parent.addRef()](
            jsg::Lock& js) mutable { return expectDisconnect(js, callChild(js, *parent)); });
    auto childRejected = env.context.awaitJs(env.js, kj::mv(commitDuringBackoff));
    return kj::joinPromises(kj::arr(kj::mv(parentRejected), kj::mv(childRejected)))
        .attach(kj::mv(parent), kj::mv(fetcher));
  });

  KJ_EXPECT(state.metadata.size() == 2);
  KJ_EXPECT(state.acceptedRetries == 2);
  KJ_EXPECT(state.observedRetries == 1);
  KJ_ASSERT(state.outcomes.size() == 1);
  KJ_EXPECT(state.outcomes[0] == ActorRetryOutcome::UNABLE_TO_RETRY);
  KJ_EXPECT(state.replayMemoryBytes == 0);
}

KJ_TEST("dropping a committed replacement actor RPC attempt records cancellation") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  ImmediateTimerChannel timer;
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  auto replacementStarted = kj::newPromiseAndFulfiller<void>();
  state.replacementStarted = kj::mv(replacementStarted.fulfiller);

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher =
        makeRetryFetcher(env, receiver, state, FirstFailure::AMBIGUOUS, 1, Replacement::HANGING);
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

KJ_TEST("actor RPC property reads retry on a fresh session") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  RetryTestState state;
  ImmediateTimerChannel timer;
  TestFixture receiver(makeReceiverParams(io.waitScope));
  TestFixture sender(makeSenderParams(io.waitScope, makeRetryFlags(flagsMessage), timer, state));

  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = makeRetryFetcher(env, receiver, state, FirstFailure::AMBIGUOUS, 1);
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

}  // namespace
}  // namespace workerd::api
