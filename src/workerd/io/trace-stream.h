#pragma once

#include <workerd/io/io-context.h>
#include <workerd/io/trace.h>
#include <workerd/io/worker-interface.h>

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

  // TODO(streaming-tail-workers): Specify the correct type as specified in the
  // internal capnp definition.
  static constexpr uint16_t TYPE = 11;

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
  // If the Reporter returns false, then the writer should transition into a
  // closed state.
  using Reporter = kj::Function<bool(TailEvent&&)>;

  // A callback that provides the timestamps for tail stream events.
  // Ideally this uses the same time context as IoContext:now().
  using TimeSource = kj::Function<kj::Date()>;
  TailStreamWriter(Reporter reporter, TimeSource timeSource);
  KJ_DISALLOW_COPY_AND_MOVE(TailStreamWriter);

  void report(const InvocationSpanContext& context, TailEvent::Event&& event);
  inline void report(const InvocationSpanContext& context, Mark&& event) {
    report(context, TailEvent::Event(kj::mv(event)));
  }

  inline bool isClosed() const {
    return state == kj::none;
  }

 private:
  struct State {
    Reporter reporter;
    TimeSource timeSource;
    uint32_t sequence = 0;
    bool onsetSeen = false;
    State(Reporter reporter, TimeSource timeSource)
        : reporter(kj::mv(reporter)),
          timeSource(kj::mv(timeSource)) {}
  };
  kj::Maybe<State> state;
};

}  // namespace workerd::tracing
