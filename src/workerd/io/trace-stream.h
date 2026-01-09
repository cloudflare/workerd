#pragma once

#include <workerd/io/io-context.h>
#include <workerd/io/trace.h>
#include <workerd/io/tracer.h>
#include <workerd/io/worker-interface.h>
#include <workerd/util/checked-queue.h>

namespace workerd::tracing {

// A WorkerInterface::CustomEvent implementation used to deliver streaming tail
// events to a tail worker.
class TailStreamCustomEvent final: public WorkerInterface::CustomEvent {
 public:
  TailStreamCustomEvent(uint16_t typeId = TYPE,
      kj::PromiseFulfillerPair<rpc::TailStreamTarget::Client> paf =
          kj::newPromiseAndFulfiller<rpc::TailStreamTarget::Client>())
      : capFulfiller(kj::mv(paf.fulfiller)),
        clientCap(kj::mv(paf.promise)),
        typeId(typeId) {}

  ~TailStreamCustomEvent() noexcept(false) {
    if (capFulfiller->isWaiting()) {
      capFulfiller->reject(
          KJ_EXCEPTION(DISCONNECTED, "TailStreamCustomEvent was destroyed before completion"));
    }
  }

  kj::Promise<Result> run(kj::Own<IoContext::IncomingRequest> incomingRequest,
      kj::Maybe<kj::StringPtr> entrypointName,
      Frankenvalue props,
      kj::TaskSet& waitUntilTasks) override;

  kj::Promise<Result> sendRpc(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      rpc::EventDispatcher::Client dispatcher) override;

  kj::Promise<Result> notSupported() override {
    JSG_FAIL_REQUIRE(TypeError, "The receiver is not a tail stream");
  }

  uint16_t getType() override {
    return typeId;
  }

  tracing::EventInfo getEventInfo() const override;

  void failed(const kj::Exception& e) override {
    capFulfiller->reject(kj::cp(e));
  }

  // Specify same type as with TraceCustomEvent here by default.
  static constexpr uint16_t TYPE = 2;

  rpc::TailStreamTarget::Client getCap() {
    auto result = kj::mv(KJ_ASSERT_NONNULL(clientCap, "can only call getCap() once"));
    clientCap = kj::none;
    return result;
  }

 private:
  kj::Own<kj::PromiseFulfiller<workerd::rpc::TailStreamTarget::Client>> capFulfiller;
  kj::Maybe<rpc::TailStreamTarget::Client> clientCap;
  uint16_t typeId;
};

// A utility class that receives tracing events and generates/reports TailEvents.
class TailStreamWriter final {
 public:
  // The initial state of our tail worker writer is that it is pending the first onset event. During
  // this time we will only have a collection of WorkerInterface instances. When our first event is
  // reported (the onset) we will arrange to acquire tailStream capabilities from each then use
  // those to report the initial onset.
  using Pending = kj::Array<kj::Own<WorkerInterface>>;
  TailStreamWriter(Pending pending, kj::TaskSet& waitUntilTasks);
  KJ_DISALLOW_COPY_AND_MOVE(TailStreamWriter);

  void report(const InvocationSpanContext& context, TailEvent::Event&& event, kj::Date time);

 private:
  // Instances of Active are refcounted. The TailStreamWriter itself holds the initial ref. Whenever
  // events are being dispatched, an additional ref will be held by the outstanding pump promise in
  // order to keep the client stub alive long enough for the rpc calls to complete. It is possible
  // that the TailStreamWriter will be dropped while pump promises are still pending.
  struct Active: public kj::Refcounted {
    // Reference to keep the worker interface instance alive.
    kj::Maybe<rpc::TailStreamTarget::Client> capability;
    bool pumping = false;
    bool onsetSeen = false;
    workerd::util::Queue<TailEvent> queue;

    Active(rpc::TailStreamTarget::Client capability): capability(kj::mv(capability)) {}
  };

  struct Closed {};

  kj::OneOf<Pending, kj::Vector<kj::Own<Active>>, Closed> inner;
  kj::TaskSet& waitUntilTasks;

  static kj::Promise<void> pump(kj::Own<Active> current);
  bool reportImpl(TailEvent&& event);

  uint32_t sequence = 0;
  bool onsetSeen = false;
  bool outcomeSeen = false;
};

kj::Maybe<kj::Own<tracing::TailStreamWriter>> initializeTailStreamWriter(
    kj::Array<kj::Own<WorkerInterface>> streamingTailWorkers, kj::TaskSet& waitUntilTasks);

}  // namespace workerd::tracing
