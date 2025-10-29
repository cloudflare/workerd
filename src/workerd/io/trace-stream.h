#pragma once

#include <workerd/io/io-context.h>
#include <workerd/io/trace.h>
#include <workerd/io/tracer.h>
#include <workerd/io/worker-interface.h>
#include <workerd/util/checked-queue.h>

namespace workerd::tracing {

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
  uint16_t typeId;
};

kj::Maybe<kj::Own<tracing::TailStreamWriter>> initializeTailStreamWriter(
    kj::Array<kj::Own<WorkerInterface>> streamingTailWorkers, kj::TaskSet& waitUntilTasks);

}  // namespace workerd::tracing
