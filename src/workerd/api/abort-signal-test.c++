// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/basics.h>
#include <workerd/api/worker-rpc.h>
#include <workerd/tests/test-fixture.h>

#include <capnp/message.h>
#include <kj/test.h>

namespace workerd::api {
namespace {

constexpr int64_t REASON_SIZE = 1024 * 1024;
constexpr uint DELIVERIES = 8;

jsg::JsValue makeReason(jsg::Lock& js) {
  return js.str(kj::str(kj::repeat('x', REASON_SIZE)));
}

// The isolate's external memory, including adjustments not yet applied.
int64_t getExternalMemory(jsg::Lock& js) {
  js.getExternalMemoryTarget()->applyDeferredMemoryUpdate();
  return js.v8Isolate->GetExternalMemory();
}

// External memory held by an abort's queued deliveries, and still held once they have run.
struct QueuedAbortMemory {
  int64_t queued;
  int64_t retained;
};

// Creates a signal in one non-actor IoContext, has `registerDeliveries` register deliveries on it
// in another, and aborts it from the first, which queues the deliveries in the second. Then runs
// them.
QueuedAbortMemory measureQueuedAbort(TestFixture& fixture,
    kj::FunctionParam<void(TestFixture::Environment&, AbortSignal&)> registerDeliveries) {
  auto ownerContext = fixture.newIoContext();
  auto owner = fixture.newIncomingRequest(*ownerContext);
  auto aborterContext = fixture.newIoContext();
  auto aborter = fixture.newIncomingRequest(*aborterContext);
  // Simulates outstanding I/O, so hang detection doesn't abort the contexts when the loop is idle.
  auto ownerPendingEvent = ownerContext->registerPendingEvent();
  auto aborterPendingEvent = aborterContext->registerPendingEvent();

  kj::Maybe<jsg::Ref<AbortSignal>> signal;
  fixture.enterContext(
      *aborter, [&](TestFixture::Environment& env) { signal = env.js.alloc<AbortSignal>(); });
  fixture.enterContext(*owner,
      [&](TestFixture::Environment& env) { registerDeliveries(env, *KJ_ASSERT_NONNULL(signal)); });

  QueuedAbortMemory memory{};
  int64_t before = 0;
  fixture.enterContext(*aborter, [&](TestFixture::Environment& env) {
    before = getExternalMemory(env.js);
    KJ_ASSERT_NONNULL(signal)->triggerAbort(
        env.js, kj::OneOf<kj::Exception, jsg::JsValue>(makeReason(env.js)));
    memory.queued = getExternalMemory(env.js) - before;
  });

  fixture.pollEventLoop();
  fixture.enterContext(*aborter, [&](TestFixture::Environment& env) {
    memory.retained = getExternalMemory(env.js) - before;
    signal = kj::none;
  });

  fixture.drainAndDestroy(kj::mv(aborter));
  fixture.drainAndDestroy(kj::mv(owner));
  return memory;
}

KJ_TEST("queued native abort deliveries share one counted copy of the exception") {
  TestFixture fixture;
  uint delivered = 0;
  kj::Vector<kj::Own<void>> registrations;
  auto memory =
      measureQueuedAbort(fixture, [&](TestFixture::Environment& env, AbortSignal& signal) {
    for (uint i = 0; i < DELIVERIES; i++) {
      registrations.add(signal.addAbortAction(
          env.js, [&delivered](jsg::Lock&, const kj::Exception&) { ++delivered; }));
    }
  });

  // The copy holds the reason once or twice (stringified, and possibly serialized); a copy per
  // delivery would hold it at least DELIVERIES times.
  KJ_EXPECT(memory.queued >= REASON_SIZE && memory.queued < 4 * REASON_SIZE, memory.queued);
  KJ_EXPECT(delivered == DELIVERIES, delivered);
  KJ_EXPECT(memory.retained < REASON_SIZE, memory.retained);
}

// Counts the aborts sent to a signal's RPC clones.
class CountingAbortTrigger final: public rpc::AbortTrigger::Server {
 public:
  explicit CountingAbortTrigger(uint& aborts): aborts(aborts) {}

  kj::Promise<void> abort(AbortContext) override {
    ++aborts;
    return kj::READY_NOW;
  }

 private:
  uint& aborts;
};

// Stands in for the receiving end of RPC, giving each serialized signal a CountingAbortTrigger.
class TestExternalPusher final: public rpc::JsValue::ExternalPusher::Server {
 public:
  explicit TestExternalPusher(uint& aborts): aborts(aborts) {}

  kj::Promise<void> pushAbortSignal(PushAbortSignalContext context) override {
    context.initResults().setTrigger(kj::heap<CountingAbortTrigger>(aborts));
    return kj::READY_NOW;
  }

 private:
  uint& aborts;
};

KJ_TEST("queued RPC abort deliveries share one counted copy of the serialized reason") {
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setAbortSignalRpc(true);
  uint aborts = 0;
  TestFixture fixture({.featureFlags = flags.asReader()});
  rpc::JsValue::ExternalPusher::Client pusher = kj::heap<TestExternalPusher>(aborts);

  int64_t capacity = 0;
  auto memory =
      measureQueuedAbort(fixture, [&](TestFixture::Environment& env, AbortSignal& signal) {
    for (uint i = 0; i < DELIVERIES; i++) {
      RpcSerializerExternalHandler handler(
          RpcSerializerExternalHandler::TRANSFER, pusher, kj::none);
      jsg::Serializer serializer(env.js, {.externalHandler = handler});
      signal.serialize(env.js, serializer);
    }

    // The abort serializes its reason the same way, into a buffer of this capacity.
    jsg::Serializer serializer(env.js);
    serializer.write(env.js, makeReason(env.js));
    capacity = serializer.release().dataCapacity;
  });

  // A copy per clone would hold the buffer DELIVERIES times.
  KJ_EXPECT(memory.queued >= capacity && memory.queued < 2 * capacity, memory.queued, capacity);
  KJ_EXPECT(aborts == DELIVERIES, aborts);
  KJ_EXPECT(memory.retained < REASON_SIZE, memory.retained);
}

}  // namespace
}  // namespace workerd::api
