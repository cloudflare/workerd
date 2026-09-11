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
    auto result = KJ_REQUIRE_NONNULL(
        function.call(env.js, env.js.undefined()).tryCast<jsg::JsObject>());
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
    auto immediate = Harness::call(env, *fetcher, "echo"_kj, Harness::makeStub(env));
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
}

KJ_TEST("an RPC call to an actor that cannot retry is observed as such") {
  Harness harness;
  harness.sender->runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = harness.makeFetcher(env, ActorCallTargetRetryable::NO).fetcher;
    return Harness::call(env, *fetcher, "echo"_kj, env.js.num(1)).attach(kj::mv(fetcher));
  });

  auto& observations = harness.state.observations;
  KJ_ASSERT(observations.size() == 1);
  KJ_EXPECT(observations[0]->payloadReplayable == ActorCallPayloadReplayable::YES);
  KJ_EXPECT(observations[0]->targetRetryable == ActorCallTargetRetryable::NO);
  KJ_EXPECT(observations[0]->settlement == Settlement::SUCCESS);
}

KJ_TEST("an RPC call through an actor result pipeline is observed as non-retryable") {
  Harness harness;
  harness.sender->runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = harness.makeFetcher(env).fetcher;
    auto child = Harness::startCall(env, *fetcher, "makeChild"_kj);
    auto echo = KJ_REQUIRE_NONNULL(child->getProperty(env.js, kj::str("echo")));
    return Harness::call(env, kj::mv(echo), env.js.num(1))
        .attach(kj::mv(child), kj::mv(fetcher));
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
    return Harness::call(env, *fetcher, "echo"_kj, env.js.num(42))
        .then([&harness]() {
      // The call's result pipeline is still held by its JsRpcPromise, so the session has not yet
      // ended when the observation must already have settled.
      KJ_ASSERT(harness.state.observations.size() == 1);
      KJ_EXPECT(harness.state.observations[0]->settlement == Settlement::SUCCESS);
    }).attach(kj::mv(fetcher));
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
}

}  // namespace
}  // namespace workerd::api
