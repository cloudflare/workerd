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

constexpr kj::StringPtr SENDER_SOURCE = R"JS(
  import { RpcStub, RpcTarget } from "cloudflare:workers";
  globalThis.makeStub = () => new RpcStub(new RpcTarget());
  export default {};
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
      .mainModuleSource = SENDER_SOURCE,
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

  static jsg::JsValue runScript(const TestFixture::Environment& env, kj::StringPtr source) {
    return jsg::NonModuleScript::compile(env.js, source, "jsrpc-call-observation-test.js"_kj)
        .runAndReturn(env.js);
  }

  static jsg::JsValue runScript(
      const TestFixture::Environment& env, jsg::Ref<Fetcher> fetcher, kj::StringPtr source) {
    auto global = jsg::JsObject(env.js.v8Context()->Global());
    auto& handler = KJ_REQUIRE_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<Fetcher>>());
    global.set(env.js, "fetcher"_kj, jsg::JsValue(handler.wrap(env.js, kj::mv(fetcher))));
    KJ_DEFER(global.delete_(env.js, "fetcher"_kj));
    return runScript(env, source);
  }

  static kj::Promise<void> awaitScript(
      const TestFixture::Environment& env, jsg::Ref<Fetcher> fetcher, kj::StringPtr source) {
    return env.context.awaitJs(env.js, env.js.toPromise(runScript(env, kj::mv(fetcher), source)))
        .ignoreResult();
  }

  static kj::Promise<void> awaitScript(const TestFixture::Environment& env, kj::StringPtr source) {
    return env.context.awaitJs(env.js, env.js.toPromise(runScript(env, source))).ignoreResult();
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
    auto held = Harness::awaitScript(env, fetcher.addRef(), "fetcher.echo(1)"_kj);
    auto immediate = Harness::awaitScript(env, kj::mv(fetcher), "fetcher.echo(makeStub())"_kj);
    return kj::mv(immediate)
        .then([&harness, release = kj::mv(release)]() mutable {
      auto& observations = harness.state.observations;
      KJ_ASSERT(observations.size() == 2);
      KJ_EXPECT(observations[0]->settlement == Settlement::PENDING);
      KJ_EXPECT(observations[1]->settlement == Settlement::SUCCESS);
      release->fulfill();
    }).then([held = kj::mv(held)]() mutable { return kj::mv(held); });
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
    return Harness::awaitScript(env, kj::mv(fetcher), "fetcher.echo(1)"_kj);
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
    return Harness::awaitScript(env, kj::mv(fetcher), "fetcher.makeChild().echo(1)"_kj);
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

KJ_TEST("actor RPC calls remain observed after their parent promise resolves") {
  Harness harness;
  harness.sender->runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = harness.makeFetcher(env).fetcher;
    return Harness::awaitScript(env, kj::mv(fetcher), R"JS(
      (async () => {
        const child = fetcher.makeChild();
        const beforeResolution = child.echo(1);
        await child;
        await beforeResolution;
        await child.echo(2);
      })()
    )JS"_kj);
  });

  auto& observations = harness.state.observations;
  KJ_ASSERT(observations.size() == 3);
  KJ_EXPECT(observations[0]->targetRetryable == ActorCallTargetRetryable::YES);
  KJ_EXPECT(observations[1]->targetRetryable == ActorCallTargetRetryable::NO);
  KJ_EXPECT(observations[2]->targetRetryable == ActorCallTargetRetryable::NO);
  for (auto& observation: observations) {
    KJ_EXPECT(observation->payloadReplayable == ActorCallPayloadReplayable::YES);
    KJ_EXPECT(observation->settlement == Settlement::SUCCESS);
  }
}

KJ_TEST("an RPC property get is observed with a non-replayable payload") {
  Harness harness;
  harness.sender->runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = harness.makeFetcher(env).fetcher;
    return Harness::awaitScript(env, kj::mv(fetcher), "fetcher.value"_kj);
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
    return Harness::awaitScript(env, kj::mv(fetcher), "fetcher.fail().catch(() => {})"_kj);
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
    return Harness::awaitScript(env, kj::mv(fetcher), "fetcher.echo(1)"_kj);
  });

  KJ_EXPECT(harness.state.observations.empty());
}

KJ_TEST("RPC calls through a JsRpcStub are not observed as actor calls") {
  Harness harness;
  harness.sender->runInIoContext([&](const TestFixture::Environment& env) {
    return Harness::awaitScript(env, "makeStub().missing().catch(() => {})"_kj);
  });

  KJ_EXPECT(harness.state.observations.empty());
}

KJ_TEST("an RPC call is observed when its result settles, not when its session ends") {
  Harness harness;
  harness.sender->runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = harness.makeFetcher(env).fetcher;
    auto result = Harness::runScript(env, kj::mv(fetcher), "fetcher.makeChild()"_kj);
    auto checked =
        env.js.toPromise(result).then(env.js, [&harness](jsg::Lock& js, jsg::Value value) {
      // The returned stub keeps the session alive while the call's observation is already settled.
      auto object = KJ_REQUIRE_NONNULL(jsg::JsValue(value.getHandle(js)).tryCast<jsg::JsObject>());
      auto child = KJ_REQUIRE_NONNULL(object.tryUnwrapAs<JsRpcStub>(js));
      KJ_ASSERT(harness.state.observations.size() == 1);
      KJ_EXPECT(harness.state.observations[0]->settlement == Settlement::SUCCESS);
    });
    return env.context.awaitJs(env.js, kj::mv(checked));
  });
}

KJ_TEST("tearing down the caller records cancellation rather than a disconnect") {
  Harness harness;
  harness.sender->runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = harness.makeFetcher(env).fetcher;
    env.context.addTask(Harness::awaitScript(env, kj::mv(fetcher), "fetcher.neverReturns()"_kj));
  });

  KJ_ASSERT(harness.state.observations.size() == 1);
  KJ_EXPECT(harness.state.observations[0]->settlement == Settlement::CANCELED);
}

}  // namespace
}  // namespace workerd::api
