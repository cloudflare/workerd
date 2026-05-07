// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0
//
// Tests for HibernationManager behavior. The tests interact with the abstract
// HibernationManager interface so that the same suite can run against any
// concrete implementation that may exist over time (autogated in production).
//
// A note on the code comments throughout this file: they mix descriptions of
// the implementation as it stands today with motivations and references to
// in-progress refactor work. They may go stale relative to the current
// implementation as that work lands. The tests themselves are the source of
// truth for the contract; comments are best-effort context.
//
// A few tests use KJ_EXPECT_LOG to capture the production "another message
// send is already in progress" assertion as an expected ERROR log. They pass
// while the bug is present and fail loudly when the fix lands. Search the
// file for "regression test for EW-10817" to find them.

#include <workerd/api/web-socket.h>
#include <workerd/io/hibernation-manager.h>
#include <workerd/io/worker-interface.h>
#include <workerd/io/worker.h>
#include <workerd/tests/test-fixture.h>

#include <kj/compat/http.h>
#include <kj/test.h>

#if KJ_HAS_COMPILER_FEATURE(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#include <sanitizer/lsan_interface.h>
#endif

namespace workerd {

namespace {

// ============================================================================
// Test fixtures
// ============================================================================

// Counts callbacks observed by StubLoopback / StubWorkerInterface so tests can
// assert dispatch behavior (e.g., auto-response should NOT dispatch).
struct DispatchStats {
  uint getWorkerCalls = 0;
  uint customEventCalls = 0;
};

// Minimal WorkerInterface for tests. Returns success on customEvent (so the HM's
// readLoop continues normally) and counts calls. All other methods are
// unimplemented — this is only suitable for tests that exercise the
// hibernation event dispatch path, which goes through customEvent().
class StubWorkerInterface final: public WorkerInterface {
 public:
  explicit StubWorkerInterface(DispatchStats& stats): stats(stats) {}

  kj::Promise<WorkerInterface::CustomEvent::Result> customEvent(
      kj::Own<WorkerInterface::CustomEvent> event) override {
    ++stats.customEventCalls;
    return WorkerInterface::CustomEvent::Result{.outcome = EventOutcome::OK};
  }

  kj::Promise<void> request(kj::HttpMethod,
      kj::StringPtr,
      const kj::HttpHeaders&,
      kj::AsyncInputStream&,
      kj::HttpService::Response&) override {
    KJ_UNIMPLEMENTED("StubWorkerInterface::request not used");
  }
  kj::Promise<void> connect(kj::StringPtr,
      const kj::HttpHeaders&,
      kj::AsyncIoStream&,
      ConnectResponse&,
      kj::HttpConnectSettings) override {
    KJ_UNIMPLEMENTED("StubWorkerInterface::connect not used");
  }
  kj::Promise<void> prewarm(kj::StringPtr) override {
    KJ_UNIMPLEMENTED("StubWorkerInterface::prewarm not used");
  }
  kj::Promise<ScheduledResult> runScheduled(kj::Date, kj::StringPtr) override {
    KJ_UNIMPLEMENTED("StubWorkerInterface::runScheduled not used");
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date, uint32_t) override {
    KJ_UNIMPLEMENTED("StubWorkerInterface::runAlarm not used");
  }

 private:
  DispatchStats& stats;
};

// Test loopback that hands out StubWorkerInterfaces and counts getWorker calls.
class StubLoopback final: public Worker::Actor::Loopback, public kj::Refcounted {
 public:
  explicit StubLoopback(DispatchStats& stats): stats(stats) {}

  kj::Own<WorkerInterface> getWorker(IoChannelFactory::SubrequestMetadata) override {
    ++stats.getWorkerCalls;
    return kj::heap<StubWorkerInterface>(stats);
  }

  kj::Own<Worker::Actor::Loopback> addRef() override {
    return kj::addRef(*this);
  }

 private:
  DispatchStats& stats;
};

// Helpers below are intentionally split so the HibernationManager can outlive any single
// IncomingRequest, which matters for tests that span multiple IRs. makeTestHm() needs no
// IoContext; acceptNewWebSocket() and sendFromDo() do (the api::WebSocket constructor stores
// IoOwn members, and ws.send() is delivered through the IoContext's pump).

// SetupParams builder that installs a StubLoopback on the actor referencing `stats`. The
// caller MUST keep `stats` alive for the lifetime of the resulting TestFixture (declare it
// before the fixture). The same StubLoopback is later retrieved via actor.getLoopback() and
// handed to the HM, so actor and HM share a single Loopback (mirroring production).
TestFixture::SetupParams stubLoopbackParams(DispatchStats& stats, kj::String actorId) {
  return {
    .actorId = Worker::Actor::Id(kj::mv(actorId)),
    .useRealTimers = true,
    .actorLoopback = kj::refcounted<StubLoopback>(stats),
  };
}

// Create a HibernationManager. The constructor (and setTimerChannel) don't need an IoContext;
// production typically constructs the HM inside one only because the trigger — a JS call to
// state.acceptWebSocket — runs in one. The HM itself is IoContext-independent and this test
// pattern keeps that explicit so any inadvertent dependency growth shows up.
kj::Own<Worker::Actor::HibernationManager> makeTestHm(TestFixture& fixture) {
  auto hm = kj::refcounted<HibernationManagerImpl>(fixture.getActor().getLoopback(), 0);
  hm->setTimerChannel(fixture.getTimerChannel());
  return hm;
}

// Same, but configure auto-response. Both `autoRequest` and `autoResponse` are required.
kj::Own<Worker::Actor::HibernationManager> makeTestHm(
    TestFixture& fixture, kj::StringPtr autoRequest, kj::StringPtr autoResponse) {
  auto hm = makeTestHm(fixture);
  using AutoResponseData = kj::OneOf<kj::StringPtr, kj::ArrayPtr<const kj::byte>>;
  hm->setWebSocketAutoResponse(kj::Maybe<AutoResponseData>(AutoResponseData(autoRequest)),
      kj::Maybe<AutoResponseData>(AutoResponseData(autoResponse)));
  return hm;
}

kj::Own<Worker::Actor::HibernationManager> makeTestHm(TestFixture& fixture,
    kj::ArrayPtr<const kj::byte> autoRequest,
    kj::ArrayPtr<const kj::byte> autoResponse) {
  auto hm = makeTestHm(fixture);
  using AutoResponseData = kj::OneOf<kj::StringPtr, kj::ArrayPtr<const kj::byte>>;
  hm->setWebSocketAutoResponse(kj::Maybe<AutoResponseData>(AutoResponseData(autoRequest)),
      kj::Maybe<AutoResponseData>(AutoResponseData(autoResponse)));
  return hm;
}

// Create an api::WebSocket, accept it into the HM under `tag` (or untagged if `tag` is empty),
// and return the eyeball end of the new pipe. Tests can call this multiple times to attach
// multiple concurrent WebSockets; pass distinct tags to identify them later via getWebSockets.
//
// Needs an IoContext (the api::WebSocket constructor stores IoOwn members), supplied by the
// IR. Test code should pick an IR whose IoContext should "own" this api::WebSocket.
kj::Own<kj::WebSocket> acceptNewWebSocket(TestFixture& fixture,
    IoContext::IncomingRequest& request,
    Worker::Actor::HibernationManager& hm,
    kj::StringPtr tag = ""_kj) {
  kj::Own<kj::WebSocket> eyeball;
  fixture.enterContext(request, [&](const TestFixture::Environment& env) {
    auto pipe = kj::newWebSocketPipe();
    eyeball = kj::mv(pipe.ends[0]);
    // TODO(bug) EW-10817: leak a ref so the api::WebSocket survives the AsyncObject destructor
    // issue (resolving EW-10817 will naturally remove the need for this). Tell LSan the leak
    // is intentional so it doesn't fail tests under sanitizer builds.
    auto apiWs = env.js.alloc<api::WebSocket>(env.js, kj::mv(pipe.ends[1]));
    auto* leaked = new jsg::Ref<api::WebSocket>(apiWs.addRef());
#if KJ_HAS_COMPILER_FEATURE(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
    __lsan_ignore_object(leaked);
#else
    (void)leaked;
#endif
    auto tags = kj::heapArray<kj::String>(tag.size() == 0 ? 0 : 1);
    if (tag.size() != 0) tags[0] = kj::str(tag);
    hm.acceptWebSocket(kj::mv(apiWs), tags);
  });
  return eyeball;
}

// Send a string message from the DO side, on the WebSocket identified by `tag` (or the only
// untagged one if `tag` is empty). Enters the supplied IR's IoContext for the duration of
// the send setup; the actual ws.send happens asynchronously after the lock is released.
void sendFromDo(TestFixture& fixture,
    IoContext::IncomingRequest& request,
    Worker::Actor::HibernationManager& hm,
    kj::StringPtr msg,
    kj::StringPtr tag = ""_kj) {
  fixture.enterContext(request, [&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto websockets =
        hm.getWebSockets(js, tag.size() == 0 ? kj::Maybe<kj::StringPtr>(kj::none) : tag);
    KJ_ASSERT(
        websockets.size() == 1, "expected exactly one WebSocket for tag", tag, websockets.size());
    websockets[0]->send(js, kj::OneOf<kj::Array<kj::byte>, kj::String>(kj::str(msg)));
  });
}

KJ_TEST("HibernationManager: smoke (create, accept, query)") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("smoke")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(fixture, *request, *hm);

  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto websockets = hm->getWebSockets(env.js, kj::none);
    KJ_ASSERT(websockets.size() == 1);
  });

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: DO sends string message to eyeball") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("do-send-string")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  sendFromDo(fixture, *request, *hm, "hello"_kj);

  // Drive the pump; the message should arrive at the eyeball end.
  auto msg = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg.is<kj::String>());
  KJ_ASSERT(msg.get<kj::String>() == "hello"_kj);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: eyeball sends non-auto-response message → dispatched to worker") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("eyeball-send")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  // Eyeball sends a message that does NOT match any auto-response config.
  end1->send("hello from eyeball"_kj).wait(fixture.getWaitScope());

  // Give the HM's readLoop time to receive and dispatch.
  fixture.pollEventLoop();

  KJ_ASSERT(stats.customEventCalls >= 1, "expected at least one customEvent dispatch",
      stats.customEventCalls);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: DO close sends close frame to eyeball") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("do-close")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto websockets = hm->getWebSockets(js, kj::none);
    KJ_ASSERT(websockets.size() == 1);
    websockets[0]->close(js, 1001, jsg::USVString(kj::str("bye")));
  });

  // The eyeball should receive a Close message.
  auto msg = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg.is<kj::WebSocket::Close>());
  auto& close = msg.get<kj::WebSocket::Close>();
  KJ_ASSERT(close.code == 1001, close.code);
  KJ_ASSERT(close.reason == "bye"_kj, close.reason);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: eyeball close dispatches webSocketClose to worker") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("eyeball-close")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  // Eyeball closes the WS. The HM's readLoop should observe the close and dispatch a
  // webSocketClose event to the worker via customEvent.
  end1->close(1001, "eyeball bye"_kj).wait(fixture.getWaitScope());

  fixture.pollEventLoop();

  KJ_ASSERT(stats.customEventCalls >= 1, "expected at least one customEvent dispatch",
      stats.customEventCalls);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: DO sends binary message to eyeball") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("do-send-bin")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto websockets = hm->getWebSockets(js, kj::none);
    KJ_ASSERT(websockets.size() == 1);
    auto bytes = kj::heapArray<kj::byte>({0xde, 0xad, 0xbe, 0xef});
    websockets[0]->send(js, kj::OneOf<kj::Array<kj::byte>, kj::String>(kj::mv(bytes)));
  });

  auto msg = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg.is<kj::Array<kj::byte>>());
  auto& bytes = msg.get<kj::Array<kj::byte>>();
  KJ_ASSERT(bytes.size() == 4);
  KJ_ASSERT(bytes[0] == 0xde && bytes[1] == 0xad && bytes[2] == 0xbe && bytes[3] == 0xef);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: eyeball sends binary message → dispatched to worker") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("eyeball-send-bin")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  auto bytes = kj::heapArray<kj::byte>({0xca, 0xfe, 0xba, 0xbe});
  end1->send(bytes.asPtr()).wait(fixture.getWaitScope());

  fixture.pollEventLoop();
  KJ_ASSERT(stats.customEventCalls >= 1, "expected at least one customEvent dispatch",
      stats.customEventCalls);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: multiple tagged WebSockets are addressable independently") {
  // Accept two WebSockets under distinct tags. getWebSockets(js, tag) should return only the
  // matching one; getWebSockets(js, kj::none) returns both. DO-side sends, scoped via tag,
  // reach only the addressed eyeball.
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("multi-ws")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto aliceEnd1 = acceptNewWebSocket(fixture, *request, *hm, "alice"_kj);
  auto bobEnd1 = acceptNewWebSocket(fixture, *request, *hm, "bob"_kj);

  // The HM tracks both; getWebSockets without a tag returns the union.
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto& js = env.js;
    KJ_ASSERT(hm->getWebSockets(js, kj::none).size() == 2);
    KJ_ASSERT(hm->getWebSockets(js, "alice"_kj).size() == 1);
    KJ_ASSERT(hm->getWebSockets(js, "bob"_kj).size() == 1);
  });

  // DO sends a message addressed to alice; only alice's eyeball gets it.
  sendFromDo(fixture, *request, *hm, "for-alice"_kj, "alice"_kj);
  auto msgA = aliceEnd1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msgA.is<kj::String>() && msgA.get<kj::String>() == "for-alice"_kj);

  // Bob should have received nothing yet.
  auto bobReceive = bobEnd1->receive();
  fixture.pollEventLoop();
  KJ_ASSERT(!bobReceive.poll(fixture.getWaitScope()), "bob should not have received anything yet");

  // Now send to bob; the previous receive promise resolves.
  sendFromDo(fixture, *request, *hm, "for-bob"_kj, "bob"_kj);
  auto msgB = bobReceive.wait(fixture.getWaitScope());
  KJ_ASSERT(msgB.is<kj::String>() && msgB.get<kj::String>() == "for-bob"_kj);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: auto-response request not dispatched to worker (active)") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("autoresp-active")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  // Eyeball sends a message that matches the auto-response request.
  end1->send("ping"_kj).wait(fixture.getWaitScope());

  // The HM should reply with the configured response and NOT dispatch to the worker.
  auto msg = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg.is<kj::String>());
  KJ_ASSERT(msg.get<kj::String>() == "pong"_kj);

  // Wait for any potential dispatch (there shouldn't be one).
  fixture.pollEventLoop();
  KJ_ASSERT(stats.customEventCalls == 0, "auto-response should not dispatch to worker",
      stats.customEventCalls);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: auto-response not dispatched to worker (hibernated)") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("autoresp-hibernated")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  // Hibernate before any messages flow.
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });

  // Eyeball sends a ping. The HM's hibernated-mode readLoop should send pong directly
  // (bypassing the pump, which has no IoContext during hibernation) and NOT dispatch.
  end1->send("ping"_kj).wait(fixture.getWaitScope());
  auto msg = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg.is<kj::String>() && msg.get<kj::String>() == "pong"_kj);

  fixture.pollEventLoop();
  KJ_ASSERT(
      stats.customEventCalls == 0, "auto-response should not dispatch", stats.customEventCalls);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: auto-response interleaved with DO sends (active)") {
  // Verifies that, in active mode, auto-response pongs interleaved with DO-side sends all
  // arrive at the eyeball without tripping the "another message send is already in progress"
  // assertion. The pump and sendAutoResponse synchronize on ongoingAutoResponse in active
  // mode; if that synchronization breaks this test will trip the bug class targeted by
  // EW-10817 — but in active mode it should hold.
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("autoresp-interleaved")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  sendFromDo(fixture, *request, *hm, "before"_kj);
  end1->send("ping"_kj).wait(fixture.getWaitScope());
  sendFromDo(fixture, *request, *hm, "after"_kj);

  // Drain three messages from the eyeball. The order isn't guaranteed; verify the set.
  bool sawBefore = false, sawPong = false, sawAfter = false;
  for (int i = 0; i < 3; ++i) {
    auto msg = end1->receive().wait(fixture.getWaitScope());
    KJ_ASSERT(msg.is<kj::String>(), "expected string message", i);
    auto& s = msg.get<kj::String>();
    if (s == "before"_kj)
      sawBefore = true;
    else if (s == "pong"_kj)
      sawPong = true;
    else if (s == "after"_kj)
      sawAfter = true;
    else
      KJ_FAIL_ASSERT("unexpected message", s);
  }
  KJ_ASSERT(sawBefore && sawPong && sawAfter);

  KJ_ASSERT(stats.customEventCalls == 0, "auto-response should not dispatch to worker",
      stats.customEventCalls);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: comm across multiple IncomingRequests sharing an IoContext") {
  // The actor pattern: a single IoContext outlives any one IncomingRequest. The api::WebSocket
  // is bound to the IoContext (via IoOwn members), not to any specific IR, so it must remain
  // usable as IRs come and go.
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("multi-ir-serial")));
  auto hm = makeTestHm(fixture);
  auto context = fixture.newIoContext();

  // Request 1: accept a WS, send a message from the DO side, receive it on the eyeball.
  // (We must read the message before draining; the pump's send blocks on a reader.)
  auto request1 = fixture.newIncomingRequest(*context);
  auto end1 = acceptNewWebSocket(fixture, *request1, *hm);
  sendFromDo(fixture, *request1, *hm, "from-r1"_kj);
  auto msg1 = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg1.is<kj::String>());
  KJ_ASSERT(msg1.get<kj::String>() == "from-r1"_kj);
  fixture.drainAndDestroy(kj::mv(request1));

  // Request 2: same IoContext, same WS; send another message and receive it.
  auto request2 = fixture.newIncomingRequest(*context);
  sendFromDo(fixture, *request2, *hm, "from-r2"_kj);
  auto msg2 = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg2.is<kj::String>());
  KJ_ASSERT(msg2.get<kj::String>() == "from-r2"_kj);
  fixture.drainAndDestroy(kj::mv(request2));
}

KJ_TEST("HibernationManager: two concurrent IncomingRequests sharing an IoContext") {
  // Two IncomingRequests delivered against the same IoContext, with overlapping lifetimes.
  // This is a real production pattern: e.g. a chat-room DO might be handling a message from
  // one user (one IR) and concurrently fan it out to another user, where the fan-out is
  // structured as a second IR against the same actor. The IoContext model accommodates this
  // — the second's delivered() just makes the first non-current — and work routed via either
  // IR's enterContext lands on the single shared IoContext correctly.
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("multi-ir-parallel")));
  auto hm = makeTestHm(fixture);
  auto context = fixture.newIoContext();

  auto request1 = fixture.newIncomingRequest(*context);
  auto end1 = acceptNewWebSocket(fixture, *request1, *hm);
  auto request2 = fixture.newIncomingRequest(*context);  // IR1 still alive at this point.

  // Send via IR1; the IoContext is shared, so this works even though IR2 is "current".
  sendFromDo(fixture, *request1, *hm, "from-r1"_kj);
  auto msg1 = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg1.is<kj::String>() && msg1.get<kj::String>() == "from-r1"_kj);

  // Send via IR2.
  sendFromDo(fixture, *request2, *hm, "from-r2"_kj);
  auto msg2 = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg2.is<kj::String>() && msg2.get<kj::String>() == "from-r2"_kj);

  // Destroy the older IR first; IR2 keeps working.
  fixture.drainAndDestroy(kj::mv(request1));
  sendFromDo(fixture, *request2, *hm, "from-r2-again"_kj);
  auto msg3 = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg3.is<kj::String>() && msg3.get<kj::String>() == "from-r2-again"_kj);

  fixture.drainAndDestroy(kj::mv(request2));
}

// ---------- Same-IoContext hibernation flows ----------

KJ_TEST("HibernationManager: comm survives hibernation/revival within one IoContext") {
  // The classic hibernation flow: the HM's activeOrPackage transitions from
  // jsg::Ref<api::WebSocket> to HibernationPackage, then a fresh api::WebSocket is
  // materialized on demand by getWebSockets(). This works as long as no message is
  // in-flight on the pipe at the moment hibernation runs (the in-flight cases are tested
  // separately below).
  //
  // This test stays within a single IoContext. See the cross-IoContext variant further down
  // for the production-style flow where the actor is also evicted and recreated.
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("hibernate-survive")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  // Round-trip a message, fully drained, before hibernation.
  sendFromDo(fixture, *request, *hm, "before-hib"_kj);
  auto msg1 = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg1.is<kj::String>() && msg1.get<kj::String>() == "before-hib"_kj);

  // Hibernate. Replaces the active api::WebSocket on the HM with a HibernationPackage.
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });

  // After hibernation, getWebSockets should rebuild a fresh api::WebSocket from the package.
  sendFromDo(fixture, *request, *hm, "after-hib"_kj);
  auto msg2 = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg2.is<kj::String>() && msg2.get<kj::String>() == "after-hib"_kj);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: in-flight DO close survives hibernation within one IoContext") {
  // The DO calls close() while the eyeball isn't reading; the pump queues the Close into
  // outgoingMessages and blocks on a BlockedSend on the pipe. Hibernation runs. Verify that
  // when the eyeball reads, it gets the Close.
  //
  // Mechanism: the OLD api::WebSocket is dropped from activeOrPackage during hibernation, but
  // its pump task lives on (held alive via JSG_THIS in the pump's continuation, which is in
  // the IoContext's tasks/waitUntilTasks list). The old pump's blocked ws.close(...) is still
  // waiting on the pipe; once the eyeball reads, it delivers the Close. The Close is NOT
  // dropped within a single IoContext — IoContext destruction is what loses it.
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("close-race-same-ioc")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto websockets = hm->getWebSockets(js, kj::none);
    websockets[0]->close(js, 1001, jsg::USVString(kj::str("queued-close")));
  });
  fixture.pollEventLoop();  // pump blocks on the close BlockedSend

  // Hibernate while the close is mid-send. activeOrPackage transitions; but we leave the
  // IoContext alive (don't drainAndDestroy yet), so the OLD pump task keeps running.
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });

  // Eyeball reads — should receive the Close that was queued before hibernation.
  auto msg = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg.is<kj::WebSocket::Close>(), "expected Close");
  auto& close = msg.get<kj::WebSocket::Close>();
  KJ_ASSERT(close.code == 1001, close.code);
  KJ_ASSERT(close.reason == "queued-close"_kj, close.reason);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: in-flight auto-response orphans BlockedSend during hibernation") {
  // Regression test for EW-10817. sendAutoResponse creates a BlockedSend on the pipe (held in
  // a plain kj::Own outside any IoOwn — see web-socket.c++:874), then hibernation replaces
  // activeOrPackage without carrying that state. The new api::WebSocket's pump skips the wait
  // and trips on the orphaned BlockedSend.
  //
  // The KJ_EXPECT_LOG below captures the bug's symptom (the assertion's ERROR log) so the
  // test passes while EW-10817 is open. When the bug is fixed, the log won't fire and the
  // KJ_EXPECT_LOG will fail — that's the signal to update this test (delete the EXPECT_LOG,
  // delete the workaround end1->receive() drain at the end, and add positive assertions about
  // what should happen instead).
  KJ_EXPECT_LOG(ERROR, "another message send is already in progress");

  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("ew-10817-autoresp")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  // Send ping → readLoop → sendAutoResponse → BlockedSend.
  end1->send("ping"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();

  // Hibernate.
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });

  // Unhibernate + close → hits orphaned BlockedSend.
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto websockets = hm->getWebSockets(js, kj::none);
    KJ_ASSERT(websockets.size() == 1);
    websockets[0]->close(js, 1001, jsg::USVString(kj::str("stale")));
  });

  fixture.pollEventLoop();

  // Drain end1 to consume the orphaned pong (held outside any IoOwn — the very thing this
  // test is documenting). Without this, the kj event loop fails its "no events left"
  // assertion at teardown. Once EW-10817 is fixed, the orphan won't exist; this drain becomes
  // unnecessary and the test will need updating.
  end1->receive().wait(fixture.getWaitScope());
}

KJ_TEST("HibernationManager: in-flight DO send orphans BlockedSend during hibernation") {
  // Regression test for EW-10817. Same shape as the auto-response variant above but driven by
  // a DO-side ws.send() — the pump creates a BlockedSend on the pipe (no BPT yet), hibernation
  // orphans it, the next operation on the new api::WebSocket trips the assertion. See the
  // auto-response variant above for the EXPECT_LOG / lifecycle details.
  //
  // The pump task is in waitUntilTasks (actors route addTask there) and it's stuck on the
  // BlockedSend; the IR's destructor at the end of the test logs a "failed to invoke drain()"
  // warning that we capture here so the test output stays clean.
  KJ_EXPECT_LOG(ERROR, "another message send is already in progress");
  KJ_EXPECT_LOG(WARNING, "failed to invoke drain() on IncomingRequest before destroying it");

  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("ew-10817-dosend")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(fixture, *request, *hm);

  sendFromDo(fixture, *request, *hm, "hello from DO"_kj);

  fixture.pollEventLoop();

  // Hibernate.
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });

  // Unhibernate + close → hits orphaned BlockedSend.
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto websockets = hm->getWebSockets(js, kj::none);
    KJ_ASSERT(websockets.size() == 1);
    websockets[0]->close(js, 1001, jsg::USVString(kj::str("stale")));
  });

  fixture.pollEventLoop();
}

// ---------- Cross-IoContext hibernation flows (with actor eviction) ----------

KJ_TEST("HibernationManager: comm survives hibernation + actor eviction (cross-IoContext)") {
  // Production-style hibernation: the actor is fully evicted and a new one is created on
  // revival. The HM outlives any actor instance (in production, the namespace pulls the HM
  // off the dying actor; in this test, the test holds it directly). After eviction, a brand
  // new IoContext is built against the new actor, and the HM revives the WebSocket into it.
  //
  // This test exercises the no-in-flight-state cross-IoContext path: it round-trips a message
  // before hibernating, so there's no pending BlockedSend on the pipe to orphan. It passes
  // both before and after EW-10817 is fixed; its job is to ensure the unified-queue refactor
  // doesn't break the basic eviction-and-revive flow. The actual bug-firing cross-IoContext
  // case is the auto-response variant below.
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("hibernate-evict")));
  auto hm = makeTestHm(fixture);

  // Phase 1: accept WS under the original actor's IoContext, round-trip a message.
  auto request1 = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request1, *hm);
  sendFromDo(fixture, *request1, *hm, "pre-evict"_kj);
  auto msg1 = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg1.is<kj::String>() && msg1.get<kj::String>() == "pre-evict"_kj);

  // Hibernate, drain the IR, evict the actor.
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });
  fixture.drainAndDestroy(kj::mv(request1));
  fixture.resetActor();

  // Phase 2: a brand new actor + IoContext. The HM (held by the test) is unchanged.
  auto request2 = fixture.newIncomingRequest();
  sendFromDo(fixture, *request2, *hm, "post-evict"_kj);
  auto msg2 = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg2.is<kj::String>() && msg2.get<kj::String>() == "post-evict"_kj);

  fixture.drainAndDestroy(kj::mv(request2));
}

KJ_TEST("HibernationManager: in-flight DO close lost across IoContext destruction") {
  // Cross-IoContext variant of the in-flight-close test above: same setup, but we drop the
  // IR (destroying the IoContext) before the eyeball reads. The IoContext destruction
  // cancels the pump task, which cancels the in-flight ws.close(), cleaning up the
  // BlockedSend. The Close is silently lost — the eyeball never sees a clean WebSocket close.
  //
  // This is the close-race version of the silent-message-drop bug. WebSockets are supposed
  // to be reliable; losing close frames is its own bug class. The unified-queue refactor's
  // design (queue lives on the adapter, persists across IoContexts) addresses this
  // incidentally — the close stays queued until actually delivered.
  //
  // The IR's destructor below logs a "failed to invoke drain()" warning because we drop it
  // without draining (the pump task is stuck in waitUntilTasks; drain() would hang). Capture
  // the warning so the test passes cleanly.
  KJ_EXPECT_LOG(WARNING, "failed to invoke drain() on IncomingRequest before destroying it");

  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("close-race-cross-ioc")));
  auto hm = makeTestHm(fixture);
  auto request1 = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request1, *hm);

  fixture.enterContext(*request1, [&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto websockets = hm->getWebSockets(js, kj::none);
    websockets[0]->close(js, 1001, jsg::USVString(kj::str("doomed-close")));
  });
  fixture.pollEventLoop();

  // Hibernate, then drop the IR (destroying the IoContext). The pump's in-flight ws.close()
  // is canceled.
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });
  request1 = nullptr;
  fixture.resetActor();

  // The eyeball's receive promise should NOT resolve to a Close — the close was canceled
  // mid-send. Verify by polling: receive should not be ready immediately. (We can't easily
  // assert "never resolves" in a test, so we observe the not-yet-ready state and move on.)
  auto receivePromise = end1->receive();
  fixture.pollEventLoop();
  KJ_ASSERT(!receivePromise.poll(fixture.getWaitScope()),
      "close was silently dropped across IoContext destruction; eyeball receives nothing");

  // The new api::WebSocket has closedOutgoing=true (from the package), so the DO can't
  // re-issue the close even if it wanted to. The eyeball is stuck.
}

KJ_TEST("HibernationManager: in-flight auto-response orphans BlockedSend across actor eviction") {
  // Regression test for EW-10817 — the production failure mode. sendAutoResponse runs from
  // the HM's readLoop (on the HM's TaskSet, NOT in an IoContext). It does a direct
  // kj::WebSocket::send that creates a BlockedSend on the pipe. IoContext destruction cancels
  // pump tasks but not sendAutoResponse, so the BlockedSend survives the IoContext's death.
  // After actor eviction and revival, the new api::WebSocket's pump trips on the orphan. See
  // the same-IoContext auto-response variant above for the EXPECT_LOG / lifecycle details.
  KJ_EXPECT_LOG(ERROR, "another message send is already in progress");

  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("ew-10817-cross-autoresp")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);

  auto request1 = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(fixture, *request1, *hm);

  // Eyeball sends ping → HM readLoop → sendAutoResponse → BlockedSend on the pipe.
  end1->send("ping"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();

  // Hibernate, drop the IR (IoContext1 is destroyed; the BlockedSend survives because
  // sendAutoResponse runs outside any IoContext). Then evict the actor.
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });
  request1 = nullptr;
  fixture.resetActor();

  // Phase 3: under a brand-new actor + IoContext, do something that starts a fresh pump.
  // The new pump trips on the orphaned BlockedSend.
  auto request2 = fixture.newIncomingRequest();
  fixture.enterContext(*request2, [&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto websockets = hm->getWebSockets(js, kj::none);
    KJ_ASSERT(websockets.size() == 1);
    websockets[0]->close(js, 1001, jsg::USVString(kj::str("post-evict")));
  });
  fixture.pollEventLoop();

  // Drain end1 to consume the orphaned pong (see same-IoContext variant above for why).
  end1->receive().wait(fixture.getWaitScope());
}

KJ_TEST("HibernationManager: DO send waits for the actor's output gate") {
  // The pump calls IoContext::waitForOutputLocksIfNecessary() before each kj::WebSocket::send.
  // Locking the actor's OutputGate should hold a DO-side message until the gate releases.
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("output-gate-do-send")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  // Lock the output gate. `blocker` is the wrapped promise; keep it in scope until we've
  // either fulfilled the underlying promise or are otherwise done.
  auto paf = kj::newPromiseAndFulfiller<void>();
  auto blocker = fixture.getActor().getOutputGate().lockWhile(kj::mv(paf.promise), nullptr);

  // DO sends a message. The pump should block on the gate.
  sendFromDo(fixture, *request, *hm, "gated"_kj);

  // Set up the eyeball's receive promise without waiting.
  auto receivePromise = end1->receive();

  // Drive the loop; receivePromise should NOT be ready (gate still locked).
  fixture.pollEventLoop();
  KJ_ASSERT(!receivePromise.poll(fixture.getWaitScope()),
      "message should not have arrived while output gate is locked");

  // Release the gate. The pump should now flush the message.
  paf.fulfiller->fulfill();
  auto msg = receivePromise.wait(fixture.getWaitScope());
  KJ_ASSERT(msg.is<kj::String>() && msg.get<kj::String>() == "gated"_kj);

  // blocker must outlive the gate-locking promise; let it die naturally at end of scope.
  blocker.wait(fixture.getWaitScope());
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: DO close waits for the actor's output gate") {
  // Like the DO-send-waits-for-gate test, but for close. close() goes through the same pump
  // (it inserts a Close GatedMessage into outgoingMessages with the current output lock), so
  // it must wait for the gate to release before the close frame reaches the eyeball.
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("output-gate-do-close")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  auto paf = kj::newPromiseAndFulfiller<void>();
  auto blocker = fixture.getActor().getOutputGate().lockWhile(kj::mv(paf.promise), nullptr);

  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto websockets = hm->getWebSockets(js, kj::none);
    KJ_ASSERT(websockets.size() == 1);
    websockets[0]->close(js, 1001, jsg::USVString(kj::str("gated-bye")));
  });

  auto receivePromise = end1->receive();
  fixture.pollEventLoop();
  KJ_ASSERT(!receivePromise.poll(fixture.getWaitScope()),
      "close should not have arrived while output gate is locked");

  paf.fulfiller->fulfill();
  auto msg = receivePromise.wait(fixture.getWaitScope());
  KJ_ASSERT(msg.is<kj::WebSocket::Close>());
  auto& close = msg.get<kj::WebSocket::Close>();
  KJ_ASSERT(close.code == 1001, close.code);
  KJ_ASSERT(close.reason == "gated-bye"_kj, close.reason);

  blocker.wait(fixture.getWaitScope());
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: auto-response (active) waits when pump is gate-blocked on a DO send") {
  // When the pump is already running (isPumping == true) and stalled on the output gate for a
  // queued DO message, an arriving auto-response request causes sendAutoResponse to push the
  // pong onto pendingAutoResponseDeque. The pump only drains that deque after it finishes the
  // outer outgoingMessages loop, so the pong waits for the gate to release transitively.
  //
  // Order at the eyeball: the gated DO message arrives first (after the gate releases), and
  // the pong follows immediately after (line 998 in web-socket.c++).
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("output-gate-autoresp-gated")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  auto paf = kj::newPromiseAndFulfiller<void>();
  auto blocker = fixture.getActor().getOutputGate().lockWhile(kj::mv(paf.promise), nullptr);

  // DO sends "msg1" — pump starts, blocks on gate.
  sendFromDo(fixture, *request, *hm, "msg1"_kj);

  // Eyeball sends ping. sendAutoResponse sees isPumping=true and queues "pong".
  end1->send("ping"_kj).wait(fixture.getWaitScope());

  // Neither msg1 nor pong has arrived yet.
  auto receivePromise = end1->receive();
  fixture.pollEventLoop();
  KJ_ASSERT(!receivePromise.poll(fixture.getWaitScope()),
      "msg1 should not have arrived while output gate is locked");

  // Release the gate. msg1 flushes, then pong follows.
  paf.fulfiller->fulfill();
  auto msg1 = receivePromise.wait(fixture.getWaitScope());
  KJ_ASSERT(msg1.is<kj::String>() && msg1.get<kj::String>() == "msg1"_kj);
  auto msg2 = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg2.is<kj::String>() && msg2.get<kj::String>() == "pong"_kj);

  blocker.wait(fixture.getWaitScope());
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: auto-response (active) bypasses the output gate") {
  // Documents CURRENT behavior: in active mode, sendAutoResponse uses a direct kj::WebSocket::send
  // that doesn't go through the pump, and therefore doesn't check waitForOutputLocksIfNecessary.
  // The unified-queue refactor planned for EW-10817 should change this so auto-response respects
  // the output gate in active mode; flip this assertion when that lands.
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("output-gate-autoresp-active")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  auto paf = kj::newPromiseAndFulfiller<void>();
  auto blocker = fixture.getActor().getOutputGate().lockWhile(kj::mv(paf.promise), nullptr);

  // Eyeball sends ping; auto-response should send pong despite the gate being locked.
  end1->send("ping"_kj).wait(fixture.getWaitScope());
  auto msg = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg.is<kj::String>() && msg.get<kj::String>() == "pong"_kj);

  paf.fulfiller->fulfill();
  blocker.wait(fixture.getWaitScope());
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: auto-response (hibernated) bypasses the output gate") {
  // Documents CURRENT behavior. The hibernated-mode readLoop sends the pong directly on the
  // kj::WebSocket without an IoContext, so the actor's output gate never enters the picture
  // (and couldn't be checked anyway, since waitForOutputLocksIfNecessary needs an IoContext).
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("output-gate-autoresp-hib")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });

  auto paf = kj::newPromiseAndFulfiller<void>();
  auto blocker = fixture.getActor().getOutputGate().lockWhile(kj::mv(paf.promise), nullptr);

  end1->send("ping"_kj).wait(fixture.getWaitScope());
  auto msg = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg.is<kj::String>() && msg.get<kj::String>() == "pong"_kj);

  paf.fulfiller->fulfill();
  blocker.wait(fixture.getWaitScope());
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: binary auto-response matches binary frame (active)") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("bin-autoresp-active")));
  auto req = kj::heapArray<kj::byte>({0x01, 0x02, 0x03});
  auto resp = kj::heapArray<kj::byte>({0x04, 0x05, 0x06});
  auto hm = makeTestHm(fixture, req.asPtr(), resp.asPtr());
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  end1->send(req.asPtr()).wait(fixture.getWaitScope());

  auto msg = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg.is<kj::Array<kj::byte>>());
  auto& got = msg.get<kj::Array<kj::byte>>();
  KJ_ASSERT(got.size() == 3);
  KJ_ASSERT(got[0] == 0x04 && got[1] == 0x05 && got[2] == 0x06);

  fixture.pollEventLoop();
  KJ_ASSERT(stats.customEventCalls == 0, "binary auto-response should not dispatch to worker",
      stats.customEventCalls);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: binary auto-response matches binary frame (hibernated)") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("bin-autoresp-hib")));
  auto req = kj::heapArray<kj::byte>({0x01, 0x02, 0x03});
  auto resp = kj::heapArray<kj::byte>({0x04, 0x05, 0x06});
  auto hm = makeTestHm(fixture, req.asPtr(), resp.asPtr());
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });

  end1->send(req.asPtr()).wait(fixture.getWaitScope());
  auto msg = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg.is<kj::Array<kj::byte>>());
  auto& got = msg.get<kj::Array<kj::byte>>();
  KJ_ASSERT(got.size() == 3);
  KJ_ASSERT(got[0] == 0x04 && got[1] == 0x05 && got[2] == 0x06);

  fixture.pollEventLoop();
  KJ_ASSERT(stats.customEventCalls == 0, "binary auto-response should not dispatch",
      stats.customEventCalls);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: binary auto-response does NOT match text frame") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("bin-autoresp-no-text")));
  auto req = kj::heapArray<kj::byte>({0x70, 0x69, 0x6e, 0x67});   // "ping" as bytes
  auto resp = kj::heapArray<kj::byte>({0x70, 0x6f, 0x6e, 0x67});  // "pong" as bytes
  auto hm = makeTestHm(fixture, req.asPtr(), resp.asPtr());
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  // Send "ping" as a TEXT frame — should NOT match the binary auto-response.
  end1->send("ping"_kj).wait(fixture.getWaitScope());

  fixture.pollEventLoop();
  KJ_ASSERT(stats.customEventCalls >= 1,
      "text frame should be dispatched despite matching binary content", stats.customEventCalls);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: text auto-response does NOT match binary frame") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("text-autoresp-no-bin")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  // Send "ping" as a BINARY frame — should NOT match the text auto-response.
  auto pingBytes = kj::heapArray<kj::byte>({0x70, 0x69, 0x6e, 0x67});
  end1->send(pingBytes.asPtr()).wait(fixture.getWaitScope());

  fixture.pollEventLoop();
  KJ_ASSERT(stats.customEventCalls >= 1,
      "binary frame should be dispatched despite matching text content", stats.customEventCalls);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: binary auto-response interleaved with DO sends (active)") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("bin-autoresp-interleaved")));
  auto req = kj::heapArray<kj::byte>({0x01, 0x02});
  auto resp = kj::heapArray<kj::byte>({0x03, 0x04});
  auto hm = makeTestHm(fixture, req.asPtr(), resp.asPtr());
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  sendFromDo(fixture, *request, *hm, "before"_kj);
  end1->send(req.asPtr()).wait(fixture.getWaitScope());
  sendFromDo(fixture, *request, *hm, "after"_kj);

  bool sawBefore = false, sawPong = false, sawAfter = false;
  for (int i = 0; i < 3; ++i) {
    auto msg = end1->receive().wait(fixture.getWaitScope());
    KJ_SWITCH_ONEOF(msg) {
      KJ_CASE_ONEOF(text, kj::String) {
        if (text == "before"_kj)
          sawBefore = true;
        else if (text == "after"_kj)
          sawAfter = true;
        else
          KJ_FAIL_ASSERT("unexpected text message", text);
      }
      KJ_CASE_ONEOF(data, kj::Array<kj::byte>) {
        KJ_ASSERT(data.size() == 2 && data[0] == 0x03 && data[1] == 0x04);
        sawPong = true;
      }
      KJ_CASE_ONEOF_DEFAULT {
        KJ_FAIL_ASSERT("unexpected message type");
      }
    }
  }
  KJ_ASSERT(sawBefore && sawPong && sawAfter);

  KJ_ASSERT(stats.customEventCalls == 0, "auto-response should not dispatch to worker",
      stats.customEventCalls);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: getWebSocketAutoResponse round-trips binary data") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("bin-autoresp-roundtrip")));
  auto req = kj::heapArray<kj::byte>({0xde, 0xad});
  auto resp = kj::heapArray<kj::byte>({0xbe, 0xef});
  auto hm = makeTestHm(fixture, req.asPtr(), resp.asPtr());
  auto request = fixture.newIncomingRequest();

  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto maybePair = hm->getWebSocketAutoResponse(env.js);
    auto pair = KJ_ASSERT_NONNULL(kj::mv(maybePair));
    auto gotReq = pair->getRequest();
    auto gotResp = pair->getResponse();

    KJ_ASSERT(gotReq.is<kj::ArrayPtr<const kj::byte>>());
    auto& reqBytes = gotReq.get<kj::ArrayPtr<const kj::byte>>();
    KJ_ASSERT(reqBytes.size() == 2 && reqBytes[0] == 0xde && reqBytes[1] == 0xad);

    KJ_ASSERT(gotResp.is<kj::ArrayPtr<const kj::byte>>());
    auto& respBytes = gotResp.get<kj::ArrayPtr<const kj::byte>>();
    KJ_ASSERT(respBytes.size() == 2 && respBytes[0] == 0xbe && respBytes[1] == 0xef);
  });

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: getWebSocketAutoResponse round-trips text data") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("text-autoresp-roundtrip")));
  auto hm = makeTestHm(fixture, "hello"_kj, "world"_kj);
  auto request = fixture.newIncomingRequest();

  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto maybePair = hm->getWebSocketAutoResponse(env.js);
    auto pair = KJ_ASSERT_NONNULL(kj::mv(maybePair));
    auto gotReq = pair->getRequest();
    auto gotResp = pair->getResponse();

    KJ_ASSERT(gotReq.is<kj::StringPtr>());
    KJ_ASSERT(gotReq.get<kj::StringPtr>() == "hello"_kj);

    KJ_ASSERT(gotResp.is<kj::StringPtr>());
    KJ_ASSERT(gotResp.get<kj::StringPtr>() == "world"_kj);
  });

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: clear auto-response with kj::none after binary set") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("bin-autoresp-clear")));
  auto req = kj::heapArray<kj::byte>({0x01, 0x02});
  auto resp = kj::heapArray<kj::byte>({0x03, 0x04});
  auto hm = makeTestHm(fixture, req.asPtr(), resp.asPtr());
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  hm->setWebSocketAutoResponse(kj::none, kj::none);

  end1->send(req.asPtr()).wait(fixture.getWaitScope());

  fixture.pollEventLoop();
  KJ_ASSERT(stats.customEventCalls >= 1, "after clearing, binary frame should be dispatched",
      stats.customEventCalls);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST(
    "HibernationManager: in-flight binary auto-response orphans BlockedSend during hibernation") {
  KJ_EXPECT_LOG(ERROR, "another message send is already in progress");

  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("ew-10817-bin-autoresp")));
  auto req = kj::heapArray<kj::byte>({0x01, 0x02});
  auto resp = kj::heapArray<kj::byte>({0x03, 0x04});
  auto hm = makeTestHm(fixture, req.asPtr(), resp.asPtr());
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  end1->send(req.asPtr()).wait(fixture.getWaitScope());
  fixture.pollEventLoop();

  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });

  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto websockets = hm->getWebSockets(js, kj::none);
    KJ_ASSERT(websockets.size() == 1);
    websockets[0]->close(js, 1001, jsg::USVString(kj::str("stale")));
  });

  fixture.pollEventLoop();
  end1->receive().wait(fixture.getWaitScope());
}

KJ_TEST(
    "HibernationManager: in-flight binary auto-response orphans BlockedSend across actor eviction") {
  KJ_EXPECT_LOG(ERROR, "another message send is already in progress");

  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("ew-10817-cross-bin-autoresp")));
  auto req = kj::heapArray<kj::byte>({0x01, 0x02});
  auto resp = kj::heapArray<kj::byte>({0x03, 0x04});
  auto hm = makeTestHm(fixture, req.asPtr(), resp.asPtr());

  auto request1 = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(fixture, *request1, *hm);

  end1->send(req.asPtr()).wait(fixture.getWaitScope());
  fixture.pollEventLoop();

  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });
  request1 = nullptr;
  fixture.resetActor();

  auto request2 = fixture.newIncomingRequest();
  fixture.enterContext(*request2, [&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto websockets = hm->getWebSockets(js, kj::none);
    KJ_ASSERT(websockets.size() == 1);
    websockets[0]->close(js, 1001, jsg::USVString(kj::str("post-evict")));
  });
  fixture.pollEventLoop();

  end1->receive().wait(fixture.getWaitScope());
}

}  // namespace
}  // namespace workerd
