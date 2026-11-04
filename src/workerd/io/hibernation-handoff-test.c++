// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Tests for handing a HibernationManager's WebSockets to another manager.

#include "hibernation-manager-test-fixture.h"

#include <workerd/io/hibernation-handoff.h>
#include <workerd/io/legacy-hibernation-manager.h>
#include <workerd/io/worker.h>
#include <workerd/tests/test-fixture.h>

#include <kj/compat/http.h>
#include <kj/test.h>

namespace workerd {
namespace {

using test::acceptNewWebSocket;
using test::DispatchStats;
using test::makeTestHm;
using test::stubLoopbackParams;

// A transport for a WebSocket that acceptNewWebSocket() accepts. The test plays the transport: it
// sends the client's messages on the eyeball end, and counts them in `sentCount`, which a pause
// reports.
struct FakeTransport {
  uint64_t sentCount = 0;
  uint pauseCount = 0;

  // True while a pause's lease is held.
  bool paused = false;

  // When set, pause() waits until the test fulfills `heldPause`.
  bool holdPauses = false;
  kj::Maybe<kj::Own<kj::PromiseFulfiller<void>>> heldPause;

  // When set, pause() fails, as it does once the transport's connection has ended.
  bool failPauses = false;

  kj::Own<HibernatableTransportHandle> makeHandle();
};

class FakeTransportHandle final: public HibernatableTransportHandle {
 public:
  explicit FakeTransportHandle(FakeTransport& transport): transport(transport) {}

  kj::Promise<Pause> pause() override {
    KJ_REQUIRE(!transport.paused, "transport is already paused");
    ++transport.pauseCount;
    if (transport.failPauses) {
      kj::throwFatalException(KJ_EXCEPTION(DISCONNECTED, "the transport's connection ended"));
    }
    if (transport.holdPauses) {
      auto paf = kj::newPromiseAndFulfiller<void>();
      transport.heldPause = kj::mv(paf.fulfiller);
      co_await paf.promise;
    }
    transport.paused = true;
    co_return Pause{
      .sentCount = transport.sentCount,
      .token = kj::heapArray<kj::byte>(32),
      .timeLimit = 30 * kj::SECONDS,
      .lease = kj::heap<Lease>(transport),
    };
  }

 private:
  class Lease final: public PauseLease {
   public:
    explicit Lease(FakeTransport& transport): transport(transport) {}
    ~Lease() noexcept(false) {
      transport.paused = false;
    }

    kj::Promise<bool> confirmEnded() override {
      KJ_UNIMPLEMENTED("the manager doesn't end transports");
    }

   private:
    FakeTransport& transport;
  };

  FakeTransport& transport;
};

kj::Own<HibernatableTransportHandle> FakeTransport::makeHandle() {
  return kj::heap<FakeTransportHandle>(*this);
}

void hibernateAll(TestFixture& fixture, Worker::Actor::HibernationManager& hm) {
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm.hibernateWebSockets(lock); });
}

DetachedHibernatableWebSockets detachAll(
    TestFixture& fixture, Worker::Actor::HibernationManager& hm) {
  return hm.detachWebSocketsForHandoff(kj::NEVER_DONE).wait(fixture.getWaitScope())->take();
}

// The blocker carried by the failure of `func`, which must fail.
template <typename Func>
kj::Maybe<WebSocketHandoffBlocker> blockerOfFailure(Func&& func) {
  auto failure = kj::runCatchingExceptions(kj::fwd<Func>(func));
  return getWebSocketHandoffBlocker(KJ_ASSERT_NONNULL(failure, "expected a failure"));
}

KJ_TEST("HibernationManager: hibernated WebSockets with transports can be handed off") {
  FakeTransport transport;
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("handoff-check")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto request = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED =
      acceptNewWebSocket(fixture, *request, *hm, "tag"_kj, transport.makeHandle());
  KJ_EXPECT(hm->checkWebSocketHandoff(1024) == WebSocketHandoffBlocker::WEBSOCKET_AWAKE);

  hibernateAll(fixture, *hm);
  KJ_EXPECT(hm->checkWebSocketHandoff(1024) == kj::none);

  // The state is the auto-response, the tag, which is the only WebSocket property this test sets,
  // and the allowances for the socket and its tag.
  size_t stateBytes = "pingpongtag"_kj.size() + WEBSOCKET_HANDOFF_BYTES_PER_SOCKET +
      WEBSOCKET_HANDOFF_BYTES_PER_TAG;
  KJ_EXPECT(hm->checkWebSocketHandoff(stateBytes) == kj::none);
  KJ_EXPECT(hm->checkWebSocketHandoff(stateBytes - 1) == WebSocketHandoffBlocker::STATE_TOO_LARGE);

  auto end2 KJ_UNUSED = acceptNewWebSocket(fixture, *request, *hm);
  hibernateAll(fixture, *hm);
  KJ_EXPECT(hm->checkWebSocketHandoff(1024) == WebSocketHandoffBlocker::NO_TRANSPORT);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: a WebSocket that sent its Close can't be handed off") {
  FakeTransport transport;
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("handoff-check-closed")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm, "tag"_kj, transport.makeHandle());

  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    hm->getWebSockets(env.js, kj::none)[0]->close(env.js, 1000, jsg::USVString(kj::str("bye")));
  });
  KJ_ASSERT(end1->receive().wait(fixture.getWaitScope()).is<kj::WebSocket::Close>());
  hibernateAll(fixture, *hm);
  KJ_EXPECT(hm->checkWebSocketHandoff(1024) == WebSocketHandoffBlocker::WEBSOCKET_CLOSED);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: an event in progress blocks the handoff") {
  FakeTransport transport;
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("handoff-check-event")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm, "tag"_kj, transport.makeHandle());
  hibernateAll(fixture, *hm);

  // The event waits for a loopback.
  auto handoff = hm->beginLoopbackHandoff();
  ++transport.sentCount;
  end1->send("hello"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();
  KJ_EXPECT(hm->checkWebSocketHandoff(1024) == WebSocketHandoffBlocker::EVENT_IN_PROGRESS);
  KJ_EXPECT(blockerOfFailure([&]() {
    hm->detachWebSocketsForHandoff(kj::NEVER_DONE).wait(fixture.getWaitScope());
  }) == WebSocketHandoffBlocker::EVENT_IN_PROGRESS);
  KJ_EXPECT(transport.pauseCount == 0, transport.pauseCount);

  handoff = nullptr;
  fixture.pollEventLoop();
  KJ_EXPECT(stats.customEventCalls == 1, stats.customEventCalls);
  KJ_EXPECT(hm->checkWebSocketHandoff(1024) == kj::none);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: detaching requires events to wait for a loopback") {
  FakeTransport transport;
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("handoff-detach-loopback")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED =
      acceptNewWebSocket(fixture, *request, *hm, "tag"_kj, transport.makeHandle());
  hibernateAll(fixture, *hm);

  KJ_EXPECT_THROW_MESSAGE("requires events to wait for a loopback",
      hm->detachWebSocketsForHandoff(kj::NEVER_DONE).wait(fixture.getWaitScope()));

  auto handoff = hm->beginLoopbackHandoff();
  ++transport.sentCount;
  auto detach = hm->detachWebSocketsForHandoff(kj::NEVER_DONE);
  KJ_ASSERT(!detach.poll(fixture.getWaitScope()));
  KJ_EXPECT_THROW_MESSAGE("already being detached",
      hm->detachWebSocketsForHandoff(kj::NEVER_DONE).wait(fixture.getWaitScope()));
  KJ_EXPECT(transport.pauseCount == 1, transport.pauseCount);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: detaching waits for each WebSocket to receive what was sent to it") {
  FakeTransport transport;
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("handoff-detach-wait")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm, "tag"_kj, transport.makeHandle());
  hibernateAll(fixture, *hm);
  auto handoff = hm->beginLoopbackHandoff();

  // The transport has sent a message that the WebSocket hasn't received yet.
  ++transport.sentCount;
  auto detach = hm->detachWebSocketsForHandoff(kj::NEVER_DONE);
  KJ_ASSERT(!detach.poll(fixture.getWaitScope()));
  KJ_EXPECT(transport.paused);

  // It matches the auto-response, which needs no actor, but detaching waits for the reply too.
  end1->send("ping"_kj).wait(fixture.getWaitScope());
  KJ_ASSERT(!detach.poll(fixture.getWaitScope()));
  auto reply = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(reply.is<kj::String>() && reply.get<kj::String>() == "pong"_kj);

  // The WebSocket stays in the manager until it's taken.
  auto detachment = detach.wait(fixture.getWaitScope());
  KJ_EXPECT(hm->getWebSocketCount() == 1);
  auto detached = detachment->take();
  KJ_EXPECT(hm->getWebSocketCount() == 0);
  KJ_EXPECT(stats.customEventCalls == 0, stats.customEventCalls);
  KJ_ASSERT(detached.sockets.size() == 1);
  KJ_EXPECT(KJ_ASSERT_NONNULL(detached.sockets[0].pause).sentCount == 1);
  KJ_EXPECT(detached.sockets[0].autoResponseTimestamp != kj::none);

  // The read loop was waiting for the next message, so the WebSocket brings that receive along.
  KJ_EXPECT(detached.sockets[0].pendingReceive != kj::none);

  // The pause lasts as long as the detached WebSocket keeps it.
  KJ_EXPECT(transport.paused);
  detached.sockets[0].pause = kj::none;
  KJ_EXPECT(!transport.paused);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: an event abandons detachment and resumes the transports") {
  FakeTransport transport;
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("handoff-detach-event")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm, "tag"_kj, transport.makeHandle());
  hibernateAll(fixture, *hm);
  auto handoff = hm->beginLoopbackHandoff();

  ++transport.sentCount;
  auto detach = hm->detachWebSocketsForHandoff(kj::NEVER_DONE);
  KJ_ASSERT(!detach.poll(fixture.getWaitScope()));

  // The message needs the actor, so its event waits for a loopback.
  end1->send("hello"_kj).wait(fixture.getWaitScope());
  KJ_EXPECT(blockerOfFailure([&]() { detach.wait(fixture.getWaitScope()); }) ==
      WebSocketHandoffBlocker::EVENT_IN_PROGRESS);
  KJ_EXPECT(!transport.paused);
  KJ_EXPECT(hm->getWebSocketCount() == 1);

  handoff = nullptr;
  fixture.pollEventLoop();
  KJ_EXPECT(stats.customEventCalls == 1, stats.customEventCalls);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: a loopback arriving abandons detachment") {
  FakeTransport transport;
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("handoff-detach-loopback-arrives")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED =
      acceptNewWebSocket(fixture, *request, *hm, "tag"_kj, transport.makeHandle());
  hibernateAll(fixture, *hm);
  auto handoff = hm->beginLoopbackHandoff();

  ++transport.sentCount;
  auto detach = hm->detachWebSocketsForHandoff(kj::NEVER_DONE);
  KJ_ASSERT(!detach.poll(fixture.getWaitScope()));

  // Dropping the handle puts the loopback back.
  handoff = nullptr;
  KJ_EXPECT(blockerOfFailure([&]() { detach.wait(fixture.getWaitScope()); }) ==
      WebSocketHandoffBlocker::LOOPBACK_ARRIVED);
  KJ_EXPECT(!transport.paused);
  KJ_EXPECT(hm->getWebSocketCount() == 1);

  fixture.drainAndDestroy(kj::mv(request));
}

// A manager whose only WebSocket, carried by `transport`, is ready to be taken for a handoff.
struct ReadiedHandoff {
  TestFixture& fixture;
  IoContext::IncomingRequest& request;
  Worker::Actor::HibernationManager& hm;
  FakeTransport& transport;
};

// Readies a handoff, makes `change`, and checks that the change abandoned the handoff with
// `blocker`, leaving the WebSocket in place with its transport resumed.
void expectChangeAbandonsHandoff(kj::StringPtr actorId,
    WebSocketHandoffBlocker blocker,
    kj::FunctionParam<void(ReadiedHandoff&)> change) {
  FakeTransport transport;
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str(actorId)));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED =
      acceptNewWebSocket(fixture, *request, *hm, "tag"_kj, transport.makeHandle());
  hibernateAll(fixture, *hm);
  auto handoff = hm->beginLoopbackHandoff();

  auto detachment = hm->detachWebSocketsForHandoff(kj::NEVER_DONE).wait(fixture.getWaitScope());
  KJ_ASSERT(transport.paused);
  ReadiedHandoff readied{fixture, *request, *hm, transport};
  change(readied);
  KJ_EXPECT(!transport.paused, actorId);
  KJ_EXPECT(blockerOfFailure([&]() { detachment->take(); }) == blocker, actorId);
  KJ_EXPECT(hm->getWebSocketCount() >= 1, actorId);

  // The handoff is over once its result is dropped, so another can start.
  KJ_EXPECT(LegacyHibernationManagerTestAccess::isDetaching(*hm), actorId);
  detachment = nullptr;
  KJ_EXPECT(!LegacyHibernationManagerTestAccess::isDetaching(*hm), actorId);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: changing the WebSockets before they're taken abandons the handoff") {
  expectChangeAbandonsHandoff("handoff-abandon-wake"_kj, WebSocketHandoffBlocker::WEBSOCKET_AWAKE,
      [](ReadiedHandoff& readied) {
    readied.fixture.enterContext(readied.request, [&](const TestFixture::Environment& env) {
      KJ_EXPECT(readied.hm.getWebSockets(env.js, kj::none).size() == 1);
    });
  });

  expectChangeAbandonsHandoff("handoff-abandon-attach"_kj,
      WebSocketHandoffBlocker::WEBSOCKETS_CHANGED, [](ReadiedHandoff& readied) {
    LegacyHibernationManagerTestAccess::getOnlyTransport(
        kj::downcast<LegacyHibernationManagerImpl>(readied.hm))
        ->attach(readied.transport.makeHandle());
  });

  expectChangeAbandonsHandoff("handoff-abandon-auto-response"_kj,
      WebSocketHandoffBlocker::WEBSOCKETS_CHANGED,
      [](ReadiedHandoff& readied) { readied.hm.setWebSocketAutoResponse("ping"_kj, "pong"_kj); });

  expectChangeAbandonsHandoff("handoff-abandon-event-timeout"_kj,
      WebSocketHandoffBlocker::WEBSOCKETS_CHANGED,
      [](ReadiedHandoff& readied) { readied.hm.setEventTimeout(1234); });

  expectChangeAbandonsHandoff("handoff-abandon-loopback"_kj,
      WebSocketHandoffBlocker::LOOPBACK_ARRIVED, [](ReadiedHandoff& readied) {
    readied.hm.setLoopback(readied.fixture.getActor().getLoopback());
  });
}

KJ_TEST("HibernationManager: accepting a WebSocket before they're taken abandons the handoff") {
  FakeTransport transport;
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("handoff-abandon-accept")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED =
      acceptNewWebSocket(fixture, *request, *hm, "tag"_kj, transport.makeHandle());
  hibernateAll(fixture, *hm);
  auto handoff = hm->beginLoopbackHandoff();

  auto detachment = hm->detachWebSocketsForHandoff(kj::NEVER_DONE).wait(fixture.getWaitScope());
  auto end2 KJ_UNUSED = acceptNewWebSocket(fixture, *request, *hm);
  KJ_EXPECT(!transport.paused);
  KJ_EXPECT(
      blockerOfFailure([&]() { detachment->take(); }) == WebSocketHandoffBlocker::WEBSOCKET_AWAKE);
  KJ_EXPECT(hm->getWebSocketCount() == 2);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: cancelling detachment leaves the WebSockets running") {
  FakeTransport transport;
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("handoff-detach-cancel")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm, "tag"_kj, transport.makeHandle());
  hibernateAll(fixture, *hm);
  auto handoff = hm->beginLoopbackHandoff();

  ++transport.sentCount;
  {
    auto detach = hm->detachWebSocketsForHandoff(kj::NEVER_DONE);
    KJ_ASSERT(!detach.poll(fixture.getWaitScope()));
    KJ_EXPECT(transport.paused);
  }
  KJ_EXPECT(!transport.paused);
  KJ_EXPECT(hm->getWebSocketCount() == 1);

  // The read loop still runs.
  handoff = nullptr;
  end1->send("hello"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();
  KJ_EXPECT(stats.customEventCalls == 1, stats.customEventCalls);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: dropping a detachment without taking leaves the WebSockets running") {
  FakeTransport transport;
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("handoff-detach-drop")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm, "tag"_kj, transport.makeHandle());
  hibernateAll(fixture, *hm);
  auto handoff = hm->beginLoopbackHandoff();

  auto detachment = hm->detachWebSocketsForHandoff(kj::NEVER_DONE).wait(fixture.getWaitScope());
  KJ_EXPECT(transport.paused);
  detachment = nullptr;
  KJ_EXPECT(!transport.paused);
  KJ_EXPECT(hm->getWebSocketCount() == 1);

  // The read loop still runs, and the WebSocket can be handed off later.
  handoff = nullptr;
  ++transport.sentCount;
  end1->send("hello"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();
  KJ_EXPECT(stats.customEventCalls == 1, stats.customEventCalls);
  KJ_EXPECT(hm->checkWebSocketHandoff(1024) == kj::none);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: a WebSocket that received more than was sent to it isn't detached") {
  FakeTransport transport;
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("handoff-detach-overcount")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm, "tag"_kj, transport.makeHandle());
  hibernateAll(fixture, *hm);
  auto handoff = hm->beginLoopbackHandoff();

  transport.holdPauses = true;
  auto detach = hm->detachWebSocketsForHandoff(kj::NEVER_DONE);
  KJ_ASSERT(!detach.poll(fixture.getWaitScope()));
  KJ_ASSERT(transport.pauseCount == 1, transport.pauseCount);

  // The transport doesn't count this message.
  end1->send("ping"_kj).wait(fixture.getWaitScope());
  KJ_ASSERT(end1->receive().wait(fixture.getWaitScope()).is<kj::String>());
  KJ_ASSERT_NONNULL(transport.heldPause)->fulfill();
  KJ_EXPECT(blockerOfFailure([&]() { detach.wait(fixture.getWaitScope()); }) ==
      WebSocketHandoffBlocker::TRANSPORT_MISCOUNTED);
  KJ_EXPECT(!transport.paused);
  KJ_EXPECT(hm->getWebSocketCount() == 1);

  fixture.drainAndDestroy(kj::mv(request));
}

// Two hibernated WebSockets, tagged "a" and "b" in the order they were accepted, whose events wait
// for a loopback. Both answer "ping" with the auto-response "pong".
struct TwoWebSocketHandoff {
  explicit TwoWebSocketHandoff(kj::StringPtr actorId)
      : fixture(stubLoopbackParams(stats, kj::str(actorId))),
        hm(makeTestHm(fixture, "ping"_kj, "pong"_kj)),
        request(fixture.newIncomingRequest()),
        endA(acceptNewWebSocket(fixture, *request, *hm, "a"_kj, transportA.makeHandle())),
        endB(acceptNewWebSocket(fixture, *request, *hm, "b"_kj, transportB.makeHandle())) {
    hibernateAll(fixture, *hm);
    handoff = hm->beginLoopbackHandoff();
  }

  kj::WaitScope& getWaitScope() {
    return fixture.getWaitScope();
  }

  void drain() {
    fixture.drainAndDestroy(kj::mv(request));
  }

  FakeTransport transportA;
  FakeTransport transportB;
  DispatchStats stats;
  TestFixture fixture;
  kj::Rc<Worker::Actor::HibernationManager> hm;
  kj::Own<IoContext::IncomingRequest> request;
  kj::Own<kj::WebSocket> endA;
  kj::Own<kj::WebSocket> endB;
  kj::Own<void> handoff;
};

// The tags of the detached WebSockets, in order.
kj::String describeTags(const DetachedHibernatableWebSockets& detached) {
  return kj::strArray(
      KJ_MAP(socket, detached.sockets) { return kj::strArray(socket.tags, "+"); }, ",");
}

KJ_TEST("HibernationManager: a WebSocket whose transport fails to pause is left out") {
  TwoWebSocketHandoff test("handoff-left-out-pause-fails"_kj);
  test.transportB.failPauses = true;
  auto detachment = test.hm->detachWebSocketsForHandoff(kj::NEVER_DONE).wait(test.getWaitScope());
  KJ_EXPECT(detachment->getLeftOutCount() == 1, detachment->getLeftOutCount());
  auto detached = detachment->take();
  KJ_EXPECT(describeTags(detached) == "a"_kj, describeTags(detached));
  KJ_EXPECT(test.transportA.paused);

  // The WebSocket left out stays in the manager, and a later handoff can take it.
  KJ_EXPECT(test.hm->getWebSocketCount() == 1);
  test.transportB.failPauses = false;
  KJ_EXPECT(describeTags(detachAll(test.fixture, *test.hm)) == "b"_kj);

  test.drain();
}

KJ_TEST(
    "HibernationManager: a WebSocket whose transport hasn't paused by the deadline is left out") {
  TwoWebSocketHandoff test("handoff-left-out-pause-hangs"_kj);
  test.transportB.holdPauses = true;
  auto deadline = kj::newPromiseAndFulfiller<void>();
  auto detach = test.hm->detachWebSocketsForHandoff(kj::mv(deadline.promise));
  KJ_ASSERT(!detach.poll(test.getWaitScope()));
  KJ_EXPECT(test.transportA.paused);

  deadline.fulfiller->fulfill();
  auto detachment = detach.wait(test.getWaitScope());
  KJ_EXPECT(detachment->getLeftOutCount() == 1, detachment->getLeftOutCount());
  KJ_EXPECT(describeTags(detachment->take()) == "a"_kj);
  KJ_EXPECT(test.hm->getWebSocketCount() == 1);

  // The pause that comes too late was dropped with the rest of the work on the WebSocket.
  KJ_ASSERT_NONNULL(test.transportB.heldPause)->fulfill();
  test.fixture.pollEventLoop();
  KJ_EXPECT(!test.transportB.paused);

  test.drain();
}

KJ_TEST("HibernationManager: a pause that finishes as the deadline passes counts") {
  TwoWebSocketHandoff test("handoff-pause-at-deadline"_kj);
  test.transportB.holdPauses = true;
  auto deadline = kj::newPromiseAndFulfiller<void>();
  auto detach = test.hm->detachWebSocketsForHandoff(kj::mv(deadline.promise));
  KJ_ASSERT(!detach.poll(test.getWaitScope()));

  KJ_ASSERT_NONNULL(test.transportB.heldPause)->fulfill();
  deadline.fulfiller->fulfill();
  auto detachment = detach.wait(test.getWaitScope());
  KJ_EXPECT(detachment->getLeftOutCount() == 0, detachment->getLeftOutCount());
  KJ_EXPECT(describeTags(detachment->take()) == "a,b"_kj);

  test.drain();
}

KJ_TEST("HibernationManager: a WebSocket that hasn't caught up by the deadline is left out") {
  TwoWebSocketHandoff test("handoff-left-out-lagging"_kj);
  // The transport has sent B a message that B hasn't received.
  ++test.transportB.sentCount;
  auto deadline = kj::newPromiseAndFulfiller<void>();
  auto detach = test.hm->detachWebSocketsForHandoff(kj::mv(deadline.promise));
  KJ_ASSERT(!detach.poll(test.getWaitScope()));

  // A starts waiting for its writes after the deadline has passed, and is still taken.
  deadline.fulfiller->fulfill();
  auto detachment = detach.wait(test.getWaitScope());
  KJ_EXPECT(detachment->getLeftOutCount() == 1, detachment->getLeftOutCount());
  KJ_EXPECT(test.transportA.paused && !test.transportB.paused);

  // The message needs the actor, so its event waits for a loopback, without abandoning the
  // handoff.
  test.endB->send("hello"_kj).wait(test.getWaitScope());
  test.fixture.pollEventLoop();
  KJ_EXPECT(describeTags(detachment->take()) == "a"_kj);
  KJ_EXPECT(test.stats.customEventCalls == 0, test.stats.customEventCalls);

  // It runs once the loopback is back.
  test.handoff = nullptr;
  test.fixture.pollEventLoop();
  KJ_EXPECT(test.stats.customEventCalls == 1, test.stats.customEventCalls);
  KJ_EXPECT(test.hm->getWebSocketCount() == 1);

  test.drain();
}

KJ_TEST("HibernationManager: a WebSocket still writing at the deadline is left out") {
  TwoWebSocketHandoff test("handoff-left-out-writing"_kj);
  ++test.transportB.sentCount;
  auto deadline = kj::newPromiseAndFulfiller<void>();
  auto detach = test.hm->detachWebSocketsForHandoff(kj::mv(deadline.promise));

  // B answers the message with the auto-response, which its client doesn't read yet.
  test.endB->send("ping"_kj).wait(test.getWaitScope());
  KJ_ASSERT(!detach.poll(test.getWaitScope()));
  deadline.fulfiller->fulfill();
  auto detachment = detach.wait(test.getWaitScope());
  KJ_EXPECT(detachment->getLeftOutCount() == 1, detachment->getLeftOutCount());
  KJ_EXPECT(describeTags(detachment->take()) == "a"_kj);

  auto reply = test.endB->receive().wait(test.getWaitScope());
  KJ_EXPECT(reply.is<kj::String>() && reply.get<kj::String>() == "pong"_kj);
  KJ_EXPECT(test.stats.customEventCalls == 0, test.stats.customEventCalls);

  test.drain();
}

// Readies a handoff while B still has a message to receive, ends B's connection with `end`, and
// checks that this leaves B out, and that its close event waits for the loopback.
void expectEndLeavesWebSocketOut(
    kj::StringPtr actorId, kj::FunctionParam<void(TwoWebSocketHandoff&)> end) {
  TwoWebSocketHandoff test(actorId);
  ++test.transportB.sentCount;
  auto detach = test.hm->detachWebSocketsForHandoff(kj::NEVER_DONE);
  KJ_ASSERT(!detach.poll(test.getWaitScope()));

  end(test);
  auto detachment = detach.wait(test.getWaitScope());
  KJ_EXPECT(detachment->getLeftOutCount() == 1, actorId, detachment->getLeftOutCount());
  KJ_EXPECT(!test.transportB.paused, actorId);
  KJ_EXPECT(describeTags(detachment->take()) == "a"_kj, actorId);
  KJ_EXPECT(test.stats.customEventCalls == 0, actorId, test.stats.customEventCalls);

  test.handoff = nullptr;
  test.fixture.pollEventLoop();
  KJ_EXPECT(test.stats.customEventCalls == 1, actorId, test.stats.customEventCalls);
  KJ_EXPECT(test.hm->getWebSocketCount() == 0, actorId);

  test.drain();
}

KJ_TEST("HibernationManager: a WebSocket whose connection ends is left out") {
  // The Close is the message B hasn't received.
  expectEndLeavesWebSocketOut("handoff-left-out-close"_kj, [](TwoWebSocketHandoff& test) {
    test.endB->close(1000, "bye"_kj).wait(test.getWaitScope());
  });

  expectEndLeavesWebSocketOut(
      "handoff-left-out-disconnect"_kj, [](TwoWebSocketHandoff& test) { test.endB->abort(); });
}

KJ_TEST("HibernationManager: detaching fails if every WebSocket is left out") {
  TwoWebSocketHandoff test("handoff-left-out-all"_kj);
  test.transportA.failPauses = true;
  test.transportB.failPauses = true;
  KJ_EXPECT_THROW_MESSAGE("every hibernatable WebSocket was left out",
      test.hm->detachWebSocketsForHandoff(kj::NEVER_DONE).wait(test.getWaitScope()));
  KJ_EXPECT(test.hm->getWebSocketCount() == 2);
  KJ_EXPECT(!LegacyHibernationManagerTestAccess::isDetaching(*test.hm));

  test.drain();
}

KJ_TEST("HibernationManager: WebSockets adopted back keep their tags, order and settings") {
  FakeTransport transportA;
  FakeTransport transportB;
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("handoff-adopt-back")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto& legacyHm = kj::downcast<LegacyHibernationManagerImpl>(*hm);
  hm->setEventTimeout(1234);
  auto request = fixture.newIncomingRequest();
  auto endA = acceptNewWebSocket(fixture, *request, *hm, "a"_kj, transportA.makeHandle());
  auto endB = acceptNewWebSocket(fixture, *request, *hm, "b"_kj, transportB.makeHandle());
  hibernateAll(fixture, *hm);
  auto handoff = hm->beginLoopbackHandoff();

  auto detached = detachAll(fixture, *hm);
  KJ_EXPECT(hm->getWebSocketCount() == 0);
  KJ_ASSERT(detached.sockets.size() == 2);
  KJ_EXPECT(detached.sockets[0].tags[0] == "a"_kj);
  KJ_EXPECT(detached.sockets[1].tags[0] == "b"_kj);
  KJ_EXPECT(KJ_ASSERT_NONNULL(detached.autoResponse).request == "ping"_kj);
  KJ_EXPECT(KJ_ASSERT_NONNULL(detached.eventTimeoutMs) == 1234);
  KJ_EXPECT(transportA.paused && transportB.paused);

  // Adopting applies the settings that the WebSockets carry.
  hm->setWebSocketAutoResponse(kj::none, kj::none);
  hm->setEventTimeout(kj::none);
  hm->adoptWebSockets(kj::mv(detached));
  KJ_EXPECT(!transportA.paused && !transportB.paused);
  KJ_EXPECT(LegacyHibernationManagerTestAccess::describeTagsInAcceptOrder(legacyHm) == "a,b"_kj);
  KJ_EXPECT(KJ_ASSERT_NONNULL(hm->getEventTimeout()) == 1234);

  endB->send("ping"_kj).wait(fixture.getWaitScope());
  auto reply = endB->receive().wait(fixture.getWaitScope());
  KJ_EXPECT(reply.is<kj::String>() && reply.get<kj::String>() == "pong"_kj);

  handoff = nullptr;
  endA->send("hello"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();
  KJ_EXPECT(stats.customEventCalls == 1, stats.customEventCalls);
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    KJ_EXPECT(hm->getWebSockets(env.js, "a"_kj).size() == 1);
    KJ_EXPECT(hm->getWebSockets(env.js, "b"_kj).size() == 1);
  });

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: an adopted WebSocket without a pending receive receives anew") {
  FakeTransport transport;
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("handoff-adopt-no-receive")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm, "tag"_kj, transport.makeHandle());
  hibernateAll(fixture, *hm);
  auto handoff = hm->beginLoopbackHandoff();

  // Nothing pumps into this pipe, so the receive can be cancelled without losing anything.
  auto detached = detachAll(fixture, *hm);
  KJ_ASSERT(detached.sockets.size() == 1);
  detached.sockets[0].pendingReceive = kj::none;
  hm->adoptWebSockets(kj::mv(detached));

  handoff = nullptr;
  end1->send("hello"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();
  KJ_EXPECT(stats.customEventCalls == 1, stats.customEventCalls);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: detaching leaves a pump into the WebSocket running") {
  // couple() feeds a WebSocket by pumping into its pipe from the transport. The pipe ties such a
  // pump to the receive that it feeds, so cancelling that receive would end the pump.
  FakeTransport transport;
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("handoff-detach-pump")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm, "tag"_kj, transport.makeHandle());
  auto stream = kj::newTwoWayPipe();
  auto client = kj::newWebSocket(kj::mv(stream.ends[0]), kj::none);
  auto transportEnd = kj::newWebSocket(kj::mv(stream.ends[1]), kj::none);
  auto pump = transportEnd->pumpTo(*end1).eagerlyEvaluate(nullptr);
  hibernateAll(fixture, *hm);
  auto handoff = hm->beginLoopbackHandoff();

  hm->adoptWebSockets(detachAll(fixture, *hm));
  KJ_ASSERT(!pump.poll(fixture.getWaitScope()));

  handoff = nullptr;
  ++transport.sentCount;
  client->send("hello"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();
  KJ_EXPECT(stats.customEventCalls == 1, stats.customEventCalls);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: another manager adopts detached WebSockets") {
  FakeTransport transport;
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("handoff-adopt-other")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto successor = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm, "tag"_kj, transport.makeHandle());
  hibernateAll(fixture, *hm);
  auto handoff = hm->beginLoopbackHandoff();

  successor->adoptWebSockets(detachAll(fixture, *hm));
  KJ_EXPECT(hm->getWebSocketCount() == 0);
  KJ_EXPECT(successor->getWebSocketCount() == 1);
  KJ_EXPECT(!transport.paused);

  // The successor answers with the auto-response that came with the WebSockets.
  end1->send("ping"_kj).wait(fixture.getWaitScope());
  auto reply = end1->receive().wait(fixture.getWaitScope());
  KJ_EXPECT(reply.is<kj::String>() && reply.get<kj::String>() == "pong"_kj);
  KJ_EXPECT(stats.customEventCalls == 0, stats.customEventCalls);

  // The successor's read loop sees the client go away and delivers the close event through its
  // own loopback, while the first manager's events would still wait for one.
  end1->abort();
  fixture.pollEventLoop();
  KJ_EXPECT(stats.customEventCalls == 1, stats.customEventCalls);
  KJ_EXPECT(successor->getWebSocketCount() == 0);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: adopting an auto-response requires a timer channel") {
  FakeTransport transport;
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("handoff-adopt-no-timer")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto successor = kj::rc<LegacyHibernationManagerImpl>(fixture.getActor().getLoopback(), 0);
  auto request = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED =
      acceptNewWebSocket(fixture, *request, *hm, "tag"_kj, transport.makeHandle());
  hibernateAll(fixture, *hm);
  auto handoff = hm->beginLoopbackHandoff();

  KJ_EXPECT_THROW_MESSAGE(
      "requires a timer channel", successor->adoptWebSockets(detachAll(fixture, *hm)));
  KJ_EXPECT(successor->getWebSocketCount() == 0);

  fixture.drainAndDestroy(kj::mv(request));
}

}  // namespace
}  // namespace workerd
