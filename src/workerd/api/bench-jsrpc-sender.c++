// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Benchmarks the local JSRPC sender path through Fetcher::getClientForOneCall() and callImpl().
// Run with: bazel run @workerd//src/workerd/api:bench-jsrpc-sender -- --benchmark_repetitions=10

#include "http.h"
#include "worker-rpc.h"

#include <workerd/tests/bench-tools.h>
#include <workerd/tests/test-fixture.h>

#include <kj/async-io.h>

namespace workerd::api {
namespace {

constexpr kj::StringPtr RECEIVER_SOURCE = R"JS(
  import { WorkerEntrypoint } from "cloudflare:workers";

  export default class extends WorkerEntrypoint {
    accept(value) { return 1; }
  }
)JS"_kj;

constexpr size_t CONCURRENT_CALLS = 64;
constexpr size_t LARGE_VALUE_SIZE = MAX_JS_RPC_MESSAGE_SIZE - 4096;

struct PendingMemory {
  kj::Maybe<int64_t> allocatedBytes;
  int64_t externalBytes;
};

kj::Maybe<size_t> currentAllocatedBytes() {
#ifdef WD_USE_TCMALLOC
  auto bytes = tcmalloc::MallocExtension::GetNumericProperty("generic.current_allocated_bytes");
  if (bytes.has_value()) return *bytes;
#endif
  return kj::none;
}

class CallGate {
 public:
  void start() {
    KJ_REQUIRE(!active);
    KJ_REQUIRE(fulfillers.empty());
    active = true;
  }

  kj::Promise<void> wait() {
    KJ_REQUIRE(active);
    auto paf = kj::newPromiseAndFulfiller<void>();
    fulfillers.add(kj::mv(paf.fulfiller));
    return kj::mv(paf.promise);
  }

  void release(size_t expectedCalls) {
    KJ_REQUIRE(active);
    KJ_REQUIRE(fulfillers.size() == expectedCalls, fulfillers.size(), expectedCalls);
    active = false;
    for (auto& fulfiller: fulfillers) {
      fulfiller->fulfill();
    }
    fulfillers.clear();
  }

  bool isActive() const {
    return active;
  }

 private:
  bool active = false;
  kj::Vector<kj::Own<kj::PromiseFulfiller<void>>> fulfillers;
};

class LocalOutgoingFactory final: public Fetcher::OutgoingFactory {
 public:
  LocalOutgoingFactory(TestFixture& receiver, CallGate& callGate)
      : receiver(receiver),
        callGate(callGate) {}

  Result newSingleUseClient(kj::Maybe<kj::String>, MakeUserSpanParent) override {
    if (callGate.isActive()) {
      auto destination = receiver.makeWorkerEntrypoint();
      auto client = callGate.wait().then(
          [destination = kj::mv(destination)]() mutable { return kj::mv(destination); });
      return {.client = newPromisedWorkerInterface(kj::mv(client)), .spanParents = kj::none};
    }
    return {.client = receiver.makeWorkerEntrypoint(), .spanParents = kj::none};
  }

  bool supportsActorCallRetries() const override {
    return true;
  }

 private:
  TestFixture& receiver;
  CallGate& callGate;
};

enum class ArgumentKind {
  NONE,
  PLAIN,
  WRITABLE_STREAM,
  TRANSFERRED_RPC_STUB,
};

class JsRpcSenderHarness {
 public:
  explicit JsRpcSenderHarness(size_t payloadSize = 0): io(kj::setupAsyncIo()) {
    capnp::MallocMessageBuilder flagsMessage;
    auto flags = flagsMessage.initRoot<CompatibilityFlags>();
    flags.setFetcherRpc(true);
    flags.setStreamsJavaScriptControllers(true);
    flags.setUnwrapCustomThenables(true);

    receiver = kj::heap<TestFixture>(TestFixture::SetupParams{
      .waitScope = io.waitScope,
      .mainModuleSource = RECEIVER_SOURCE,
      .useRealTimers = false,
    });
    sender = kj::heap<TestFixture>(TestFixture::SetupParams{
      .waitScope = io.waitScope,
      .featureFlags = flags.asReader(),
      .useRealTimers = false,
    });
    ioContext = sender->newIoContext();
    request = sender->newIncomingRequest(*ioContext);

    sender->enterContext(*request, [&](const TestFixture::Environment& env) {
      auto fetcher = env.js.alloc<Fetcher>(env.context.addObject<Fetcher::OutgoingFactory>(
                                               kj::heap<LocalOutgoingFactory>(*receiver, callGate)),
          Fetcher::RequiresHostAndProtocol::YES);
      accept = wrapMethod(env.js, *fetcher, "accept"_kj);
      if (payloadSize > 0) {
        payload =
            jsg::JsRef<jsg::JsString>(env.js, env.js.str(kj::str(kj::repeat('x', payloadSize))));
      }
    });
  }

  void run(ArgumentKind kind) {
    kj::Maybe<kj::Promise<void>> pending;
    sender->enterContext(*request, [&](const TestFixture::Environment& env) {
      pending = call(env, KJ_ASSERT_NONNULL(accept), kind);
    });
    KJ_ASSERT_NONNULL(pending).wait(io.waitScope);
  }

  PendingMemory runPendingCalls(size_t callCount) {
    auto allocatedBefore = currentAllocatedBytes();
    callGate.start();
    int64_t externalBefore = 0;
    int64_t externalWhilePending = 0;
    kj::Maybe<kj::Promise<void>> pending;
    sender->enterContext(*request, [&](const TestFixture::Environment& env) {
      externalBefore = static_cast<int64_t>(env.js.v8Isolate->GetExternalMemory());
      auto promises = kj::heapArrayBuilder<kj::Promise<void>>(callCount);
      for (size_t i = 0; i < callCount; ++i) {
        promises.add(call(env, KJ_ASSERT_NONNULL(accept), ArgumentKind::PLAIN));
      }
      pending = kj::joinPromises(promises.finish());
      externalWhilePending = static_cast<int64_t>(env.js.v8Isolate->GetExternalMemory());
    });
    auto allocatedWhilePending = currentAllocatedBytes();
    callGate.release(callCount);
    KJ_ASSERT_NONNULL(pending).wait(io.waitScope);

    kj::Maybe<int64_t> allocatedBytes;
    KJ_IF_SOME(before, allocatedBefore) {
      KJ_IF_SOME(whilePending, allocatedWhilePending) {
        allocatedBytes = static_cast<int64_t>(whilePending) - static_cast<int64_t>(before);
      }
    }
    return {
      .allocatedBytes = allocatedBytes,
      .externalBytes = externalWhilePending - externalBefore,
    };
  }

 private:
  static jsg::JsRef<jsg::JsFunction> wrapMethod(
      jsg::Lock& js, Fetcher& fetcher, kj::StringPtr name) {
    auto method = KJ_REQUIRE_NONNULL(fetcher.getRpcMethodForTestOnly(js, kj::str(name)));
    auto& handler = KJ_REQUIRE_NONNULL(js.tryGetTypeHandler<jsg::Ref<JsRpcProperty>>());
    auto function = KJ_REQUIRE_NONNULL(
        jsg::JsValue(handler.wrap(js, kj::mv(method))).tryCast<jsg::JsFunction>());
    return jsg::JsRef<jsg::JsFunction>(js, function);
  }

  kj::Promise<void> call(
      const TestFixture::Environment& env, jsg::JsRef<jsg::JsFunction>& method, ArgumentKind kind) {
    v8::LocalVector<v8::Value> args(env.js.v8Isolate);
    switch (kind) {
      case ArgumentKind::NONE:
        break;
      case ArgumentKind::PLAIN:
        args.push_back(KJ_ASSERT_NONNULL(payload).getHandle(env.js));
        break;
      case ArgumentKind::WRITABLE_STREAM: {
        auto constructor = KJ_REQUIRE_NONNULL(jsg::JsObject(env.js.v8Context()->Global())
                                                  .get(env.js, "WritableStream"_kj)
                                                  .tryCast<jsg::JsFunction>());
        args.push_back(constructor.newInstance(env.js));
        break;
      }
      case ArgumentKind::TRANSFERRED_RPC_STUB: {
        auto& targetHandler = KJ_REQUIRE_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<JsRpcTarget>>());
        auto target =
            KJ_REQUIRE_NONNULL(jsg::JsValue(targetHandler.wrap(env.js, env.js.alloc<JsRpcTarget>()))
                                   .tryCast<jsg::JsObject>());
        auto stub = JsRpcStub::constructor(env.js, target);
        auto& stubHandler = KJ_REQUIRE_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<JsRpcStub>>());
        args.push_back(stubHandler.wrap(env.js, kj::mv(stub)));
        break;
      }
    }

    auto result = jsg::JsFunction(method.getHandle(env.js)).call(env.js, env.js.undefined(), args);
    auto promise = env.js.toPromise(result);
    return env.context.awaitJs(env.js, kj::mv(promise)).ignoreResult();
  }

  kj::AsyncIoContext io;
  CallGate callGate;
  kj::Own<TestFixture> receiver;
  kj::Own<TestFixture> sender;
  kj::Own<IoContext> ioContext;
  kj::Own<IoContext::IncomingRequest> request;
  kj::Maybe<jsg::JsRef<jsg::JsFunction>> accept;
  kj::Maybe<jsg::JsRef<jsg::JsString>> payload;
};

void JsRpcNoArgs(benchmark::State& state) {
  JsRpcSenderHarness harness;
  for (auto _: state) {
    harness.run(ArgumentKind::NONE);
  }
  state.SetItemsProcessed(state.iterations());
}

void JsRpcPlainValue(benchmark::State& state) {
  auto size = static_cast<size_t>(state.range(0));
  JsRpcSenderHarness harness(size);
  for (auto _: state) {
    harness.run(ArgumentKind::PLAIN);
  }
  state.SetItemsProcessed(state.iterations());
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(size));
}

void JsRpcWritableStream(benchmark::State& state) {
  JsRpcSenderHarness harness;
  for (auto _: state) {
    harness.run(ArgumentKind::WRITABLE_STREAM);
  }
  state.SetItemsProcessed(state.iterations());
}

void JsRpcTransferredStub(benchmark::State& state) {
  JsRpcSenderHarness harness;
  for (auto _: state) {
    harness.run(ArgumentKind::TRANSFERRED_RPC_STUB);
  }
  state.SetItemsProcessed(state.iterations());
}

void runPendingCallsBenchmark(benchmark::State& state, size_t payloadSize, size_t concurrentCalls) {
  JsRpcSenderHarness harness(payloadSize);
  int64_t pendingAllocatedBytes = 0;
  int64_t pendingExternalBytes = 0;
  for (auto _: state) {
    auto memory = harness.runPendingCalls(concurrentCalls);
    KJ_IF_SOME(bytes, memory.allocatedBytes) {
      pendingAllocatedBytes += bytes;
    }
    pendingExternalBytes += memory.externalBytes;
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(concurrentCalls));
  state.SetBytesProcessed(state.iterations() * static_cast<int64_t>(concurrentCalls * payloadSize));
  state.counters["pending_calls"] = concurrentCalls;
  state.counters["pending_external_bytes"] =
      static_cast<double>(pendingExternalBytes) / state.iterations();
#ifdef WD_USE_TCMALLOC
  state.counters["pending_allocated_bytes"] =
      static_cast<double>(pendingAllocatedBytes) / state.iterations();
#endif
}

void JsRpcConcurrentSmallCalls(benchmark::State& state) {
  runPendingCallsBenchmark(state, 1024, CONCURRENT_CALLS);
}

void JsRpcPendingLargeCalls(benchmark::State& state) {
  runPendingCallsBenchmark(state, LARGE_VALUE_SIZE, state.range(0));
}

WD_BENCHMARK(JsRpcNoArgs);
WD_BENCHMARK(JsRpcPlainValue)->Arg(1024)->Arg(64 * 1024)->Arg(1024 * 1024)->Arg(LARGE_VALUE_SIZE);
WD_BENCHMARK(JsRpcWritableStream);
WD_BENCHMARK(JsRpcTransferredStub);
WD_BENCHMARK(JsRpcConcurrentSmallCalls)->Iterations(1);
WD_BENCHMARK(JsRpcPendingLargeCalls)->Arg(1)->Arg(2)->Iterations(1);

}  // namespace
}  // namespace workerd::api
