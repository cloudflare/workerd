// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "basics.h"
#include "http.h"
#include "worker-rpc.h"

#include <workerd/tests/test-fixture.h>
#include <workerd/util/autogate.h>

#include <capnp/capability.h>
#include <kj/async-io.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

using Replayability = RpcSerializerExternalHandler::Replayability;

class CountingJsRpcTarget final: public rpc::JsRpcTarget::Server {
 public:
  explicit CountingJsRpcTarget(uint& callCount): callCount(callCount) {}

  kj::Promise<void> call(CallContext) override {
    ++callCount;
    return kj::READY_NOW;
  }

 private:
  uint& callCount;
};

class RecordingFailingOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  RecordingFailingOutgoingFactory(uint& singleUseCount,
      uint& actorAttemptCount,
      kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata>& metadata)
      : singleUseCount(singleUseCount),
        actorAttemptCount(actorAttemptCount),
        metadata(metadata) {}

  Result newSingleUseClient(kj::Maybe<kj::String>, MakeUserSpanParent) override {
    ++singleUseCount;
    KJ_FAIL_REQUIRE("destination failed");
  }

  kj::Maybe<ActorCallTargetRetryable> getActorTargetRetryability() const override {
    return ActorCallTargetRetryable::YES;
  }

  Result newActorCallAttempt(
      kj::Maybe<kj::String>, ActorCallRetryState::Attempt attempt, MakeUserSpanParent) override {
    ++actorAttemptCount;
    metadata = attempt.takeMetadata();
    KJ_FAIL_REQUIRE("destination failed");
  }

 private:
  uint& singleUseCount;
  uint& actorAttemptCount;
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata>& metadata;
};

class RejectingClaimObserver final: public RequestObserver {
 public:
  void claimRetryTokenBeforeUserCode() override {
    ++claimCount;
    auto exception = KJ_EXCEPTION(FAILED, "retry claim rejected");
    exception.setDetail(jsg::ACTOR_RETRY_CLAIM_REJECTED_DETAIL_ID, kj::heapArray<kj::byte>(0));
    kj::throwFatalException(kj::mv(exception));
  }

  void delivered() override {
    ++deliveredCount;
  }

  uint claimCount = 0;
  uint deliveredCount = 0;
};

capnp::Capability::Client brokenCap() {
  return capnp::Capability::Client(KJ_EXCEPTION(FAILED, "test cap"));
}

jsg::JsRef<jsg::JsFunction> wrapMethod(jsg::Lock& js, jsg::Ref<JsRpcProperty> method) {
  auto& handler = KJ_REQUIRE_NONNULL(js.tryGetTypeHandler<jsg::Ref<JsRpcProperty>>());
  auto function =
      KJ_REQUIRE_NONNULL(jsg::JsValue(handler.wrap(js, kj::mv(method))).tryCast<jsg::JsFunction>());
  return jsg::JsRef<jsg::JsFunction>(js, function);
}

jsg::JsRef<jsg::JsFunction> wrapMethod(jsg::Lock& js, Fetcher& fetcher, kj::StringPtr name) {
  return wrapMethod(js, KJ_REQUIRE_NONNULL(fetcher.getRpcMethodForTestOnly(js, kj::str(name))));
}

template <typename Func>
JsRpcCallPlan makePlan(Func populate,
    kj::Array<const byte> serializedData = kj::heapArray<const byte>(0),
    Replayability replayability = Replayability::REPLAYABLE) {
  auto message = kj::heap<capnp::MallocMessageBuilder>(
      JsRpcCallPlan::METADATA_SEGMENT_WORDS, capnp::AllocationStrategy::FIXED_SIZE);
  populate(message->initRoot<rpc::JsRpcTarget::CallParams>());
  return JsRpcCallPlan(kj::mv(message), kj::mv(serializedData), replayability);
}

// Builds a plan for a call whose arguments produced `externalHandler`'s externals, so that a test
// asserts on the plan's classification of them rather than on serializer-side state.
JsRpcCallPlan makePlanWithExternals(RpcSerializerExternalHandler& externalHandler) {
  return makePlan([&](rpc::JsRpcTarget::CallParams::Builder builder) {
    auto args = builder.getOperation().initCallWithArgs();
    args.adoptExternals(externalHandler.build(capnp::Orphanage::getForMessageContaining(args)));
  }, kj::heapArray<const byte>(1), externalHandler.getReplayability());
}

KJ_TEST("JS RPC call plan copies calls and property accesses") {
  auto noArgs = makePlan(
      [](rpc::JsRpcTarget::CallParams::Builder builder) { builder.setMethodName("ping"); });
  KJ_EXPECT(noArgs.getReplayable());
  capnp::MallocMessageBuilder noArgsAttempt;
  noArgs.copyTo(noArgsAttempt.initRoot<rpc::JsRpcTarget::CallParams>());
  auto noArgsParams = noArgsAttempt.getRoot<rpc::JsRpcTarget::CallParams>();
  KJ_EXPECT(noArgsParams.getMethodName() == "ping");
  KJ_EXPECT(noArgsParams.getOperation().isCallWithArgs());
  KJ_EXPECT(!noArgsParams.getOperation().hasCallWithArgs());

  auto property = makePlan([](rpc::JsRpcTarget::CallParams::Builder builder) {
    builder.setMethodName("value");
    builder.getOperation().setGetProperty();
  });
  KJ_EXPECT(!property.getReplayable());
  capnp::MallocMessageBuilder propertyAttempt;
  property.copyTo(propertyAttempt.initRoot<rpc::JsRpcTarget::CallParams>());
  auto propertyParams = propertyAttempt.getRoot<rpc::JsRpcTarget::CallParams>();
  KJ_EXPECT(propertyParams.getMethodName() == "value");
  KJ_EXPECT(propertyParams.getOperation().isGetProperty());

  static constexpr byte SERIALIZED[] = {1, 2, 3, 4};
  auto plain = makePlan([](rpc::JsRpcTarget::CallParams::Builder builder) {
    auto path = builder.initMethodPath(3);
    path.set(0, "foo");
    path.set(1, "bar");
    path.set(2, "baz");
    builder.getOperation().initCallWithArgs();
  }, kj::heapArray<const byte>(kj::arrayPtr(SERIALIZED)));
  KJ_EXPECT(plain.getReplayable());

  for (uint attempt = 0; attempt < 2; ++attempt) {
    capnp::MallocMessageBuilder attemptMessage;
    plain.copyTo(attemptMessage.initRoot<rpc::JsRpcTarget::CallParams>());
    auto params = attemptMessage.getRoot<rpc::JsRpcTarget::CallParams>();
    KJ_EXPECT(params.getMethodPath().size() == 3);
    KJ_EXPECT(params.getMethodPath()[0] == "foo");
    KJ_EXPECT(params.getMethodPath()[1] == "bar");
    KJ_EXPECT(params.getMethodPath()[2] == "baz");
    auto data = params.getOperation().getCallWithArgs().getV8Serialized();
    KJ_EXPECT(data == kj::arrayPtr(SERIALIZED));
  }
}

KJ_TEST("JS RPC call plan copies usable external capabilities but rejects replay") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);
  uint callCount = 0;
  auto plan = makePlan([&](rpc::JsRpcTarget::CallParams::Builder builder) {
    auto args = builder.getOperation().initCallWithArgs();
    auto external = args.initExternals(1)[0].initRpcTarget();
    external.setCap(kj::heap<CountingJsRpcTarget>(callCount));
  }, kj::heapArray<const byte>(1), Replayability::INELIGIBLE);
  KJ_EXPECT(!plan.getReplayable());

  for (uint attempt = 0; attempt < 2; ++attempt) {
    capnp::MallocMessageBuilder attemptMessage;
    plan.copyTo(attemptMessage.initRoot<rpc::JsRpcTarget::CallParams>());
    auto externals = attemptMessage.getRoot<rpc::JsRpcTarget::CallParams>()
                         .getOperation()
                         .getCallWithArgs()
                         .getExternals();
    KJ_EXPECT(externals.size() == 1);
    KJ_EXPECT(externals[0].isRpcTarget());
    auto request = externals[0].getRpcTarget().getCap().callRequest();
    request.send().wait(waitScope);
  }
  KJ_EXPECT(callCount == 2);
}

KJ_TEST("JS RPC call plan rejects replay for every external kind") {
  // The serializer reports REPLAYABLE so that the plan's own classifier is what rejects each kind.
  auto expectIneligible = [](auto populate) {
    auto plan = makePlan([&](rpc::JsRpcTarget::CallParams::Builder builder) {
      auto external = builder.getOperation().initCallWithArgs().initExternals(1)[0];
      populate(external);
    }, kj::heapArray<const byte>(1), Replayability::REPLAYABLE);
    KJ_EXPECT(!plan.getReplayable());
  };

  expectIneligible([](auto external) { external.setInvalid(); });
  expectIneligible([](auto external) { external.initRpcTarget(); });
  expectIneligible([](auto external) { external.initWritableStream(); });
  expectIneligible([](auto external) { external.initReadableStream(); });
  expectIneligible([](auto external) { external.setObsolete7(); });
  expectIneligible([](auto external) {
    external.setAbortSignal(brokenCap().castAs<rpc::JsValue::ExternalPusher::AbortSignal>());
  });
  expectIneligible([](auto external) { external.initSubrequestChannelToken(0); });
  expectIneligible([](auto external) { external.initActorClassChannelToken(0); });
  expectIneligible([](auto external) {
    external.setDelayedSubrequestChannelToken(
        brokenCap().castAs<rpc::JsValue::ExternalPusher::DelayedChannelToken>());
  });
  expectIneligible([](auto external) {
    external.setDelayedActorClassChannelToken(
        brokenCap().castAs<rpc::JsValue::ExternalPusher::DelayedChannelToken>());
  });
  expectIneligible([](auto external) { external.initSocket(); });
}

KJ_TEST("JS RPC call plan honors serializer ineligibility without an external entry") {
  auto plan = makePlan([](rpc::JsRpcTarget::CallParams::Builder builder) {
    builder.getOperation().initCallWithArgs();
  }, kj::heapArray<const byte>(1), Replayability::INELIGIBLE);
  KJ_EXPECT(!plan.getReplayable());
}

KJ_TEST("native RPC stub serialization is replay-ineligible in both ownership modes") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    for (auto stubOwnership:
        {RpcSerializerExternalHandler::DUPLICATE, RpcSerializerExternalHandler::TRANSFER}) {
      auto cap = brokenCap().castAs<rpc::JsRpcTarget>();
      auto stub = env.js.alloc<JsRpcStub>(env.context.addObject(kj::heap(kj::mv(cap))), kj::none);
      RpcSerializerExternalHandler externalHandler(
          stubOwnership, brokenCap().castAs<rpc::JsValue::ExternalPusher>(), kj::none);
      jsg::Serializer serializer(env.js, {.externalHandler = externalHandler});
      stub->serialize(env.js, serializer);
      KJ_EXPECT(externalHandler.size() == 1);
      KJ_EXPECT(externalHandler.getReplayability() == Replayability::REPLAYABLE);
      KJ_EXPECT(!makePlanWithExternals(externalHandler).getReplayable());
    }
  });
}

KJ_TEST("JavaScript RPC targets, functions, and proxies dup() once and stay ineligible") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& targetHandler = KJ_REQUIRE_NONNULL(
        env.js.tryGetTypeHandler<jsg::Ref<JsRpcTarget>>(), "JsRpcTarget type handler is missing");
    auto makeTarget = [&]() {
      return KJ_REQUIRE_NONNULL(
          jsg::JsValue(targetHandler.wrap(env.js, env.js.alloc<JsRpcTarget>()))
              .tryCast<jsg::JsObject>());
    };
    auto expectSingleIneligibleExternal = [&](jsg::JsValue value) {
      RpcSerializerExternalHandler externalHandler(RpcSerializerExternalHandler::DUPLICATE,
          brokenCap().castAs<rpc::JsValue::ExternalPusher>(), kj::none);
      jsg::Serializer serializer(env.js, {.externalHandler = externalHandler});
      serializer.write(env.js, value);
      serializer.release();
      KJ_EXPECT(externalHandler.size() == 1);
      KJ_EXPECT(externalHandler.getReplayability() == Replayability::REPLAYABLE);
      KJ_EXPECT(!makePlanWithExternals(externalHandler).getReplayable());
    };
    auto addDup = [&](jsg::JsObject object, uint& dupCount) {
      auto ref = jsg::JsRef<jsg::JsObject>(env.js, object);
      object.set(env.js, "dup"_kj,
          jsg::JsValue(env.js.wrapReturningFunction(env.js.v8Context(),
              [&dupCount, ref = kj::mv(ref)](
                  jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>&) mutable {
        ++dupCount;
        return v8::Local<v8::Value>(ref.getHandle(js));
      })));
    };

    // A target without dup() falls back to taking ownership.
    expectSingleIneligibleExternal(jsg::JsValue(makeTarget()));

    uint dupCount = 0;
    auto original = makeTarget();
    addDup(original, dupCount);
    expectSingleIneligibleExternal(jsg::JsValue(original));
    KJ_EXPECT(dupCount == 1);

    // Destination failure is checked before dup() can consume the source.
    RpcSerializerExternalHandler failingHandler(RpcSerializerExternalHandler::DUPLICATE,
        RpcSerializerExternalHandler::GetExternalPusher(
            []() { return brokenCap().castAs<rpc::JsValue::ExternalPusher>(); }),
        RpcSerializerExternalHandler::ResolveDestinationAndGetSpanParents(
            []() -> kj::Maybe<TraceContextParent> { KJ_FAIL_REQUIRE("destination failed"); }));
    jsg::Serializer failingSerializer(env.js, {.externalHandler = failingHandler});
    bool threw = false;
    KJ_EXPECT_LOG(ERROR, "destination failed");
    JSG_TRY(env.js) {
      failingSerializer.write(env.js, jsg::JsValue(original));
    }
    JSG_CATCH(exception KJ_UNUSED) {
      threw = true;
    }
    KJ_EXPECT(threw);
    KJ_EXPECT(dupCount == 1);

    uint functionDupCount = 0;
    auto function = jsg::JsObject(env.js.wrapReturningFunction(
        env.js.v8Context(), [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>&) {
      return v8::Local<v8::Value>(js.undefined());
    }));
    addDup(function, functionDupCount);
    expectSingleIneligibleExternal(jsg::JsValue(function));
    KJ_EXPECT(functionDupCount == 1);

    uint proxyDupCount = 0;
    auto proxyTarget = makeTarget();
    addDup(proxyTarget, proxyDupCount);
    auto proxy = jsg::check(v8::Proxy::New(env.js.v8Context(), v8::Local<v8::Object>(proxyTarget),
        v8::Local<v8::Object>(env.js.obj())));
    expectSingleIneligibleExternal(jsg::JsValue(proxy));
    KJ_EXPECT(proxyDupCount == 1);
  });
}

KJ_TEST("RPC stub transfer waits for destination resolution") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto cap = brokenCap().castAs<rpc::JsRpcTarget>();
    auto stub = env.js.alloc<JsRpcStub>(env.context.addObject(kj::heap(kj::mv(cap))), kj::none);
    RpcSerializerExternalHandler externalHandler(RpcSerializerExternalHandler::TRANSFER,
        RpcSerializerExternalHandler::GetExternalPusher(
            []() { return brokenCap().castAs<rpc::JsValue::ExternalPusher>(); }),
        RpcSerializerExternalHandler::ResolveDestinationAndGetSpanParents(
            []() -> kj::Maybe<TraceContextParent> { KJ_FAIL_REQUIRE("destination failed"); }));
    jsg::Serializer serializer(env.js, {.externalHandler = externalHandler});

    KJ_EXPECT_THROW_MESSAGE("destination failed", stub->serialize(env.js, serializer));
    auto duplicate KJ_UNUSED = stub->dup(env.js);
  });
}

KJ_TEST("retry-capable RPC calls resolve the destination only after safe serialization") {
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setFetcherRpc(true);
  flags.setRpcParamsDupStubs(false);
  TestFixture fixture({.featureFlags = flags.asReader(),
    .autogates = kj::arr("durable-object-retries-fetch"_kj,
        "durable-object-retries-fetch-retry-requests"_kj, "durable-object-retries-jsrpc"_kj)});

  uint singleUseCount = 0;
  uint actorAttemptCount = 0;
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> metadata;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = env.js.alloc<Fetcher>(
        env.context.addObject<Fetcher::OutgoingFactory>(
            kj::heap<RecordingFailingOutgoingFactory>(singleUseCount, actorAttemptCount, metadata)),
        Fetcher::RequiresHostAndProtocol::YES);
    auto method = wrapMethod(env.js, *fetcher, "method"_kj);
    auto startRejectedCall = [&](v8::Local<v8::Value> arg) {
      v8::LocalVector<v8::Value> args(env.js.v8Isolate);
      args.push_back(arg);
      auto result =
          jsg::JsFunction(method.getHandle(env.js)).call(env.js, env.js.undefined(), args);
      return env.context.awaitJs(env.js, env.js.toPromise(result)).ignoreResult().then([]() {
        KJ_FAIL_ASSERT("RPC call unexpectedly succeeded");
      }, [](kj::Exception&&) {});
    };

    auto serializationFailure =
        startRejectedCall(v8::Local<v8::Value>(v8::Symbol::New(env.js.v8Isolate)));
    KJ_EXPECT(singleUseCount == 0);
    KJ_EXPECT(actorAttemptCount == 0);

    auto cap = brokenCap().castAs<rpc::JsRpcTarget>();
    auto stub = env.js.alloc<JsRpcStub>(env.context.addObject(kj::heap(kj::mv(cap))), kj::none);
    auto& stubHandler = KJ_REQUIRE_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<JsRpcStub>>());
    KJ_EXPECT_LOG(ERROR, "destination failed");
    auto destinationFailure = startRejectedCall(stubHandler.wrap(env.js, stub.addRef()));
    KJ_EXPECT(singleUseCount == 1);
    KJ_EXPECT(actorAttemptCount == 0);
    KJ_EXPECT(metadata == kj::none);
    auto duplicate KJ_UNUSED = stub->dup(env.js);

    return kj::joinPromises(kj::arr(kj::mv(serializationFailure), kj::mv(destinationFailure)))
        .attach(kj::mv(method), kj::mv(stub), kj::mv(fetcher));
  });
}

constexpr kj::StringPtr RECEIVER_SOURCE = R"JS(
  import { RpcTarget, WorkerEntrypoint } from "cloudflare:workers";

  class Counter extends RpcTarget {
    constructor(value) {
      super();
      this.value = value;
    }

    increment(amount) { return this.value += amount; }
  }

  export default class extends WorkerEntrypoint {
    echo(value) { return value.x; }
    get nested() { return { echo(value) { return value.x; } }; }
    makeCounter(value) { return new Counter(value); }
  }
)JS"_kj;

class ReceiverOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  explicit ReceiverOutgoingFactory(TestFixture& receiver): receiver(receiver) {}

  Result newSingleUseClient(kj::Maybe<kj::String>, MakeUserSpanParent) override {
    return {.client = receiver.makeWorkerEntrypoint(), .spanParents = kj::none};
  }

  kj::Maybe<ActorCallTargetRetryable> getActorTargetRetryability() const override {
    return ActorCallTargetRetryable::YES;
  }

  Result newActorCallAttempt(kj::Maybe<kj::String> cfStr,
      ActorCallRetryState::Attempt,
      MakeUserSpanParent makeUserSpanParent) override {
    return newSingleUseClient(kj::mv(cfStr), kj::mv(makeUserSpanParent));
  }

 private:
  TestFixture& receiver;
};

class GatedReceiverOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  GatedReceiverOutgoingFactory(
      TestFixture& receiver, kj::Promise<void> gate, uint& destinationCallCount)
      : receiver(receiver),
        gate(gate.fork()),
        destinationCallCount(destinationCallCount) {}

  Result newSingleUseClient(kj::Maybe<kj::String>, MakeUserSpanParent) override {
    ++destinationCallCount;
    auto destination = receiver.makeWorkerEntrypoint();
    return {
      .client = newPromisedWorkerInterface(gate.addBranch().then(
          [destination = kj::mv(destination)]() mutable { return kj::mv(destination); })),
      .spanParents = kj::none,
    };
  }

  kj::Maybe<ActorCallTargetRetryable> getActorTargetRetryability() const override {
    return ActorCallTargetRetryable::YES;
  }

  Result newActorCallAttempt(kj::Maybe<kj::String> cfStr,
      ActorCallRetryState::Attempt,
      MakeUserSpanParent makeUserSpanParent) override {
    return newSingleUseClient(kj::mv(cfStr), kj::mv(makeUserSpanParent));
  }

 private:
  TestFixture& receiver;
  kj::ForkedPromise<void> gate;
  uint& destinationCallCount;
};

KJ_TEST("the sender serializes a structured-clone argument once per call") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setFetcherRpc(true);
  TestFixture receiver(TestFixture::SetupParams{
    .waitScope = io.waitScope,
    .mainModuleSource = RECEIVER_SOURCE,
    .useRealTimers = false,
  });
  TestFixture sender(TestFixture::SetupParams{
    .waitScope = io.waitScope,
    .featureFlags = flags.asReader(),
    .useRealTimers = false,
  });

  uint getterCount = 0;
  uint resultCount = 0;
  sender.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = env.js.alloc<Fetcher>(env.context.addObject<Fetcher::OutgoingFactory>(
                                             kj::heap<ReceiverOutgoingFactory>(receiver)),
        Fetcher::RequiresHostAndProtocol::YES);

    // Serializing `arg` reads `x` through this getter, so the count is the serialization count.
    auto arg = env.js.obj();
    auto getter = env.js.wrapReturningFunction(env.js.v8Context(),
        [&getterCount](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>&) {
      ++getterCount;
      return v8::Local<v8::Value>(js.num(42));
    });
    v8::Local<v8::Object>(arg)->SetAccessorProperty(
        v8::Local<v8::Name>(v8::Local<v8::String>(env.js.str("x"_kj))), getter);

    auto call = [&](jsg::Ref<JsRpcProperty> method) {
      v8::LocalVector<v8::Value> args(env.js.v8Isolate);
      args.push_back(arg);
      auto function = wrapMethod(env.js, kj::mv(method));
      auto result =
          jsg::JsFunction(function.getHandle(env.js)).call(env.js, env.js.undefined(), args);
      auto checked =
          env.js.toPromise(result).then(env.js, [&resultCount](jsg::Lock& js, jsg::Value value) {
        KJ_EXPECT(jsg::JsValue(value.getHandle(js)).strictEquals(js.num(42)));
        ++resultCount;
      });
      return env.context.awaitJs(env.js, kj::mv(checked));
    };

    auto direct =
        call(KJ_REQUIRE_NONNULL(fetcher->getRpcMethodForTestOnly(env.js, kj::str("echo"))));
    KJ_EXPECT(getterCount == 1);
    auto nested = KJ_REQUIRE_NONNULL(fetcher->getRpcMethodForTestOnly(env.js, kj::str("nested")));
    auto viaPath = call(KJ_REQUIRE_NONNULL(nested->getProperty(env.js, kj::str("echo"))));
    KJ_EXPECT(getterCount == 2);

    return kj::joinPromises(kj::arr(kj::mv(direct), kj::mv(viaPath))).attach(kj::mv(fetcher));
  });
  KJ_EXPECT(getterCount == 2);
  KJ_EXPECT(resultCount == 2);
}

KJ_TEST("an unresolved retry-capable destination supports promise pipelining") {
  auto io = kj::setupAsyncIo();
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setFetcherRpc(true);
  TestFixture receiver(TestFixture::SetupParams{
    .waitScope = io.waitScope,
    .mainModuleSource = RECEIVER_SOURCE,
    .useRealTimers = false,
  });
  TestFixture sender(TestFixture::SetupParams{
    .waitScope = io.waitScope,
    .featureFlags = flags.asReader(),
    .useRealTimers = false,
  });

  auto gate = kj::newPromiseAndFulfiller<void>();
  uint destinationCallCount = 0;
  sender.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto fetcher = env.js.alloc<Fetcher>(
        env.context.addObject<Fetcher::OutgoingFactory>(kj::heap<GatedReceiverOutgoingFactory>(
            receiver, kj::mv(gate.promise), destinationCallCount)),
        Fetcher::RequiresHostAndProtocol::YES);

    v8::LocalVector<v8::Value> makeCounterArgs(env.js.v8Isolate);
    makeCounterArgs.push_back(env.js.num(12));
    auto makeCounter = wrapMethod(env.js, *fetcher, "makeCounter"_kj);
    auto counterValue = jsg::JsFunction(makeCounter.getHandle(env.js))
                            .call(env.js, env.js.undefined(), makeCounterArgs);
    auto& promiseHandler = KJ_REQUIRE_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<JsRpcPromise>>());
    auto counter = KJ_REQUIRE_NONNULL(promiseHandler.tryUnwrap(env.js, counterValue));

    v8::LocalVector<v8::Value> incrementArgs(env.js.v8Isolate);
    incrementArgs.push_back(env.js.num(3));
    auto increment =
        wrapMethod(env.js, KJ_REQUIRE_NONNULL(counter->getProperty(env.js, kj::str("increment"))));
    auto incrementValue = jsg::JsFunction(increment.getHandle(env.js))
                              .call(env.js, env.js.undefined(), incrementArgs);
    auto checked =
        env.js.toPromise(incrementValue).then(env.js, [](jsg::Lock& js, jsg::Value value) {
      KJ_EXPECT(jsg::JsValue(value.getHandle(js)).strictEquals(js.num(15)));
    });
    KJ_EXPECT(destinationCallCount == 1);
    gate.fulfiller->fulfill();
    return env.context.awaitJs(env.js, kj::mv(checked))
        .attach(kj::mv(increment), kj::mv(counter), kj::mv(makeCounter), kj::mv(fetcher));
  });

  KJ_EXPECT(destinationCallCount == 1);
}

// Makes one failing replayable actor RPC call, recording how the sender dispatched it.
struct ActorCallDispatch {
  uint singleUseCount = 0;
  uint actorAttemptCount = 0;
  kj::Maybe<IoChannelFactory::ActorRetryRequestMetadata> metadata;
};
ActorCallDispatch makeReplayableActorCall(kj::ArrayPtr<const kj::StringPtr> autogates) {
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setFetcherRpc(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  // Exactly these gates, even under WORKERD_ALL_AUTOGATES: the gate-off cases assert on absence.
  util::Autogate::initAutogateNamesForTest(autogates, util::IgnoreAllAutogatesEnv::YES);

  ActorCallDispatch dispatch;
  auto& singleUseCount = dispatch.singleUseCount;
  auto& actorAttemptCount = dispatch.actorAttemptCount;
  auto& metadata = dispatch.metadata;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = env.js.alloc<Fetcher>(
        env.context.addObject<Fetcher::OutgoingFactory>(
            kj::heap<RecordingFailingOutgoingFactory>(singleUseCount, actorAttemptCount, metadata)),
        Fetcher::RequiresHostAndProtocol::YES);
    auto method = wrapMethod(env.js, *fetcher, "method"_kj);
    v8::LocalVector<v8::Value> args(env.js.v8Isolate);
    args.push_back(v8::Number::New(env.js.v8Isolate, 123));
    KJ_EXPECT_LOG(ERROR, "destination failed");
    auto result = jsg::JsFunction(method.getHandle(env.js)).call(env.js, env.js.undefined(), args);
    return env.context.awaitJs(env.js, env.js.toPromise(result)).ignoreResult().then([]() {
      KJ_FAIL_ASSERT("RPC call unexpectedly succeeded");
    }, [](kj::Exception&&) {});
  });
  return dispatch;
}

ActorCallDispatch makeActorPropertyRead(kj::ArrayPtr<const kj::StringPtr> autogates) {
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setFetcherRpc(true);
  TestFixture fixture({.featureFlags = flags.asReader()});
  util::Autogate::initAutogateNamesForTest(autogates, util::IgnoreAllAutogatesEnv::YES);

  ActorCallDispatch dispatch;
  auto& singleUseCount = dispatch.singleUseCount;
  auto& actorAttemptCount = dispatch.actorAttemptCount;
  auto& metadata = dispatch.metadata;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher = env.js.alloc<Fetcher>(
        env.context.addObject<Fetcher::OutgoingFactory>(
            kj::heap<RecordingFailingOutgoingFactory>(singleUseCount, actorAttemptCount, metadata)),
        Fetcher::RequiresHostAndProtocol::YES);
    auto property = KJ_REQUIRE_NONNULL(fetcher->getRpcMethodForTestOnly(env.js, kj::str("value")));
    auto handler = env.js.wrapReturningFunction(
        env.js.v8Context(), [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>&) {
      return v8::Local<v8::Value>(js.undefined());
    });
    KJ_EXPECT_LOG(ERROR, "destination failed");
    auto result = property->then(env.js, handler, {});
    return env.context.awaitJs(env.js, env.js.toPromise(result)).ignoreResult().then([]() {
      KJ_FAIL_ASSERT("RPC property read unexpectedly succeeded");
    }, [](kj::Exception&&) {});
  });
  return dispatch;
}

KJ_TEST("replayable actor RPC calls carry observe-only retry metadata") {
  auto dispatch = makeReplayableActorCall(kj::arr("durable-object-retries-fetch"_kj,
      "durable-object-retries-fetch-retry-requests"_kj, "durable-object-retries-jsrpc"_kj));

  KJ_EXPECT(dispatch.singleUseCount == 0);
  KJ_EXPECT(dispatch.actorAttemptCount == 1);
  auto recordedMetadata = KJ_ASSERT_NONNULL(dispatch.metadata);
  KJ_EXPECT(recordedMetadata.isRetry == IsActorRetry::NO);
  KJ_EXPECT(recordedMetadata.retryGateEnabled == ActorRetryGateEnabled::NO);
}

KJ_TEST("replayable actor RPC calls carry no retry metadata without the JSRPC gate") {
  auto dispatch = makeReplayableActorCall(
      kj::arr("durable-object-retries-fetch"_kj, "durable-object-retries-fetch-retry-requests"_kj));

  KJ_EXPECT(dispatch.singleUseCount == 1);
  KJ_EXPECT(dispatch.actorAttemptCount == 0);
  KJ_EXPECT(dispatch.metadata == kj::none);
}

KJ_TEST("replayable actor RPC calls carry no retry metadata without the fetch gate") {
  auto dispatch = makeReplayableActorCall(kj::arr("durable-object-retries-jsrpc"_kj));

  KJ_EXPECT(dispatch.singleUseCount == 1);
  KJ_EXPECT(dispatch.actorAttemptCount == 0);
  KJ_EXPECT(dispatch.metadata == kj::none);
}

KJ_TEST("actor RPC property reads carry no retry metadata") {
  auto dispatch = makeActorPropertyRead(
      kj::arr("durable-object-retries-fetch"_kj, "durable-object-retries-jsrpc"_kj));

  KJ_EXPECT(dispatch.singleUseCount == 1);
  KJ_EXPECT(dispatch.actorAttemptCount == 0);
  KJ_EXPECT(dispatch.metadata == kj::none);
}

// A Durable Object whose methods fail in the ways the receiver must classify as delivered.
constexpr kj::StringPtr ACTOR_SOURCE = R"JS(
  import { DurableObject } from "cloudflare:workers";
  export default class extends DurableObject {
    fail() { throw new Error("method failed"); }
    get failingGetter() { throw new Error("getter failed"); }
    // An outbound disconnect reaches user code as an Error with `retryable` set; rethrowing it
    // is how such a failure propagates through a method.
    rethrowDisconnect() {
      const error = new Error("outbound disconnected");
      error.retryable = true;
      throw error;
    }
    hang() { return new Promise(() => {}); }
    abortSelf() {
      this.ctx.abort("test abort");
      return new Promise(() => {});
    }
  }
)JS"_kj;

struct StartedSession {
  rpc::JsRpcTarget::Client cap;
  kj::Promise<WorkerInterface::CustomEvent::Result> session;
};

StartedSession startSession(WorkerInterface& entrypoint) {
  auto event = kj::heap<JsRpcSessionCustomEvent>(JsRpcSessionCustomEvent::WORKER_RPC_EVENT_TYPE);
  auto cap = event->getCap();
  return {kj::mv(cap), entrypoint.customEvent(kj::mv(event)).eagerlyEvaluate(nullptr)};
}

enum class Operation { CALL, GET_PROPERTY };

kj::Exception expectCallFailure(rpc::JsRpcTarget::Client& cap,
    kj::StringPtr name,
    kj::WaitScope& waitScope,
    Operation operation = Operation::CALL) {
  auto request = cap.callRequest();
  request.setMethodName(name);
  if (operation == Operation::GET_PROPERTY) {
    request.getOperation().setGetProperty();
  }
  return KJ_ASSERT_NONNULL(kj::runCatchingExceptions([&]() { request.send().wait(waitScope); }),
      "call unexpectedly succeeded", name);
}

void expectDeliveredDetails(const kj::Exception& exception) {
  KJ_EXPECT(exception.getDetail(WORKER_REQUEST_DELIVERED_DETAIL_ID) != kj::none, exception);
  KJ_EXPECT(exception.getDetail(jsg::REQUEST_DELIVERED_TO_ACTOR_DETAIL_ID) != kj::none, exception);
  KJ_EXPECT(
      exception.getDetail(jsg::REQUEST_NOT_DELIVERED_TO_ACTOR_DETAIL_ID) == kj::none, exception);
  KJ_EXPECT(exception.getDetail(jsg::ACTOR_RETRY_CLAIM_REJECTED_DETAIL_ID) == kj::none, exception);
}

void expectClaimRejectedDetails(const kj::Exception& exception) {
  KJ_EXPECT(exception.getDetail(jsg::ACTOR_RETRY_CLAIM_REJECTED_DETAIL_ID) != kj::none, exception);
  KJ_EXPECT(exception.getDetail(WORKER_REQUEST_DELIVERED_DETAIL_ID) == kj::none, exception);
  KJ_EXPECT(exception.getDetail(jsg::REQUEST_DELIVERED_TO_ACTOR_DETAIL_ID) == kj::none, exception);
}

KJ_TEST("JSRPC claim rejection fails the native call before construction or delivery") {
  // Construction would replace the claim-rejection failure with this constructor's error.
  static constexpr auto source = R"JS(
    import { DurableObject } from "cloudflare:workers";
    export default class extends DurableObject {
      constructor(ctx, env) {
        super(ctx, env);
        throw new Error("actor was constructed");
      }
      method() {}
    }
  )JS"_kj;
  auto observer = kj::refcounted<RejectingClaimObserver>();
  TestFixture fixture(TestFixture::SetupParams{
    .mainModuleSource = source,
    .actorId = Worker::Actor::Id(kj::str("jsrpc-claim-test")),
    .actorClassName = "default"_kj,
    .requestObserverFactory = kj::Function<kj::Own<RequestObserver>()>(
        [&observer]() -> kj::Own<RequestObserver> { return kj::addRef(*observer); }),
  });
  auto entrypoint = fixture.makeWorkerEntrypoint();
  auto [cap, session] = startSession(*entrypoint);

  auto rejectedCall = expectCallFailure(cap, "method", fixture.getWaitScope());
  KJ_EXPECT(rejectedCall.getDescription().contains("retry claim rejected"), rejectedCall);
  expectClaimRejectedDetails(rejectedCall);
  KJ_EXPECT(observer->claimCount == 1);
  KJ_EXPECT(observer->deliveredCount == 0);

  auto sessionException =
      kj::runCatchingExceptions([&]() { session.wait(fixture.getWaitScope()); });
  expectClaimRejectedDetails(KJ_ASSERT_NONNULL(sessionException));
}

KJ_TEST("actor JSRPC method and getter failures carry delivered details") {
  TestFixture fixture(TestFixture::SetupParams{
    .mainModuleSource = ACTOR_SOURCE,
    .actorId = Worker::Actor::Id(kj::str("jsrpc-delivery-test")),
    .actorClassName = "default"_kj,
  });
  auto entrypoint = fixture.makeWorkerEntrypoint();
  auto [cap, session] = startSession(*entrypoint);

  auto methodFailure = expectCallFailure(cap, "fail", fixture.getWaitScope());
  KJ_EXPECT(methodFailure.getDescription().contains("method failed"), methodFailure);
  expectDeliveredDetails(methodFailure);

  auto getterFailure =
      expectCallFailure(cap, "failingGetter", fixture.getWaitScope(), Operation::GET_PROPERTY);
  KJ_EXPECT(getterFailure.getDescription().contains("getter failed"), getterFailure);
  expectDeliveredDetails(getterFailure);

  cap = nullptr;
  session.wait(fixture.getWaitScope());
}

KJ_TEST("a disconnect rethrown by an actor JSRPC method stays DISCONNECTED and delivered") {
  TestFixture fixture(TestFixture::SetupParams{
    .mainModuleSource = ACTOR_SOURCE,
    .actorId = Worker::Actor::Id(kj::str("jsrpc-rethrow-test")),
    .actorClassName = "default"_kj,
  });
  auto entrypoint = fixture.makeWorkerEntrypoint();
  auto [cap, session] = startSession(*entrypoint);

  // The sender's classifier keys on the DISCONNECTED type and the delivery details, and the
  // tunneled description is what lets it tell a remote JS failure from a local transport one.
  auto failure = expectCallFailure(cap, "rethrowDisconnect", fixture.getWaitScope());
  KJ_EXPECT(failure.getType() == kj::Exception::Type::DISCONNECTED, failure);
  KJ_EXPECT(jsg::isTunneledException(failure.getDescription()), failure);
  KJ_EXPECT(failure.getDescription().contains("outbound disconnected"), failure);
  expectDeliveredDetails(failure);

  cap = nullptr;
  session.wait(fixture.getWaitScope());
}

KJ_TEST("actor JSRPC session abort marks in-flight calls delivered") {
  TestFixture fixture(TestFixture::SetupParams{
    .mainModuleSource = ACTOR_SOURCE,
    .actorId = Worker::Actor::Id(kj::str("jsrpc-abort-test")),
    .actorClassName = "default"_kj,
  });
  auto entrypoint = fixture.makeWorkerEntrypoint();
  auto [cap, session] = startSession(*entrypoint);

  auto abortedCall = expectCallFailure(cap, "abortSelf", fixture.getWaitScope());
  expectDeliveredDetails(abortedCall);

  auto sessionException =
      kj::runCatchingExceptions([&]() { session.wait(fixture.getWaitScope()); });
  KJ_ASSERT_NONNULL(sessionException);
}

KJ_TEST("a native disconnect aborting the actor loses its not-delivered marker after delivery") {
  TestFixture fixture(TestFixture::SetupParams{
    .mainModuleSource = ACTOR_SOURCE,
    .actorId = Worker::Actor::Id(kj::str("jsrpc-native-abort-test")),
    .actorClassName = "default"_kj,
  });
  auto entrypoint = fixture.makeWorkerEntrypoint();
  auto [cap, session] = startSession(*entrypoint);

  auto hangRequest = cap.callRequest();
  hangRequest.setMethodName("hang");
  auto hangPromise = hangRequest.send();
  // Let the call reach user code before the actor's own outbound dependency disconnects.
  fixture.getWaitScope().poll();

  // Such a disconnect is marked not-delivered at its origin; once it aborts an actor that already
  // received this call, the call's result must report delivery instead.
  auto reason = KJ_EXCEPTION(DISCONNECTED, "storage disconnected");
  jsg::markActorRequestNotDelivered(reason);
  fixture.getActor().abort(reason);

  auto abortedCall = KJ_ASSERT_NONNULL(
      kj::runCatchingExceptions([&]() { hangPromise.wait(fixture.getWaitScope()); }));
  KJ_EXPECT(abortedCall.getType() == kj::Exception::Type::DISCONNECTED, abortedCall);
  expectDeliveredDetails(abortedCall);

  cap = nullptr;
  kj::runCatchingExceptions([&]() { session.wait(fixture.getWaitScope()); });
}

class RecordingSink final: public WritableStreamSink {
 public:
  explicit RecordingSink(bool& wrote): wrote(wrote) {}

  kj::Promise<void> write(kj::ArrayPtr<const byte> buffer) override {
    wrote = true;
    return kj::READY_NOW;
  }

  kj::Promise<void> write(kj::ArrayPtr<const kj::ArrayPtr<const byte>> pieces) override {
    wrote = true;
    return kj::READY_NOW;
  }

  kj::Promise<void> end() override {
    return kj::READY_NOW;
  }

  void abort(kj::Exception reason) override {}

 private:
  bool& wrote;
};

void expectWritableStreamUsableAfterDestinationFailure(TestFixture& fixture) {
  bool wrote = false;
  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto stream =
        JsWritableStream::create(env.js, env.context, kj::heap<RecordingSink>(wrote), kj::none);
    RpcSerializerExternalHandler externalHandler(RpcSerializerExternalHandler::DUPLICATE,
        RpcSerializerExternalHandler::GetExternalPusher(
            []() { return brokenCap().castAs<rpc::JsValue::ExternalPusher>(); }),
        RpcSerializerExternalHandler::ResolveDestinationAndGetSpanParents(
            []() -> kj::Maybe<TraceContextParent> { KJ_FAIL_REQUIRE("destination failed"); }));
    jsg::Serializer serializer(env.js, {.externalHandler = externalHandler});

    KJ_EXPECT_THROW_MESSAGE("destination failed", stream.serialize(env.js, serializer));
    auto writePromise =
        stream.writeForTest(env.js, jsg::JsValue(jsg::JsUint8Array::create(env.js, "test"_kjb)));
    return env.context.awaitJs(env.js, kj::mv(writePromise)).attach(kj::mv(stream));
  });
  KJ_EXPECT(wrote);
}

KJ_TEST("writable RPC serialization resolves its destination before consuming the stream") {
  {
    TestFixture legacyFixture;
    expectWritableStreamUsableAfterDestinationFailure(legacyFixture);
  }

  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setTypeScriptImplementedStreams(true);
  TestFixture tsFixture({
    .featureFlags = flags.asReader(),
    .autogates = kj::arr("per-isolate-javascript-bootstrap"_kj),
  });
  expectWritableStreamUsableAfterDestinationFailure(tsFixture);
}

KJ_TEST("terminal abort signals mark RPC serialization ineligible") {
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setAbortSignalRpc(true);
  TestFixture fixture({.featureFlags = flags.asReader()});

  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto expectIneligible = [&](AbortSignal& signal) {
      RpcSerializerExternalHandler externalHandler(RpcSerializerExternalHandler::DUPLICATE,
          brokenCap().castAs<rpc::JsValue::ExternalPusher>(), kj::none);
      jsg::Serializer serializer(env.js, {.externalHandler = externalHandler});
      signal.serialize(env.js, serializer);
      KJ_EXPECT(serializer.release().data.size() > 0);
      KJ_EXPECT(externalHandler.size() == 0);
      KJ_EXPECT(externalHandler.getReplayability() == Replayability::INELIGIBLE);
    };

    auto aborted = AbortSignal::abort(env.js, kj::none);
    expectIneligible(*aborted);
    auto neverAborts =
        env.js.alloc<AbortSignal>(kj::none, kj::none, AbortSignal::Flag::NEVER_ABORTS);
    expectIneligible(*neverAborts);
  });
}

}  // namespace
}  // namespace workerd::api
