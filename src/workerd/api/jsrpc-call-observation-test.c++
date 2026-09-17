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

constexpr kj::StringPtr RECEIVER_SOURCE = R"JS(
  import { RpcTarget, WorkerEntrypoint } from "cloudflare:workers";

  class Child extends RpcTarget {
    echo(value) { return value; }
  }

  export default class extends WorkerEntrypoint {
    echo(value) { return value; }
    makeChild() { return new Child(); }
    get value() { return 42; }
    fail() { throw new Error("test failure"); }
    neverReturns() { return new Promise(() => {}); }
  }
)JS"_kj;

enum class TargetKind { ACTOR, NON_ACTOR };

enum class Settlement { PENDING, SUCCESS, FAILURE, CANCELED };

struct Observation {
  ActorCallPayloadReplayable payloadReplayable;
  ActorCallTargetRetryable targetRetryable;
  Settlement settlement = Settlement::PENDING;
};

struct ObservationState {
  kj::Vector<kj::Own<Observation>> observations;
  size_t replayMemoryBytes = 0;
  size_t peakReplayMemoryBytes = 0;
};

class RecordingCallObserver final: public OutgoingActorCallObserver {
 public:
  explicit RecordingCallObserver(Observation& observation): observation(observation) {}
  ~RecordingCallObserver() noexcept(false) {
    settle(Settlement::CANCELED);
  }

  void recordSuccess() override {
    settle(Settlement::SUCCESS);
  }
  void recordFailure(kj::Exception&) override {
    settle(Settlement::FAILURE);
  }

 private:
  void settle(Settlement settlement) {
    if (observation.settlement != Settlement::PENDING) return;
    observation.settlement = settlement;
  }

  Observation& observation;
};

class RecordingObserver final: public RequestObserver {
 public:
  explicit RecordingObserver(ObservationState& state): state(state) {}

  kj::Maybe<kj::Own<OutgoingActorCallObserver>> observeOutgoingActorRpcCall(
      ActorCallPayloadReplayable payloadReplayable,
      ActorCallTargetRetryable targetRetryable) override {
    auto& observation =
        *state.observations.add(kj::heap<Observation>(payloadReplayable, targetRetryable));
    return kj::heap<RecordingCallObserver>(observation);
  }

  kj::Own<void> trackActorCallReplayMemory(size_t bytes) override {
    state.replayMemoryBytes += bytes;
    state.peakReplayMemoryBytes = kj::max(state.peakReplayMemoryBytes, state.replayMemoryBytes);
    return kj::heap(kj::defer([&state = state, bytes]() { state.replayMemoryBytes -= bytes; }));
  }

 private:
  ObservationState& state;
};

class ReceiverOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  ReceiverOutgoingFactory(
      TestFixture& receiver, ActorCallTargetRetryable retryable, TargetKind targetKind)
      : receiver(receiver),
        retryable(retryable),
        targetKind(targetKind) {}

  // The next session opened through this factory reaches the receiver only once the returned
  // fulfiller is fulfilled, so a call started earlier can be made to settle later.
  kj::Own<kj::PromiseFulfiller<void>> holdNextSession() {
    auto paf = kj::newPromiseAndFulfiller<void>();
    pendingHold = kj::mv(paf.promise);
    return kj::mv(paf.fulfiller);
  }

  Result newSingleUseClient(kj::Maybe<kj::String>, MakeUserSpanParent) override {
    auto destination = receiver.makeWorkerEntrypoint();
    KJ_IF_SOME(hold, pendingHold) {
      auto client = kj::mv(hold).then(
          [destination = kj::mv(destination)]() mutable { return kj::mv(destination); });
      pendingHold = kj::none;
      return {.client = newPromisedWorkerInterface(kj::mv(client)), .spanParents = kj::none};
    }
    return {.client = kj::mv(destination), .spanParents = kj::none};
  }

  kj::Maybe<ActorCallTargetRetryable> getActorTargetRetryability() const override {
    if (targetKind == TargetKind::ACTOR) {
      return retryable;
    }
    return kj::none;
  }
  Result newActorCallAttempt(kj::Maybe<kj::String> cfStr,
      ActorCallRetryState::Attempt,
      MakeUserSpanParent makeUserSpanParent) override {
    return newSingleUseClient(kj::mv(cfStr), kj::mv(makeUserSpanParent));
  }

 private:
  TestFixture& receiver;
  ActorCallTargetRetryable retryable;
  TargetKind targetKind;
  kj::Maybe<kj::Promise<void>> pendingHold;
};

class Harness {
 public:
  Harness(): io(kj::setupAsyncIo()) {
    auto flags = flagsMessage.initRoot<CompatibilityFlags>();
    flags.setFetcherRpc(true);
    receiver = kj::heap<TestFixture>(TestFixture::SetupParams{
      .waitScope = io.waitScope,
      .mainModuleSource = RECEIVER_SOURCE,
      .useRealTimers = false,
    });
    sender = kj::heap<TestFixture>(TestFixture::SetupParams{
      .waitScope = io.waitScope,
      .featureFlags = flags.asReader(),
      .useRealTimers = false,
      .autogates = kj::arr("durable-object-retries-fetch"_kj, "durable-object-retries-jsrpc"_kj),
      .requestObserverFactory =
          kj::Function<kj::Own<RequestObserver>()>([this]() -> kj::Own<RequestObserver> {
      return kj::refcounted<RecordingObserver>(state);
    }),
    });
  }

  struct FetcherAndFactory {
    jsg::Ref<Fetcher> fetcher;
    ReceiverOutgoingFactory& factory;
  };

  FetcherAndFactory makeFetcher(const TestFixture::Environment& env,
      ActorCallTargetRetryable retryable = ActorCallTargetRetryable::YES,
      TargetKind targetKind = TargetKind::ACTOR) {
    auto factory = kj::heap<ReceiverOutgoingFactory>(*receiver, retryable, targetKind);
    auto& factoryRef = *factory;
    auto fetcher =
        env.js.alloc<Fetcher>(env.context.addObject<Fetcher::OutgoingFactory>(kj::mv(factory)),
            Fetcher::RequiresHostAndProtocol::YES);
    return {.fetcher = kj::mv(fetcher), .factory = factoryRef};
  }

  // Starts `fetcher.name(...args)` and returns the KJ side of its result promise.
  template <typename... Args>
  static kj::Promise<void> call(
      const TestFixture::Environment& env, jsg::Ref<JsRpcProperty> method, Args... args) {
    auto& handler = KJ_REQUIRE_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<JsRpcProperty>>());
    auto function = KJ_REQUIRE_NONNULL(
        jsg::JsValue(handler.wrap(env.js, kj::mv(method))).tryCast<jsg::JsFunction>());
    auto result = function.call(env.js, env.js.undefined(), args...);
    return env.context.awaitJs(env.js, env.js.toPromise(result)).ignoreResult();
  }

  template <typename... Args>
  static kj::Promise<void> call(
      const TestFixture::Environment& env, Fetcher& fetcher, kj::StringPtr name, Args... args) {
    return call(
        env, KJ_REQUIRE_NONNULL(fetcher.getRpcMethodForTestOnly(env.js, kj::str(name))), args...);
  }

  static kj::Promise<void> get(
      const TestFixture::Environment& env, Fetcher& fetcher, kj::StringPtr name) {
    auto property = KJ_REQUIRE_NONNULL(fetcher.getRpcMethodForTestOnly(env.js, kj::str(name)));
    auto& handler = KJ_REQUIRE_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<JsRpcProperty>>());
    auto result = jsg::JsValue(handler.wrap(env.js, kj::mv(property)));
    return env.context.awaitJs(env.js, env.js.toPromise(result)).ignoreResult();
  }

  static jsg::Ref<JsRpcPromise> startCall(
      const TestFixture::Environment& env, Fetcher& fetcher, kj::StringPtr name) {
    auto method = KJ_REQUIRE_NONNULL(fetcher.getRpcMethodForTestOnly(env.js, kj::str(name)));
    auto& handler = KJ_REQUIRE_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<JsRpcProperty>>());
    auto function = KJ_REQUIRE_NONNULL(
        jsg::JsValue(handler.wrap(env.js, kj::mv(method))).tryCast<jsg::JsFunction>());
    auto result =
        KJ_REQUIRE_NONNULL(function.call(env.js, env.js.undefined()).tryCast<jsg::JsObject>());
    return KJ_REQUIRE_NONNULL(result.tryUnwrapAs<JsRpcPromise>(env.js));
  }

  static jsg::Ref<JsRpcStub> makeStubRef(const TestFixture::Environment& env) {
    auto& targetHandler = KJ_REQUIRE_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<JsRpcTarget>>());
    auto target =
        KJ_REQUIRE_NONNULL(jsg::JsValue(targetHandler.wrap(env.js, env.js.alloc<JsRpcTarget>()))
                               .tryCast<jsg::JsObject>());
    return JsRpcStub::constructor(env.js, target);
  }

  static jsg::JsValue makeStub(const TestFixture::Environment& env) {
    auto& stubHandler = KJ_REQUIRE_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<JsRpcStub>>());
    return jsg::JsValue(stubHandler.wrap(env.js, makeStubRef(env)));
  }

  kj::AsyncIoContext io;
  capnp::MallocMessageBuilder flagsMessage;
  ObservationState state;
  kj::Own<TestFixture> receiver;
  kj::Own<TestFixture> sender;
};

KJ_TEST("concurrent RPC calls settling out of order keep their own observations") {
  Harness harness;
  harness.sender->runInIoContext([&](const TestFixture::Environment& env) {
    auto [fetcher, factory] = harness.makeFetcher(env);
    auto release = factory.holdNextSession();
    auto held = Harness::call(env, *fetcher, "echo"_kj, env.js.num(1));
    auto heldBytes = harness.state.replayMemoryBytes;
    auto immediate = Harness::call(env, *fetcher, "echo"_kj, Harness::makeStub(env));
    KJ_EXPECT(harness.state.replayMemoryBytes == heldBytes);
    return kj::mv(immediate)
        .then([&harness, release = kj::mv(release)]() mutable {
      auto& observations = harness.state.observations;
      KJ_ASSERT(observations.size() == 2);
      KJ_EXPECT(observations[0]->settlement == Settlement::PENDING);
      KJ_EXPECT(observations[1]->settlement == Settlement::SUCCESS);
      release->fulfill();
    }).then([held = kj::mv(held)]() mutable {
      return kj::mv(held);
    }).attach(kj::mv(fetcher));
  });

  auto& observations = harness.state.observations;
  KJ_ASSERT(observations.size() == 2);
  KJ_EXPECT(observations[0]->payloadReplayable == ActorCallPayloadReplayable::YES);
  KJ_EXPECT(observations[0]->targetRetryable == ActorCallTargetRetryable::YES);
  KJ_EXPECT(observations[0]->settlement == Settlement::SUCCESS);
  KJ_EXPECT(observations[1]->payloadReplayable == ActorCallPayloadReplayable::NO);
  KJ_EXPECT(observations[1]->targetRetryable == ActorCallTargetRetryable::YES);
  KJ_EXPECT(observations[1]->settlement == Settlement::SUCCESS);
  KJ_EXPECT(harness.state.replayMemoryBytes == 0);
}

KJ_TEST("an RPC call to an actor that cannot retry is observed as such") {
  Harness harness;
  harness.sender->runInIoContext([&](const TestFixture::Environment& env) {
    auto [fetcher, factory] = harness.makeFetcher(env, ActorCallTargetRetryable::NO);
    auto release = factory.holdNextSession();
    auto call = Harness::call(env, *fetcher, "echo"_kj, env.js.num(1));
    KJ_EXPECT(harness.state.replayMemoryBytes == 0);
    release->fulfill();
    return kj::mv(call).attach(kj::mv(fetcher));
  });

  auto& observations = harness.state.observations;
  KJ_ASSERT(observations.size() == 1);
  KJ_EXPECT(observations[0]->payloadReplayable == ActorCallPayloadReplayable::YES);
  KJ_EXPECT(observations[0]->targetRetryable == ActorCallTargetRetryable::NO);
  KJ_EXPECT(observations[0]->settlement == Settlement::SUCCESS);
}

KJ_TEST("concurrent replayable RPC calls track projected replay memory independently") {
  Harness harness;
  harness.sender->runInIoContext([&](const TestFixture::Environment& env) {
    auto [fetcher, factory] = harness.makeFetcher(env);
    auto releaseFirst = factory.holdNextSession();
    auto first = Harness::call(env, *fetcher, "echo"_kj, env.js.str("first"_kj));
    auto firstBytes = harness.state.replayMemoryBytes;
    auto releaseSecond = factory.holdNextSession();
    auto second = Harness::call(env, *fetcher, "echo"_kj,
        env.js.str("a payload large enough to produce a different serialized size"_kj));
    auto secondBytes = harness.state.replayMemoryBytes - firstBytes;

    KJ_EXPECT(firstBytes > JsRpcCallPlan::REPLAY_MEMORY_OVERHEAD);
    KJ_EXPECT(secondBytes > firstBytes);
    KJ_EXPECT(harness.state.peakReplayMemoryBytes == firstBytes + secondBytes);

    releaseFirst->fulfill();
    return kj::mv(first)
        .then([&harness, secondBytes, releaseSecond = kj::mv(releaseSecond)]() mutable {
      KJ_EXPECT(harness.state.replayMemoryBytes == secondBytes);
      releaseSecond->fulfill();
    }).then([second = kj::mv(second)]() mutable {
      return kj::mv(second);
    }).attach(kj::mv(fetcher));
  });

  KJ_EXPECT(harness.state.replayMemoryBytes == 0);
}

KJ_TEST("an RPC call through an actor result pipeline is observed as non-retryable") {
  Harness harness;
  harness.sender->runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = harness.makeFetcher(env).fetcher;
    auto child = Harness::startCall(env, *fetcher, "makeChild"_kj);
    auto echo = KJ_REQUIRE_NONNULL(child->getProperty(env.js, kj::str("echo")));
    return Harness::call(env, kj::mv(echo), env.js.num(1)).attach(kj::mv(child), kj::mv(fetcher));
  });

  auto& observations = harness.state.observations;
  KJ_ASSERT(observations.size() == 2);
  KJ_EXPECT(observations[0]->payloadReplayable == ActorCallPayloadReplayable::YES);
  KJ_EXPECT(observations[0]->targetRetryable == ActorCallTargetRetryable::YES);
  KJ_EXPECT(observations[0]->settlement == Settlement::SUCCESS);
  KJ_EXPECT(observations[1]->payloadReplayable == ActorCallPayloadReplayable::YES);
  KJ_EXPECT(observations[1]->targetRetryable == ActorCallTargetRetryable::NO);
  KJ_EXPECT(observations[1]->settlement == Settlement::SUCCESS);
}

KJ_TEST("using an RPC result pipeline releases projected replay memory") {
  Harness harness;
  harness.sender->runInIoContext([&](const TestFixture::Environment& env) {
    auto [fetcher, factory] = harness.makeFetcher(env);
    auto release = factory.holdNextSession();
    auto child = Harness::startCall(env, *fetcher, "makeChild"_kj);
    KJ_EXPECT(harness.state.replayMemoryBytes >= JsRpcCallPlan::REPLAY_MEMORY_OVERHEAD);

    auto echo = KJ_REQUIRE_NONNULL(child->getProperty(env.js, kj::str("echo")));
    auto pipelined = Harness::call(env, kj::mv(echo), env.js.num(1));
    KJ_EXPECT(harness.state.replayMemoryBytes == 0);

    release->fulfill();
    return kj::mv(pipelined).attach(kj::mv(child), kj::mv(fetcher));
  });

  KJ_EXPECT(harness.state.replayMemoryBytes == 0);
}

KJ_TEST("disposing an RPC promise does not release projected replay memory early") {
  Harness harness;
  harness.sender->runInIoContext([&](const TestFixture::Environment& env) {
    auto [fetcher, factory] = harness.makeFetcher(env);
    auto release = factory.holdNextSession();
    auto call = Harness::startCall(env, *fetcher, "echo"_kj);
    auto trackedBytes = harness.state.replayMemoryBytes;
    KJ_EXPECT(trackedBytes >= JsRpcCallPlan::REPLAY_MEMORY_OVERHEAD);

    call->dispose(env.js);
    KJ_EXPECT(harness.state.replayMemoryBytes == trackedBytes);

    return kj::Promise<void>(kj::READY_NOW).attach(kj::mv(call), kj::mv(fetcher), kj::mv(release));
  });

  KJ_EXPECT(harness.state.replayMemoryBytes == 0);
}

KJ_TEST("an RPC property get is observed with a non-replayable payload") {
  Harness harness;
  harness.sender->runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = harness.makeFetcher(env).fetcher;
    return Harness::get(env, *fetcher, "value"_kj).attach(kj::mv(fetcher));
  });

  auto& observations = harness.state.observations;
  KJ_ASSERT(observations.size() == 1);
  KJ_EXPECT(observations[0]->payloadReplayable == ActorCallPayloadReplayable::NO);
  KJ_EXPECT(observations[0]->targetRetryable == ActorCallTargetRetryable::YES);
  KJ_EXPECT(observations[0]->settlement == Settlement::SUCCESS);
}

KJ_TEST("a failed RPC call records failure") {
  Harness harness;
  harness.sender->runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = harness.makeFetcher(env).fetcher;
    return Harness::call(env, *fetcher, "fail"_kj).catch_([](kj::Exception&&) {
    }).attach(kj::mv(fetcher));
  });

  auto& observations = harness.state.observations;
  KJ_ASSERT(observations.size() == 1);
  KJ_EXPECT(observations[0]->settlement == Settlement::FAILURE);
}

KJ_TEST("RPC calls through a non-actor Fetcher are not observed as actor calls") {
  Harness harness;
  harness.sender->runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher =
        harness.makeFetcher(env, ActorCallTargetRetryable::NO, TargetKind::NON_ACTOR).fetcher;
    return Harness::call(env, *fetcher, "echo"_kj, env.js.num(1)).attach(kj::mv(fetcher));
  });

  KJ_EXPECT(harness.state.observations.empty());
}

KJ_TEST("RPC calls through a JsRpcStub are not observed as actor calls") {
  Harness harness;
  harness.sender->runInIoContext([&](const TestFixture::Environment& env) {
    auto stub = Harness::makeStubRef(env);
    auto method = KJ_REQUIRE_NONNULL(stub->getRpcMethod(env.js, kj::str("missing")));
    return Harness::call(env, kj::mv(method)).catch_([](kj::Exception&&) {});
  });

  KJ_EXPECT(harness.state.observations.empty());
}

KJ_TEST("an RPC call is observed when its result settles, not when its session ends") {
  Harness harness;
  harness.sender->runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = harness.makeFetcher(env).fetcher;
    auto method =
        KJ_REQUIRE_NONNULL(fetcher->getRpcMethodForTestOnly(env.js, kj::str("makeChild")));
    auto& handler = KJ_REQUIRE_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<JsRpcProperty>>());
    auto function = KJ_REQUIRE_NONNULL(
        jsg::JsValue(handler.wrap(env.js, kj::mv(method))).tryCast<jsg::JsFunction>());
    auto result = function.call(env.js, env.js.undefined());
    auto checked =
        env.js.toPromise(result).then(env.js, [&harness](jsg::Lock& js, jsg::Value value) {
      // The returned stub keeps the session alive while the call's observation is already settled.
      auto object = KJ_REQUIRE_NONNULL(jsg::JsValue(value.getHandle(js)).tryCast<jsg::JsObject>());
      auto child = KJ_REQUIRE_NONNULL(object.tryUnwrapAs<JsRpcStub>(js));
      KJ_ASSERT(harness.state.observations.size() == 1);
      KJ_EXPECT(harness.state.observations[0]->settlement == Settlement::SUCCESS);
    });
    return env.context.awaitJs(env.js, kj::mv(checked)).attach(kj::mv(fetcher));
  });
}

KJ_TEST("tearing down the caller records cancellation rather than a disconnect") {
  Harness harness;
  harness.sender->runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = harness.makeFetcher(env).fetcher;
    env.context.addTask(Harness::call(env, *fetcher, "neverReturns"_kj));
  });

  KJ_ASSERT(harness.state.observations.size() == 1);
  KJ_EXPECT(harness.state.observations[0]->settlement == Settlement::CANCELED);
  KJ_EXPECT(harness.state.replayMemoryBytes == 0);
}

}  // namespace
}  // namespace workerd::api
