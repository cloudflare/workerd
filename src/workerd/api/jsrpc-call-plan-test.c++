// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "basics.h"
#include "http.h"
#include "worker-rpc.h"

#include <workerd/tests/test-fixture.h>

#include <capnp/capability.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

using ExternalOwnership = RpcSerializerExternalHandler::ExternalOwnership;

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

class FailingOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  explicit FailingOutgoingFactory(uint& callCount): callCount(callCount) {}

  Result newSingleUseClient(kj::Maybe<kj::String>, MakeUserSpanParent) override {
    ++callCount;
    KJ_FAIL_REQUIRE("destination failed");
  }

  bool supportsActorCallRetries() const override {
    return true;
  }

 private:
  uint& callCount;
};

capnp::Capability::Client brokenCap() {
  return capnp::Capability::Client(KJ_EXCEPTION(FAILED, "test cap"));
}

jsg::JsRef<jsg::JsFunction> wrapMethod(jsg::Lock& js, Fetcher& fetcher, kj::StringPtr name) {
  auto method = KJ_REQUIRE_NONNULL(fetcher.getRpcMethodForTestOnly(js, kj::str(name)));
  auto& handler = KJ_REQUIRE_NONNULL(js.tryGetTypeHandler<jsg::Ref<JsRpcProperty>>());
  auto function =
      KJ_REQUIRE_NONNULL(jsg::JsValue(handler.wrap(js, kj::mv(method))).tryCast<jsg::JsFunction>());
  return jsg::JsRef<jsg::JsFunction>(js, function);
}

template <typename Func>
JsRpcCallPlan makePlan(Func populate,
    kj::Array<const byte> serializedData = kj::heapArray<const byte>(0),
    kj::Array<ExternalOwnership> externalOwnerships = kj::heapArray<ExternalOwnership>(0),
    RpcSerializerExternalHandler::Replayability replayability =
        RpcSerializerExternalHandler::Replayability::REPLAYABLE) {
  auto message = kj::heap<capnp::MallocMessageBuilder>(
      JsRpcCallPlan::METADATA_SEGMENT_WORDS, capnp::AllocationStrategy::FIXED_SIZE);
  populate(message->initRoot<rpc::JsRpcTarget::CallParams>());
  return JsRpcCallPlan(
      kj::mv(message), kj::mv(serializedData), kj::mv(externalOwnerships), replayability);
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
  auto plan = makePlan(
      [&](rpc::JsRpcTarget::CallParams::Builder builder) {
    auto args = builder.getOperation().initCallWithArgs();
    auto external = args.initExternals(1)[0].initRpcTarget();
    external.setCap(kj::heap<CountingJsRpcTarget>(callCount));
  }, kj::heapArray<const byte>(1), kj::arr(ExternalOwnership::RPC_TARGET_DUPLICATED),
      RpcSerializerExternalHandler::Replayability::INELIGIBLE);
  KJ_EXPECT(!plan.getReplayable());
  KJ_EXPECT(plan.getExternalOwnerships().size() == 1);
  KJ_EXPECT(plan.getExternalOwnerships()[0] == ExternalOwnership::RPC_TARGET_DUPLICATED);

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

KJ_TEST("JS RPC call plan validates ownership metadata for every external kind") {
  auto expectValidMetadata = []<typename Func>(Func populate, ExternalOwnership ownership) {
    auto plan = makePlan(
        [&](rpc::JsRpcTarget::CallParams::Builder builder) {
      auto external = builder.getOperation().initCallWithArgs().initExternals(1)[0];
      populate(external);
    }, kj::heapArray<const byte>(1), kj::arr(ownership),
        RpcSerializerExternalHandler::Replayability::INELIGIBLE);
    KJ_EXPECT(!plan.getReplayable());
  };

  expectValidMetadata([](auto external) { external.setInvalid(); }, ExternalOwnership::NONE);
  expectValidMetadata(
      [](auto external) { external.initRpcTarget(); }, ExternalOwnership::RPC_TARGET_DUPLICATED);
  expectValidMetadata(
      [](auto external) { external.initRpcTarget(); }, ExternalOwnership::RPC_TARGET_TRANSFERRED);
  expectValidMetadata(
      [](auto external) { external.initWritableStream(); }, ExternalOwnership::NONE);
  expectValidMetadata(
      [](auto external) { external.initReadableStream(); }, ExternalOwnership::NONE);
  expectValidMetadata([](auto external) { external.setObsolete7(); }, ExternalOwnership::NONE);
  expectValidMetadata([](auto external) {
    external.setAbortSignal(brokenCap().castAs<rpc::JsValue::ExternalPusher::AbortSignal>());
  }, ExternalOwnership::NONE);
  expectValidMetadata(
      [](auto external) { external.initSubrequestChannelToken(0); }, ExternalOwnership::NONE);
  expectValidMetadata(
      [](auto external) { external.initActorClassChannelToken(0); }, ExternalOwnership::NONE);
  expectValidMetadata([](auto external) {
    external.setDelayedSubrequestChannelToken(
        brokenCap().castAs<rpc::JsValue::ExternalPusher::DelayedChannelToken>());
  }, ExternalOwnership::NONE);
  expectValidMetadata([](auto external) {
    external.setDelayedActorClassChannelToken(
        brokenCap().castAs<rpc::JsValue::ExternalPusher::DelayedChannelToken>());
  }, ExternalOwnership::NONE);
  expectValidMetadata([](auto external) { external.initSocket(); }, ExternalOwnership::NONE);
}

KJ_TEST("JS RPC call plan honors serializer ineligibility without an external entry") {
  auto plan = makePlan(
      [](rpc::JsRpcTarget::CallParams::Builder builder) {
    builder.getOperation().initCallWithArgs();
  }, kj::heapArray<const byte>(1), kj::heapArray<ExternalOwnership>(0),
      RpcSerializerExternalHandler::Replayability::INELIGIBLE);
  KJ_EXPECT(!plan.getReplayable());
}

KJ_TEST("native RPC stub serialization records duplicate and transfer ownership") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto expectOwnership = [&](RpcSerializerExternalHandler::StubOwnership stubOwnership,
                               ExternalOwnership expected) {
      auto cap = brokenCap().castAs<rpc::JsRpcTarget>();
      auto stub = env.js.alloc<JsRpcStub>(env.context.addObject(kj::heap(kj::mv(cap))), kj::none);
      RpcSerializerExternalHandler externalHandler(
          stubOwnership, brokenCap().castAs<rpc::JsValue::ExternalPusher>(), kj::none);
      jsg::Serializer serializer(env.js, {.externalHandler = externalHandler});
      stub->serialize(env.js, serializer);
      KJ_EXPECT(externalHandler.getReplayability() ==
          RpcSerializerExternalHandler::Replayability::INELIGIBLE);
      auto ownerships = externalHandler.releaseExternalOwnerships();
      KJ_EXPECT(ownerships.size() == 1);
      KJ_EXPECT(ownerships[0] == expected);
    };

    expectOwnership(
        RpcSerializerExternalHandler::DUPLICATE, ExternalOwnership::RPC_TARGET_DUPLICATED);
    expectOwnership(
        RpcSerializerExternalHandler::TRANSFER, ExternalOwnership::RPC_TARGET_TRANSFERRED);
  });
}

KJ_TEST("JavaScript RPC targets, functions, and proxies record duplication ownership") {
  TestFixture fixture;
  fixture.runInIoContext([](const TestFixture::Environment& env) {
    auto& targetHandler = KJ_REQUIRE_NONNULL(
        env.js.tryGetTypeHandler<jsg::Ref<JsRpcTarget>>(), "JsRpcTarget type handler is missing");
    auto makeTarget = [&]() {
      return KJ_REQUIRE_NONNULL(
          jsg::JsValue(targetHandler.wrap(env.js, env.js.alloc<JsRpcTarget>()))
              .tryCast<jsg::JsObject>());
    };
    auto serializeValue = [&](jsg::JsValue value) {
      RpcSerializerExternalHandler externalHandler(RpcSerializerExternalHandler::DUPLICATE,
          brokenCap().castAs<rpc::JsValue::ExternalPusher>(), kj::none);
      jsg::Serializer serializer(env.js, {.externalHandler = externalHandler});
      serializer.write(env.js, value);
      serializer.release();
      return externalHandler.releaseExternalOwnerships();
    };

    auto fallbackOwnerships = serializeValue(jsg::JsValue(makeTarget()));
    KJ_EXPECT(fallbackOwnerships.size() == 1);
    KJ_EXPECT(fallbackOwnerships[0] == ExternalOwnership::RPC_TARGET_TRANSFERRED);

    uint dupCount = 0;
    auto original = makeTarget();
    auto originalRef = jsg::JsRef<jsg::JsObject>(env.js, original);
    original.set(env.js, "dup"_kj,
        jsg::JsValue(env.js.wrapReturningFunction(env.js.v8Context(),
            [&dupCount, originalRef = kj::mv(originalRef)](
                jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>&) mutable {
      ++dupCount;
      return v8::Local<v8::Value>(originalRef.getHandle(js));
    })));

    auto duplicateOwnerships = serializeValue(jsg::JsValue(original));
    KJ_EXPECT(dupCount == 1);
    KJ_EXPECT(duplicateOwnerships.size() == 1);
    KJ_EXPECT(duplicateOwnerships[0] == ExternalOwnership::RPC_TARGET_DUPLICATED);

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

    auto function = env.js.wrapReturningFunction(
        env.js.v8Context(), [](jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>&) {
      return v8::Local<v8::Value>(js.undefined());
    });
    auto functionObject = jsg::JsObject(function);
    auto functionRef = jsg::JsRef<jsg::JsObject>(env.js, functionObject);
    functionObject.set(env.js, "dup"_kj,
        jsg::JsValue(env.js.wrapReturningFunction(env.js.v8Context(),
            [functionRef = kj::mv(functionRef)](
                jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>&) mutable {
      return v8::Local<v8::Value>(functionRef.getHandle(js));
    })));
    auto functionOwnerships = serializeValue(jsg::JsValue(functionObject));
    KJ_EXPECT(functionOwnerships.size() == 1);
    KJ_EXPECT(functionOwnerships[0] == ExternalOwnership::RPC_TARGET_DUPLICATED);

    auto proxyTarget = makeTarget();
    auto proxyTargetRef = jsg::JsRef<jsg::JsObject>(env.js, proxyTarget);
    proxyTarget.set(env.js, "dup"_kj,
        jsg::JsValue(env.js.wrapReturningFunction(env.js.v8Context(),
            [proxyTargetRef = kj::mv(proxyTargetRef)](
                jsg::Lock& js, const v8::FunctionCallbackInfo<v8::Value>&) mutable {
      return v8::Local<v8::Value>(proxyTargetRef.getHandle(js));
    })));
    auto proxy = jsg::check(v8::Proxy::New(env.js.v8Context(), v8::Local<v8::Object>(proxyTarget),
        v8::Local<v8::Object>(env.js.obj())));
    auto proxyOwnerships = serializeValue(jsg::JsValue(proxy));
    KJ_EXPECT(proxyOwnerships.size() == 1);
    KJ_EXPECT(proxyOwnerships[0] == ExternalOwnership::RPC_TARGET_DUPLICATED);
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
  TestFixture fixture({.featureFlags = flags.asReader()});

  uint destinationCallCount = 0;
  fixture.runInIoContext([&](const TestFixture::Environment& env) {
    auto fetcher =
        env.js.alloc<Fetcher>(env.context.addObject<Fetcher::OutgoingFactory>(
                                  kj::heap<FailingOutgoingFactory>(destinationCallCount)),
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
    KJ_EXPECT(destinationCallCount == 0);

    auto cap = brokenCap().castAs<rpc::JsRpcTarget>();
    auto stub = env.js.alloc<JsRpcStub>(env.context.addObject(kj::heap(kj::mv(cap))), kj::none);
    auto& stubHandler = KJ_REQUIRE_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<JsRpcStub>>());
    KJ_EXPECT_LOG(ERROR, "destination failed");
    auto destinationFailure = startRejectedCall(stubHandler.wrap(env.js, stub.addRef()));
    KJ_EXPECT(destinationCallCount == 1);
    auto duplicate KJ_UNUSED = stub->dup(env.js);

    return kj::joinPromises(kj::arr(kj::mv(serializationFailure), kj::mv(destinationFailure)))
        .attach(kj::mv(method), kj::mv(stub), kj::mv(fetcher));
  });
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
      KJ_EXPECT(externalHandler.getReplayability() ==
          RpcSerializerExternalHandler::Replayability::INELIGIBLE);
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
