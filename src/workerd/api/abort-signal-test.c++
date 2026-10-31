// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/api/basics.h>
#include <workerd/api/worker-rpc.h>
#include <workerd/io/external-pusher.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/autogate.h>

#include <capnp/capability.h>
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
  TestFixture fixture({.autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS)});
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

KJ_TEST("queued native abort deliveries aren't counted without the autogate") {
  TestFixture fixture({.ignoreAllAutogatesEnv = true});
  uint delivered = 0;
  kj::Vector<kj::Own<void>> registrations;
  auto memory =
      measureQueuedAbort(fixture, [&](TestFixture::Environment& env, AbortSignal& signal) {
    for (uint i = 0; i < DELIVERIES; i++) {
      registrations.add(signal.addAbortAction(
          env.js, [&delivered](jsg::Lock&, const kj::Exception&) { ++delivered; }));
    }
  });

  // A counted copy would hold the reason at least once.
  KJ_EXPECT(memory.queued < REASON_SIZE, memory.queued);
  KJ_EXPECT(delivered == DELIVERIES, delivered);
}

KJ_TEST("a native abort delivery to an idle actor waits for its next request") {
  TestFixture fixture({.autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .actorId = Worker::Actor::Id(kj::str("idle-native-delivery-test"))});
  auto actorContext = fixture.newIoContext();
  auto statelessContext = fixture.newStatelessIoContext();
  auto aborter = fixture.newIncomingRequest(*statelessContext);
  // Simulates outstanding I/O, so hang detection doesn't abort the aborter when the loop is idle.
  auto aborterPendingEvent = statelessContext->registerPendingEvent();

  kj::Maybe<jsg::Ref<AbortSignal>> signal;
  fixture.enterContext(
      *aborter, [&](TestFixture::Environment& env) { signal = env.js.alloc<AbortSignal>(); });

  // Registered by an actor request that then ends, leaving the actor idle.
  uint delivered = 0;
  kj::Own<void> registration;
  auto registering = fixture.newIncomingRequest(*actorContext);
  fixture.enterContext(*registering, [&](TestFixture::Environment& env) {
    registration = KJ_ASSERT_NONNULL(signal)->addAbortAction(
        env.js, [&delivered](jsg::Lock&, const kj::Exception&) { ++delivered; });
  });
  fixture.drainAndDestroy(kj::mv(registering));

  fixture.enterContext(*aborter, [&](TestFixture::Environment& env) {
    KJ_ASSERT_NONNULL(signal)->triggerAbort(
        env.js, kj::OneOf<kj::Exception, jsg::JsValue>(jsg::JsValue(env.js.str("done"_kj))));
  });
  fixture.pollEventLoop();
  KJ_EXPECT(delivered == 0, delivered);
  KJ_EXPECT(actorContext->getAbortReason() == kj::none, actorContext->getAbortReason());

  auto next = fixture.newIncomingRequest(*actorContext);
  fixture.pollEventLoop();
  KJ_EXPECT(delivered == 1, delivered);
  KJ_EXPECT(actorContext->getAbortReason() == kj::none, actorContext->getAbortReason());

  fixture.drainAndDestroy(kj::mv(next));
  fixture.enterContext(*aborter, [&](TestFixture::Environment&) {
    registration = nullptr;
    signal = kj::none;
  });
  fixture.drainAndDestroy(kj::mv(aborter));
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

// Serializes `signal` for RPC to `pusher`'s end, which registers an RPC clone with the signal.
void cloneForRpc(jsg::Lock& js, AbortSignal& signal, rpc::JsValue::ExternalPusher::Client& pusher) {
  RpcSerializerExternalHandler handler(RpcSerializerExternalHandler::TRANSFER, pusher, kj::none);
  jsg::Serializer serializer(js, {.externalHandler = handler});
  signal.serialize(js, serializer);
}

KJ_TEST("queued RPC abort deliveries share one counted copy of the serialized reason") {
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setAbortSignalRpc(true);
  uint aborts = 0;
  TestFixture fixture({.featureFlags = flags.asReader(),
    .autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS)});
  rpc::JsValue::ExternalPusher::Client pusher = kj::heap<TestExternalPusher>(aborts);

  int64_t capacity = 0;
  auto memory =
      measureQueuedAbort(fixture, [&](TestFixture::Environment& env, AbortSignal& signal) {
    for (uint i = 0; i < DELIVERIES; i++) {
      cloneForRpc(env.js, signal, pusher);
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

KJ_TEST("queued RPC abort deliveries aren't counted without the autogate") {
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setAbortSignalRpc(true);
  uint aborts = 0;
  TestFixture fixture({.featureFlags = flags.asReader(), .ignoreAllAutogatesEnv = true});
  rpc::JsValue::ExternalPusher::Client pusher = kj::heap<TestExternalPusher>(aborts);

  auto memory =
      measureQueuedAbort(fixture, [&](TestFixture::Environment& env, AbortSignal& signal) {
    for (uint i = 0; i < DELIVERIES; i++) {
      cloneForRpc(env.js, signal, pusher);
    }
  });

  // A counted copy would hold the reason at least once.
  KJ_EXPECT(memory.queued < REASON_SIZE, memory.queued);
  KJ_EXPECT(aborts == DELIVERIES, aborts);
}

constexpr uint REGISTRATIONS = 100;

// Sends `signal` from `sender`'s context to `receiver`'s over RPC, as passing it to a JS RPC call
// would, and returns the clone. The receiver's context owns the clone's RPC subscription.
jsg::Ref<AbortSignal> sendOverRpc(TestFixture& fixture,
    IoContext::IncomingRequest& sender,
    AbortSignal& signal,
    IoContext::IncomingRequest& receiver) {
  capnp::MallocMessageBuilder message;
  capnp::BuilderCapabilityTable caps;
  auto value = caps.imbue(message.initRoot<rpc::JsValue>());
  rpc::JsValue::ExternalPusher::Client pusher = receiver.getContext().getExternalPusher();
  fixture.enterContext(sender, [&](TestFixture::Environment& env) {
    RpcSerializerExternalHandler handler(
        RpcSerializerExternalHandler::TRANSFER, kj::mv(pusher), kj::none);
    jsg::Serializer serializer(env.js, {.externalHandler = handler});
    signal.serialize(env.js, serializer);
    value.setV8Serialized(serializer.release().data);
    value.adoptExternals(handler.build(capnp::Orphanage::getForMessageContaining(value)));
  });

  kj::Maybe<jsg::Ref<AbortSignal>> clone;
  fixture.enterContext(receiver, [&](TestFixture::Environment& env) {
    auto reader = value.asReader();
    RpcStubDisposalGroup disposalGroup;
    RpcDeserializerExternalHandler handler(reader.getExternals(), disposalGroup, kj::none);
    jsg::Deserializer deserializer(env.js, reader.getV8Serialized(), kj::none, kj::none,
        jsg::Deserializer::Options{.externalHandler = handler});
    clone = AbortSignal::deserialize(env.js, rpc::SerializationTag::ABORT_SIGNAL, deserializer);
  });
  return KJ_ASSERT_NONNULL(kj::mv(clone));
}

// Registers and unregisters an abort action on `signal` REGISTRATIONS times.
void churnAbortActions(jsg::Lock& js, AbortSignal& signal) {
  for (uint i = 0; i < REGISTRATIONS; i++) {
    auto registration = signal.addAbortAction(js, [](jsg::Lock&, const kj::Exception&) {});
  }
}

KJ_TEST("an idle actor queues one request to arm a received signal's RPC subscription") {
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setAbortSignalRpc(true);
  TestFixture fixture({.featureFlags = flags.asReader(),
    .autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .actorId = Worker::Actor::Id(kj::str("idle-receiver-test"))});
  auto actorContext = fixture.newIoContext();
  auto receiverQueue = actorContext->getCrossContextExecutor();
  auto statelessContext = fixture.newStatelessIoContext();
  auto sender = fixture.newIncomingRequest(*statelessContext);
  // Simulates outstanding I/O, so hang detection doesn't abort the sender when the loop is idle.
  auto senderPendingEvent = statelessContext->registerPendingEvent();

  kj::Maybe<jsg::Ref<AbortSignal>> signal;
  kj::Maybe<jsg::Ref<AbortSignal>> clone;
  fixture.enterContext(
      *sender, [&](TestFixture::Environment& env) { signal = env.js.alloc<AbortSignal>(); });
  auto receiving = fixture.newIncomingRequest(*actorContext);
  clone = sendOverRpc(fixture, *sender, *KJ_ASSERT_NONNULL(signal), *receiving);
  fixture.drainAndDestroy(kj::mv(receiving));

  // The abort reaches the idle receiver, but nothing there observes the clone, so its
  // subscription stays unarmed.
  fixture.enterContext(*sender, [&](TestFixture::Environment& env) {
    KJ_ASSERT_NONNULL(signal)->triggerAbort(
        env.js, kj::OneOf<kj::Exception, jsg::JsValue>(jsg::JsValue(env.js.str("done"_kj))));
  });
  fixture.pollEventLoop();

  // The sender's registrations queue one request on the receiver to arm it, which stays queued.
  uint delivered = 0;
  kj::Own<void> observer;
  fixture.enterContext(*sender, [&](TestFixture::Environment& env) {
    auto& received = *KJ_ASSERT_NONNULL(clone);
    churnAbortActions(env.js, received);
    observer = received.addAbortAction(
        env.js, [&delivered](jsg::Lock&, const kj::Exception&) { ++delivered; });
  });
  fixture.pollEventLoop();
  KJ_EXPECT(receiverQueue.getQueuedActionCountForTest() == 1,
      receiverQueue.getQueuedActionCountForTest());
  KJ_EXPECT(delivered == 0, delivered);

  // The receiver's next request arms the subscription, delivering the abort.
  auto next = fixture.newIncomingRequest(*actorContext);
  fixture.pollEventLoop();
  KJ_EXPECT(delivered == 1, delivered);
  KJ_EXPECT(receiverQueue.getQueuedActionCountForTest() == 0,
      receiverQueue.getQueuedActionCountForTest());
  KJ_EXPECT(actorContext->getAbortReason() == kj::none, actorContext->getAbortReason());

  fixture.drainAndDestroy(kj::mv(next));
  fixture.enterContext(*sender, [&](TestFixture::Environment&) {
    observer = nullptr;
    clone = kj::none;
    signal = kj::none;
  });
  fixture.drainAndDestroy(kj::mv(sender));
}

// Has another context make REGISTRATIONS registrations on a signal that an actor request received
// over RPC, while that request is current but its input lock is held. Returns how many requests to
// arm the signal's RPC subscription the actor queued meanwhile, then checks that they run without
// aborting it.
size_t countArmRequestsQueuedOnBusyReceiver(TestFixture& fixture) {
  auto actorContext = fixture.newIoContext();
  auto receiverQueue = actorContext->getCrossContextExecutor();
  auto statelessContext = fixture.newStatelessIoContext();
  auto sender = fixture.newIncomingRequest(*statelessContext);
  // Simulates outstanding I/O, so hang detection doesn't abort the sender when the loop is idle.
  auto senderPendingEvent = statelessContext->registerPendingEvent();
  auto receiving = fixture.newIncomingRequest(*actorContext);

  kj::Maybe<jsg::Ref<AbortSignal>> signal;
  kj::Maybe<jsg::Ref<AbortSignal>> clone;
  fixture.enterContext(
      *sender, [&](TestFixture::Environment& env) { signal = env.js.alloc<AbortSignal>(); });
  clone = sendOverRpc(fixture, *sender, *KJ_ASSERT_NONNULL(signal), *receiving);

  // Aborted, so that the subscription completes once armed, letting the receiver's request
  // finish draining.
  fixture.enterContext(*sender, [&](TestFixture::Environment& env) {
    KJ_ASSERT_NONNULL(signal)->triggerAbort(
        env.js, kj::OneOf<kj::Exception, jsg::JsValue>(jsg::JsValue(env.js.str("done"_kj))));
  });
  fixture.pollEventLoop();

  // Holding the receiver's input lock keeps the requests to arm the subscription queued.
  auto inputLock = fixture.getActor().getInputGate().wait(nullptr).wait(fixture.getWaitScope());
  fixture.enterContext(*sender,
      [&](TestFixture::Environment& env) { churnAbortActions(env.js, *KJ_ASSERT_NONNULL(clone)); });
  fixture.pollEventLoop();
  size_t queued = receiverQueue.getQueuedActionCountForTest();

  { auto released = kj::mv(inputLock); }
  fixture.pollEventLoop();
  KJ_EXPECT(receiverQueue.getQueuedActionCountForTest() == 0,
      receiverQueue.getQueuedActionCountForTest());
  KJ_EXPECT(actorContext->getAbortReason() == kj::none, actorContext->getAbortReason());

  fixture.drainAndDestroy(kj::mv(receiving));
  fixture.enterContext(*sender, [&](TestFixture::Environment&) {
    clone = kj::none;
    signal = kj::none;
  });
  fixture.drainAndDestroy(kj::mv(sender));
  return queued;
}

KJ_TEST("registrations from other contexts queue one request per received signal to arm it") {
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setAbortSignalRpc(true);
  TestFixture fixture({.featureFlags = flags.asReader(),
    .autogateKeys = kj::arr(util::AutogateKey::DEFER_CROSS_CONTEXT_ACTIONS),
    .actorId = Worker::Actor::Id(kj::str("busy-receiver-test"))});
  auto queued = countArmRequestsQueuedOnBusyReceiver(fixture);
  KJ_EXPECT(queued == 1, queued);
}

KJ_TEST("without the autogate, each registration from another context queues a request to arm a "
        "received signal") {
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setAbortSignalRpc(true);
  TestFixture fixture({.featureFlags = flags.asReader(),
    .ignoreAllAutogatesEnv = true,
    .actorId = Worker::Actor::Id(kj::str("busy-receiver-test"))});
  auto queued = countArmRequestsQueuedOnBusyReceiver(fixture);
  KJ_EXPECT(queued == REGISTRATIONS, queued);
}

}  // namespace
}  // namespace workerd::api
