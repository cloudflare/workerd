#pragma once

#include <workerd/io/io-context.h>
#include <workerd/io/trace.h>
#include <workerd/io/tracer.h>
#include <workerd/io/worker-interface.h>
#include <workerd/util/checked-queue.h>

namespace workerd::tracing {

class TailStreamTarget final: public rpc::TailStreamTarget::Server, public kj::Refcounted {
 public:
  TailStreamTarget(IoContext& ioContext,
      kj::Maybe<kj::StringPtr> entrypointNamePtr,
      Frankenvalue props,
      kj::Own<kj::PromiseFulfiller<void>> doneFulfiller);

  KJ_DISALLOW_COPY_AND_MOVE(TailStreamTarget);
  ~TailStreamTarget();

  kj::Promise<void> report(ReportContext reportContext) override;

 private:
  struct SharedResults;
  // Handles the very first (onset) event in the tail stream. This will cause
  // the exported tailStream handler to be called, passing the onset event
  // as the initial argument. If the tail stream wishes to continue receiving
  // events for this invocation, it will return a handler in the form of an
  // object or a function. If no handler is returned, the tail session is
  // shutdown.
  kj::Promise<void> handleOnset(Worker::Lock& lock,
      IoContext& ioContext,
      kj::Array<tracing::TailEvent> events,
      kj::Rc<SharedResults> results);

  kj::Promise<void> handleEvents(Worker::Lock& lock,
      const jsg::JsValue& handler,
      IoContext& ioContext,
      kj::Array<tracing::TailEvent> events,
      kj::Rc<SharedResults> results);

  kj::Own<IoContext::WeakRef> weakIoContext;
  kj::Maybe<kj::StringPtr> entrypointNamePtr;
  Frankenvalue props;
  // The done fulfiller is resolved when we receive the outcome event
  // or rejected if the capability is dropped before receiving the outcome
  // event.
  kj::Own<kj::PromiseFulfiller<void>> doneFulfiller;

  // The maybeHandler will be empty until we receive and process the
  // onset event.
 public:
  kj::Maybe<jsg::JsRef<jsg::JsValue>> maybeHandler;
  bool hasDestroyedHandler = false;

 private:
  // Indicates that we told (or should have told) the client that we want no further events, used
  // to debug events arriving when the IoContext is no longer valid.
  bool doneReceiving = false;
};

// A WorkerInterface::CustomEvent implementation used to deliver streaming tail
// events to a tail worker.
class TailStreamCustomEventImpl final: public WorkerInterface::CustomEvent {
 public:
  TailStreamCustomEventImpl(uint16_t typeId = TYPE,
      kj::PromiseFulfillerPair<rpc::TailStreamTarget::Client> paf =
          kj::newPromiseAndFulfiller<rpc::TailStreamTarget::Client>())
      : capFulfiller(kj::mv(paf.fulfiller)),
        clientCap(kj::mv(paf.promise)),
        typeId(typeId) {}
  ~TailStreamCustomEventImpl();

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

  kj::Maybe<tracing::EventInfo> getEventInfo() const override;

  // Specify same type as with TraceCustomEventImpl here by default.
  static constexpr uint16_t TYPE = 2;

  rpc::TailStreamTarget::Client getCap() {
    auto result = kj::mv(KJ_ASSERT_NONNULL(clientCap, "can only call getCap() once"));
    clientCap = kj::none;
    return result;
  }

 private:
  kj::Own<kj::PromiseFulfiller<workerd::rpc::TailStreamTarget::Client>> capFulfiller;
  kj::Maybe<rpc::TailStreamTarget::Client> clientCap;
  // Reference used to deallocate the JSG handler to ensure it does not outlive the isolate.
  kj::Maybe<kj::Own<TailStreamTarget>> target;
  uint16_t typeId;
};

// The TailStreamWriterState holds the current client-side state for a collection
// of streaming tail workers that a worker is reporting events to.
struct TailStreamWriterState {
  // The initial state of our tail worker writer is that it is pending the first
  // onset event. During this time we will only have a collection of WorkerInterface
  // instances. When our first event is reported (the onset) we will arrange to acquire
  // tailStream capabilities from each then use those to report the initial onset.
  using Pending = kj::Array<kj::Own<WorkerInterface>>;

  // Instances of Active are refcounted. The TailStreamWriterState itself
  // holds the initial ref. Whenever events are being dispatched, an additional
  // ref will be held by the outstanding pump promise in order to keep the
  // client stub alive long enough for the rpc calls to complete. It is possible
  // that the TailStreamWriterState will be dropped while pump promises are still
  // pending.
  struct Active: public kj::Refcounted {
    // Reference to keep the worker interface instance alive.
    kj::Maybe<rpc::TailStreamTarget::Client> capability;
    bool pumping = false;
    bool onsetSeen = false;
    workerd::util::Queue<tracing::TailEvent> queue;

    Active(rpc::TailStreamTarget::Client capability): capability(kj::mv(capability)) {}
  };

  struct Closed {};

  // The closing flag will be set when the Outcome event has been reported.
  // Once closing is true, no further events will be accepted and the state
  // will transition to closed once the currently active pump completes.
  bool closing = false;
  kj::OneOf<Pending, kj::Array<kj::Own<Active>>, Closed> inner;
  kj::TaskSet& waitUntilTasks;

  TailStreamWriterState(Pending pending, kj::TaskSet& waitUntilTasks)
      : inner(kj::mv(pending)),
        waitUntilTasks(waitUntilTasks) {}
  KJ_DISALLOW_COPY_AND_MOVE(TailStreamWriterState);

  void reportImpl(tracing::TailEvent&& event);

  // Delivers the queued tail events to a streaming tail worker.
  kj::Promise<void> pump(kj::Own<Active> current);
};

kj::Maybe<kj::Own<tracing::TailStreamWriter>> initializeTailStreamWriter(
    kj::Array<kj::Own<WorkerInterface>> streamingTailWorkers, kj::TaskSet& waitUntilTasks);

}  // namespace workerd::tracing
