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

// Forwards upgrade requests arriving from the far side of one end to the other end.
class UpgradeForward {
 public:
  UpgradeForward(InboundTlsUpgrade& request,
      RelayDirection& direction,
      kj::Maybe<kj::Function<kj::Promise<void>()>>& startTls)
      : request(request),
        direction(direction),
        startTls(startTls) {}
  KJ_DISALLOW_COPY_AND_MOVE(UpgradeForward);

  // Waits for the request, then upgrades the stream `direction` writes to on the requester's
  // behalf, and answers the request with the outcome.
  kj::Promise<void> run();

  // Answers the request with `failure` if it has been made and not yet answered. For when the
  // relay ends while the upgrade is under way: the requester would otherwise hear only that the
  // relay went away, not why.
  void abandon(const kj::Exception& failure) {
    if (awaitingAnswer) answer(failure.clone());
  }

 private:
  InboundTlsUpgrade& request;
  RelayDirection& direction;
  kj::Maybe<kj::Function<kj::Promise<void>()>>& startTls;
  bool awaitingAnswer = false;

  void answer(kj::Maybe<kj::Exception> failure) {
    awaitingAnswer = false;
    request.answer(kj::mv(failure));
  }
};

kj::Promise<void> UpgradeForward::run() {
  // A far side that goes away without asking leaves nothing to forward.
  bool requested = co_await request.whenRequested().then(
      []() { return true; }, [](kj::Exception&&) { return false; });
  if (!requested) co_return;
  awaitingAnswer = true;

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
    answer(e.clone());
    kj::throwFatalException(kj::mv(e));
  }
  answer(kj::none);
}

}  // namespace

kj::Promise<void> relayStreams(RelayEnd a, RelayEnd b) {
  RelayDirection aToB(*a.stream, *b.stream);
  RelayDirection bToA(*b.stream, *a.stream);

  kj::Vector<kj::Own<UpgradeForward>> forwards;
  KJ_IF_SOME(request, a.inboundUpgrade) {
    forwards.add(kj::heap<UpgradeForward>(*request, aToB, b.startTls));
  }
  KJ_IF_SOME(request, b.inboundUpgrade) {
    forwards.add(kj::heap<UpgradeForward>(*request, bToA, a.startTls));
  }
  // Forwarding can fail the relay, but only the two directions can finish it.
  auto forwarding = kj::joinPromisesFailFast(KJ_MAP(forward, forwards) {
    return forward->run();
  }).then([]() -> kj::Promise<void> { return kj::NEVER_DONE; });

  KJ_TRY {
    co_await kj::joinPromisesFailFast(kj::arr(aToB.run(), bToA.run()))
        .exclusiveJoin(kj::mv(forwarding));
  }
  KJ_CATCH(e) {
    for (auto& forward: forwards) {
      forward->abandon(e);
    }
    kj::throwFatalException(kj::mv(e));
  }

  // Both directions can end while an upgrade is still under way, if both far sides end theirs.
  auto ended = KJ_EXCEPTION(
      DISCONNECTED, "jsg.Error: The proxied connection ended before the upgrade completed.");
  for (auto& forward: forwards) {
    forward->abandon(ended);
  }
}

}  // namespace workerd::api
