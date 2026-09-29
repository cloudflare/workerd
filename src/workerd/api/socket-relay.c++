// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "socket-relay.h"

#include <kj/debug.h>

namespace workerd::api {

namespace {

constexpr size_t RELAY_BUFFER_SIZE = 16384;

// One direction of a relay. Its writes can be held back while the stream it writes to changes
// underneath it.
class RelayDirection {
 public:
  RelayDirection(kj::AsyncInputStream& from, kj::AsyncIoStream& to): from(from), to(to) {}
  KJ_DISALLOW_COPY_AND_MOVE(RelayDirection);

  kj::Promise<void> run() {
    auto buffer = kj::heapArray<kj::byte>(RELAY_BUFFER_SIZE);
    for (;;) {
      size_t amount = co_await from.tryRead(buffer.begin(), 1, buffer.size());
      if (amount == 0) break;

      KJ_IF_SOME(change, transportChange) {
        co_await change.addBranch();
      }

      auto write = to.write(buffer.first(amount)).fork();
      auto written = write.addBranch();
      inFlightWrite = kj::mv(write);
      co_await written;
      inFlightWrite = kj::none;
    }
    to.shutdownWrite();
  }

  // Runs `change` once the write in flight, if any, has finished, and holds back every write after
  // it until `change` has resolved. The result is `change`'s.
  //
  // Only one change is ever made to a stream, since a stream can only be upgraded once.
  kj::Promise<void> changeTransport(kj::Function<kj::Promise<void>()> change) {
    KJ_ASSERT(transportChange == kj::none, "the transport of a relayed stream changes only once");

    kj::Promise<void> drained = kj::READY_NOW;
    KJ_IF_SOME(write, inFlightWrite) {
      drained = write.addBranch();
    }
    auto changed = drained.then(kj::mv(change)).fork();
    auto result = changed.addBranch();
    transportChange = kj::mv(changed);
    return result;
  }

 private:
  kj::AsyncInputStream& from;
  kj::AsyncIoStream& to;
  kj::Maybe<kj::ForkedPromise<void>> inFlightWrite;
  kj::Maybe<kj::ForkedPromise<void>> transportChange;
};

// Waits for `request`, then upgrades the stream `direction` writes to on the requester's behalf.
kj::Promise<void> forwardUpgrade(InboundTlsUpgrade& request,
    RelayDirection& direction,
    kj::Maybe<kj::Function<kj::Promise<void>()>>& startTls) {
  // A far side that goes away without asking leaves nothing to forward.
  bool requested = co_await request.whenRequested().then(
      []() { return true; }, [](kj::Exception&&) { return false; });
  if (!requested) co_return;

  // The far side asks only once every byte it sent before asking has been consumed from its
  // transport, so those bytes have already been read by `direction`. The continuations that hand
  // them on to its write run without waiting on I/O, so once the event loop has nothing else to
  // do, the last of them is either written or is the write in flight.
  co_await kj::yieldUntilQueueEmpty();

  KJ_TRY {
    auto& start = KJ_REQUIRE_NONNULL(startTls,
        "jsg.Error: The peer asked to start TLS, but the socket it is proxied to does not "
        "support startTls().");
    co_await direction.changeTransport(kj::mv(start));
  }
  KJ_CATCH(e) {
    request.answer(e.clone());
    kj::throwFatalException(kj::mv(e));
  }
  request.answer(kj::none);
}

}  // namespace

kj::Promise<void> relayStreams(RelayEnd a, RelayEnd b) {
  RelayDirection aToB(*a.stream, *b.stream);
  RelayDirection bToA(*b.stream, *a.stream);

  kj::Vector<kj::Promise<void>> forwards;
  KJ_IF_SOME(request, a.inboundUpgrade) {
    forwards.add(forwardUpgrade(*request, aToB, b.startTls));
  }
  KJ_IF_SOME(request, b.inboundUpgrade) {
    forwards.add(forwardUpgrade(*request, bToA, a.startTls));
  }
  // Forwarding can fail the relay, but only the two directions can finish it.
  auto forwarding =
      kj::joinPromisesFailFast(forwards.releaseAsArray()).then([]() -> kj::Promise<void> {
    return kj::NEVER_DONE;
  });

  co_await kj::joinPromisesFailFast(kj::arr(aToB.run(), bToA.run()))
      .exclusiveJoin(kj::mv(forwarding));
}

}  // namespace workerd::api
