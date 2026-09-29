// capnp RPC integration: capnp::TwoPartyServer / capnp::TwoPartyClient running over kj-rs-io
// (tokio-backed) streams on the tokio event loop — bootstrap plus call round-trips. Uses a
// schema-less capability (raw dispatchCall / typelessRequest with Text payloads) to avoid
// needing capnp codegen in this repo.

#include "kj-rs-io/async-io.h"

#include <capnp/capability.h>
#include <capnp/message.h>
#include <capnp/rpc-twoparty.h>
#include <kj/async.h>
#include <kj/debug.h>
#include <kj/test.h>

namespace kj_rs_io_test {
namespace {

using kj_rs_io::setupTokioAsyncIo;

constexpr uint64_t ECHO_INTERFACE_ID = 0xabcd1234abcd1234ull;
constexpr uint16_t ECHO_METHOD_ID = 0;

// A schema-less capability: method 0 takes a Text param and returns "echo:" + text.
class EchoCapability final: public capnp::Capability::Server {
 public:
  DispatchCallResult dispatchCall(uint64_t interfaceId,
      uint16_t methodId,
      capnp::CallContext<capnp::AnyPointer, capnp::AnyPointer> context) override {
    KJ_ASSERT(interfaceId == ECHO_INTERFACE_ID);
    KJ_ASSERT(methodId == ECHO_METHOD_ID);
    auto params = kj::str(context.getParams().getAs<capnp::Text>());
    context.releaseParams();
    context.getResults(capnp::MessageSize{16, 0}).setAs<capnp::Text>(kj::str("echo:", params));
    return DispatchCallResult{kj::READY_NOW, false, true};
  }
};

KJ_TEST("capnp two-party RPC bootstrap and call round-trips over kj-rs-io "
        "streams on the tokio loop") {
  auto io = setupTokioAsyncIo();
  auto &ws = io.getWaitScope();

  // Server: TwoPartyServer accepting from a kj-rs-io ConnectionReceiver.
  capnp::TwoPartyServer server(kj::heap<EchoCapability>());
  auto listener = io.getNetwork().parseAddress("127.0.0.1", 0).wait(ws)->listen();
  auto listenTask = server.listen(*listener).eagerlyEvaluate(
      [](kj::Exception &&e) { KJ_FAIL_EXPECT("RPC server failed", e); });

  // Client: TwoPartyClient over a kj-rs-io connection.
  auto addr = io.getNetwork().parseAddress(kj::str("127.0.0.1:", listener->getPort())).wait(ws);
  auto connection = addr->connect().wait(ws);
  capnp::TwoPartyClient client(*connection);
  auto cap = client.bootstrap();

  // Single call round trip.
  {
    auto request = cap.typelessRequest(ECHO_INTERFACE_ID, ECHO_METHOD_ID, kj::none, {});
    request.setAs<capnp::Text>("hello tokio");
    auto response = request.send().wait(ws);
    KJ_EXPECT(response.getAs<capnp::Text>() == "echo:hello tokio");
  }

  // A pile of pipelined calls in flight at once.
  {
    constexpr int COUNT = 64;
    auto builder = kj::heapArrayBuilder<kj::Promise<void>>(COUNT);
    for (int i = 0; i < COUNT; i++) {
      auto request = cap.typelessRequest(ECHO_INTERFACE_ID, ECHO_METHOD_ID, kj::none, {});
      request.setAs<capnp::Text>(kj::str("msg", i));
      builder.add(request.send().then([i](capnp::Response<capnp::AnyPointer> response) {
        KJ_EXPECT(response.getAs<capnp::Text>() == kj::str("echo:msg", i));
      }));
    }
    kj::joinPromisesFailFast(builder.finish()).wait(ws);
  }
}

}  // namespace
}  // namespace kj_rs_io_test
