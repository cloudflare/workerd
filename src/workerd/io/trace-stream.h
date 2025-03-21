#pragma once

#include <workerd/io/io-context.h>
#include <workerd/io/trace.h>
#include <workerd/io/tracer.h>
#include <workerd/io/worker-interface.h>

#include <list>

namespace workerd::tracing {

// A WorkerInterface::CustomEvent implementation used to deliver streaming tail
// events to a tail worker.
class TailStreamCustomEventImpl final: public WorkerInterface::CustomEvent {
 public:
  TailStreamCustomEventImpl(bool isLegacy,
      kj::Maybe<kj::Rc<PipelineTracer>> pipelineTracer,
      uint16_t typeId = TYPE,
      kj::PromiseFulfillerPair<rpc::TailStreamTarget::Client> paf =
          kj::newPromiseAndFulfiller<rpc::TailStreamTarget::Client>())
      : isLegacy(isLegacy),
        pipelineTracer(kj::mv(pipelineTracer)),
        capFulfiller(kj::mv(paf.fulfiller)),
        clientCap(kj::mv(paf.promise)),
        typeId(typeId) {}

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

  // TODO(streaming-tail-workers): Specify the correct type as specified in the
  // internal capnp definition.
  static constexpr uint16_t TYPE = 11;

  rpc::TailStreamTarget::Client getCap() {
    auto result = kj::mv(KJ_ASSERT_NONNULL(clientCap, "can only call getCap() once"));
    clientCap = kj::none;
    return result;
  }

 private:
  bool isLegacy;
  kj::Maybe<kj::Rc<PipelineTracer>> pipelineTracer;
  kj::Own<kj::PromiseFulfiller<workerd::rpc::TailStreamTarget::Client>> capFulfiller;
  kj::Maybe<rpc::TailStreamTarget::Client> clientCap;
  uint16_t typeId;
};

struct WorkerInterfaceIsLegacy {
  kj::Own<WorkerInterface> interface;
  bool isLegacy;
};

// The TailStreamWriterState holds the current client-side state for a collection
// of streaming tail workers that a worker is reporting events to.
struct TailStreamWriterState {
  // The initial state of our tail worker writer is that it is pending the first
  // onset event. During this time we will only have a collection of WorkerInterface
  // instances. When our first event is reported (the onset) we will arrange to acquire
  // tailStream capabilities from each then use those to report the initial onset.
  using Pending = kj::Array<WorkerInterfaceIsLegacy>;

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
    std::list<tracing::TailEvent> queue;
    bool isLegacyTail;

    Active(rpc::TailStreamTarget::Client capability, bool isLegacyTail)
        : capability(kj::mv(capability)),
          isLegacyTail(isLegacyTail) {}
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
    kj::Array<kj::Own<WorkerInterface>> legacyTailWorkers,
    kj::Array<kj::Own<WorkerInterface>> streamingTailWorkers,
    kj::TaskSet& waitUntilTasks,
    // TODO: Be careful to surrender pipeline reference in time.
    kj::Maybe<kj::Rc<PipelineTracer>> pipelineTracer);

}  // namespace workerd::tracing
