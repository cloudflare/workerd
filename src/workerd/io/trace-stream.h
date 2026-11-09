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
  // `completion`, if given, is kept until the session's work is done. See TraceCompletion in
  // worker-interface.capnp.
  explicit TailStreamCustomEvent(kj::Maybe<rpc::TraceCompletion::Client> completion = kj::none,
      kj::PromiseFulfillerPair<rpc::TailStreamTarget::Client> paf =
          kj::newPromiseAndFulfiller<rpc::TailStreamTarget::Client>())
      : capFulfiller(kj::mv(paf.fulfiller)),
        clientCap(kj::mv(paf.promise)),
        completion(kj::mv(completion)) {}

  // Builds the event that a tailStreamSession() callee runs, keeping the caller's completion.
  static kj::Own<TailStreamCustomEvent> fromRpc(
      rpc::EventDispatcher::TailStreamSessionParams::Reader params);

  ~TailStreamCustomEvent() noexcept(false) {
    if (capFulfiller->isWaiting()) {
      capFulfiller->reject(
          KJ_EXCEPTION(DISCONNECTED, "TailStreamCustomEvent was destroyed before completion"));
    }
  }

  kj::Promise<Result> run(kj::Own<IoContext::IncomingRequest> incomingRequest,
      kj::Maybe<kj::StringPtr> entrypointName,
      kj::Maybe<Worker::VersionInfo> versionInfo,
      Frankenvalue props,
      kj::TaskSet& waitUntilTasks,
      bool isDynamicDispatch) override;

  kj::Promise<Result> sendRpc(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      FrankenvalueHandler& frankenvalueHandler,
      rpc::EventDispatcher::Client dispatcher) override;

  kj::Promise<Result> notSupported() override {
    JSG_FAIL_REQUIRE(TypeError, "The receiver is not a tail stream");
  }

  uint16_t getType() override {
    return TYPE;
  }

  tracing::EventInfo getEventInfo() const override;

  void failed(const kj::Exception& e) override {
    capFulfiller->reject(e.clone());
  }

  // Specify the event type for TailStreamCustomEvent (defined internally).
  static constexpr uint16_t TYPE = 12;

  // Sets `completion` (see the constructor) on an event created without one.
  void setCompletion(rpc::TraceCompletion::Client value) {
    completion = kj::mv(value);
  }

  rpc::TailStreamTarget::Client getCap() {
    auto result = kj::mv(KJ_ASSERT_NONNULL(clientCap, "can only call getCap() once"));
    clientCap = kj::none;
    return result;
  }

 private:
  kj::Own<kj::PromiseFulfiller<workerd::rpc::TailStreamTarget::Client>> capFulfiller;
  kj::Maybe<rpc::TailStreamTarget::Client> clientCap;
  kj::Maybe<rpc::TraceCompletion::Client> completion;
};

// A utility class that receives tracing events and generates/reports TailEvents.
class TailStreamWriter final {
 public:
  // The most memory, in bytes, that one session may hold in events it has queued or sent but not
  // had acknowledged. Beyond this, events are dropped. This fits about 16K short logs.
  static constexpr size_t MAX_SESSION_BYTES = 4 * 1024 * 1024;

  // The memory a queued event holds: the TailEvent itself, its queue node, and what it owns on the
  // heap, which `sizeHint` approximates.
  static constexpr size_t queuedEventSize(size_t sizeHint) {
    return sizeof(TailEvent) + 2 * sizeof(void*) + sizeHint;
  }

  // The initial state of our tail worker writer is that it is pending the first onset event. During
  // this time we will only have a collection of WorkerInterface instances. When our first event is
  // reported (the onset) we will arrange to acquire tailStream capabilities from each then use
  // those to report the initial onset.
  //
  // If `budget` is given, every session's held events count against it as well as against
  // MAX_SESSION_BYTES.
  using Pending = kj::Array<kj::Own<WorkerInterface>>;
  TailStreamWriter(Pending pending,
      kj::TaskSet& waitUntilTasks,
      kj::Maybe<kj::Rc<TailStreamBudget>> budget = kj::none);
  KJ_DISALLOW_COPY_AND_MOVE(TailStreamWriter);

  void report(const InvocationSpanContext& context,
      TailEvent::Event&& event,
      kj::Date time,
      size_t sizeHint);

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
    // The estimated memory held by the events in `queue`.
    size_t queuedBytes = 0;
    // The estimated memory held by events that have been sent but not acknowledged, which the
    // serialized batch and the tail worker's copy of it still hold.
    size_t sentBytes = 0;
    // The estimated memory of the onset event, which is sent on its own.
    size_t onsetBytes = 0;
    // The number of tail events we had to drop. We'll send a warning indicating this at the end of
    // the stream.
    uint32_t droppedEvents = 0;
    workerd::util::Queue<TailEvent> queue;
    kj::Maybe<kj::Rc<TailStreamBudget>> budget;

    Active(rpc::TailStreamTarget::Client capability, kj::Maybe<kj::Rc<TailStreamBudget>> budget)
        : capability(kj::mv(capability)),
          budget(kj::mv(budget)) {}
    ~Active() noexcept(false);

    // Holds `bytes` for an event about to be queued, if neither this session nor the budget would
    // exceed its limit.
    bool tryHold(size_t bytes);
    // Holds `bytes` for an event that must not be dropped.
    void hold(size_t bytes);
    // Moves `bytes` of queued events to sent.
    void markSent(size_t bytes);
    // Gives back `bytes` of sent events, once the tail worker has acknowledged them.
    void releaseSent(size_t bytes);
    // Drops all queued events and gives back everything held, once the session has stopped.
    void stop();
  };

  struct Closed {};

  kj::OneOf<Pending, kj::Vector<kj::Own<Active>>, Closed> inner;
  kj::TaskSet& waitUntilTasks;
  kj::Maybe<kj::Rc<TailStreamBudget>> budget;

  static kj::Promise<void> pump(kj::Own<Active> current);
  // Report an event to the tail stream writer.
  // sizeHint: The approximate size of the event, in bytes.
  bool reportImpl(TailEvent&& event, size_t sizeHint);

  uint32_t sequence = 0;
  bool onsetSeen = false;
  bool outcomeSeen = false;
};

kj::Maybe<kj::Own<tracing::TailStreamWriter>> initializeTailStreamWriter(
    kj::Array<kj::Own<WorkerInterface>> streamingTailWorkers,
    kj::TaskSet& waitUntilTasks,
    kj::Maybe<kj::Rc<TailStreamBudget>> budget = kj::none);

}  // namespace workerd::tracing
