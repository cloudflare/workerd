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

#include <workerd/api/hibernatable-web-socket.h>
#include <workerd/api/web-socket.h>
#include <workerd/io/frankenvalue.h>
#include <workerd/io/legacy-hibernation-manager.h>
#include <workerd/io/worker-interface.h>
#include <workerd/io/worker.h>
#include <workerd/tests/test-fixture.h>

#include <kj/compat/http.h>
#include <kj/test.h>

#if KJ_HAS_COMPILER_FEATURE(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
#include <sanitizer/lsan_interface.h>
#endif

namespace workerd {

struct LegacyHibernationManagerTestAccess {
  static void registerOnlyWebSocketForEvent(
      LegacyHibernationManagerImpl& manager, kj::String websocketId) {
    manager.registerEventWebSocket(kj::mv(websocketId), getOnlyWebSocket(manager));
  }

  static void registerWebSocketForEvent(
      LegacyHibernationManagerImpl& manager, kj::String websocketId, size_t acceptIndex) {
    manager.registerEventWebSocket(
        kj::mv(websocketId), getWebSocketByAcceptOrder(manager, acceptIndex));
  }

  static bool findManagerForEventMatches(
      kj::StringPtr websocketId, LegacyHibernationManagerImpl& expectedManager) {
    KJ_IF_SOME(manager, LegacyHibernationManagerImpl::findManagerForEvent(websocketId)) {
      return &kj::downcast<LegacyHibernationManagerImpl>(manager) == &expectedManager;
    }
    return false;
  }

  static void cancelEvent(LegacyHibernationManagerImpl& manager, kj::StringPtr websocketId) {
    manager.cancelEvent(websocketId);
  }

  static kj::Own<WorkerInterface> getWorkerForOnlyWebSocket(LegacyHibernationManagerImpl& manager) {
    return manager.getWorkerForEvent(getOnlyWebSocket(manager));
  }

  static size_t registeredEventCount(LegacyHibernationManagerImpl& manager) {
    return manager.registeredEventCount;
  }

  static size_t loopbackWaiterCount(LegacyHibernationManagerImpl& manager) {
    return manager.loopbackWaiters.size();
  }

  static bool takeWebSocketForEventMatches(
      kj::StringPtr websocketId, LegacyHibernationManagerImpl& expectedManager) {
    auto& webSocket = LegacyHibernationManagerImpl::takeWebSocketForEvent(websocketId);
    return &webSocket == &getOnlyWebSocket(expectedManager);
  }

  static bool takeWebSocketForEventMatches(kj::StringPtr websocketId,
      LegacyHibernationManagerImpl& expectedManager,
      size_t acceptIndex) {
    auto& webSocket = LegacyHibernationManagerImpl::takeWebSocketForEvent(websocketId);
    return &webSocket == &getWebSocketByAcceptOrder(expectedManager, acceptIndex);
  }

  // Whether the event is registered at all, without naming a manager. Lets a test outlive the
  // manager it registered and still observe the registry.
  static bool hasEventRegistration(kj::StringPtr websocketId) {
    return LegacyHibernationManagerImpl::findManagerForEvent(websocketId) != kj::none;
  }

 private:
  static LegacyHibernationManagerImpl::HibernatableWebSocket& getOnlyWebSocket(
      LegacyHibernationManagerImpl& manager) {
    KJ_ASSERT(manager.allWs.size() == 1);
    return getWebSocketByAcceptOrder(manager, 0);
  }

  // `acceptIndex` counts in the order the WebSockets were accepted. `allWs` is push_front-ordered,
  // so the first one accepted sits at the back.
  static LegacyHibernationManagerImpl::HibernatableWebSocket& getWebSocketByAcceptOrder(
      LegacyHibernationManagerImpl& manager, size_t acceptIndex) {
    KJ_ASSERT(acceptIndex < manager.allWs.size());
    auto it = manager.allWs.rbegin();
    for (size_t i = 0; i < acceptIndex; ++i) ++it;
    return **it;
  }
};

namespace api {
struct HibernatableWebSocketCustomEventTestAccess {
  static void ensureHibernationManagerForEvent(
      HibernatableWebSocketCustomEvent& event, Worker::Actor& actor, kj::StringPtr websocketId) {
    event.ensureHibernationManagerForEvent(actor, websocketId);
  }
};
}  // namespace api

namespace {

// ============================================================================
// Test fixtures
// ============================================================================

// Counts callbacks observed by StubLoopback / StubWorkerInterface so tests can
// assert dispatch behavior (e.g., auto-response should NOT dispatch).
struct DispatchStats {
  uint getWorkerCalls = 0;
  uint customEventCalls = 0;
  bool rejectCustomEvents = false;

  // When set, customEvent() holds the event and never settles, standing in for a peer that stops
  // responding mid-dispatch.
  bool hangCustomEvents = false;

  // What the most recent getWorker() call asked for. Null until the first call.
  kj::Maybe<ReresolveActorPipeline> lastReresolveActorPipeline;
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
    if (stats.hangCustomEvents) {
      return kj::Promise<WorkerInterface::CustomEvent::Result>(kj::NEVER_DONE)
          .attach(kj::mv(event));
    }
    if (stats.rejectCustomEvents) {
      return kj::Promise<WorkerInterface::CustomEvent::Result>(KJ_EXCEPTION(
          OVERLOADED, "jsg.Error: Durable Object is overloaded. Too many requests queued."));
    }
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

  kj::Own<WorkerInterface> getWorker(IoChannelFactory::SubrequestMetadata metadata) override {
    ++stats.getWorkerCalls;
    stats.lastReresolveActorPipeline = metadata.reresolveActorPipeline;
    return kj::heap<StubWorkerInterface>(stats);
  }

  kj::Own<Worker::Actor::Loopback> addRef() override {
    return kj::addRef(*this);
  }

 private:
  DispatchStats& stats;
};

class ControlledHibernatableEventDispatcher final
    : public rpc::HibernatableWebSocketEventDispatcher::Server {
 public:
  ControlledHibernatableEventDispatcher(
      kj::Promise<void> completion, kj::String& receivedWebsocketId, bool& called)
      : completion(kj::mv(completion)),
        receivedWebsocketId(receivedWebsocketId),
        called(called) {}

  kj::Promise<void> hibernatableWebSocketEvent(HibernatableWebSocketEventContext context) override {
    called = true;
    receivedWebsocketId = kj::str(context.getParams().getMessage().getWebsocketId());
    return kj::mv(completion);
  }

 private:
  kj::Promise<void> completion;
  kj::String& receivedWebsocketId;
  bool& called;
};

// Helpers below are intentionally split so the HibernationManager can outlive any single
// IncomingRequest, which matters for tests that span multiple IRs. makeTestHm() needs no
// IoContext; acceptNewWebSocket() and sendFromDo() do (the api::WebSocket constructor stores
// IoOwn members, and ws.send() is delivered through the IoContext's pump).

// A TimerChannel whose afterLimitTimeout() fires only when the test asks it to, so a test can reach
// a timeout measured in seconds without spending them. Install with setTimerChannel().
struct ManualTimerChannel final: public TimerChannel {
  void syncTime() override {}

  kj::Date now(kj::Maybe<kj::Date>) override {
    return kj::systemPreciseCalendarClock().now();
  }

  kj::Promise<void> atTime(kj::Date when) override {
    return kj::NEVER_DONE;
  }

  kj::Promise<void> afterLimitTimeout(kj::Duration t) override {
    auto paf = kj::newPromiseAndFulfiller<void>();
    fulfillers.add(kj::mv(paf.fulfiller));
    return kj::mv(paf.promise);
  }

  void fireAll() {
    for (auto& fulfiller: fulfillers) {
      fulfiller->fulfill();
    }
    fulfillers.clear();
  }

  kj::Vector<kj::Own<kj::PromiseFulfiller<void>>> fulfillers;
};

// SetupParams builder that installs a StubLoopback on the actor referencing `stats`. The
// caller MUST keep `stats` alive for the lifetime of the resulting TestFixture (declare it
// before the fixture). The same StubLoopback is later retrieved via actor.getLoopback() and
// handed to the HM, so actor and HM share a single Loopback (mirroring production).
TestFixture::SetupParams stubLoopbackParams(
    DispatchStats& stats, kj::String actorId, kj::Maybe<uint64_t> holderToken = kj::none) {
  return {
    .actorId = Worker::Actor::Id(kj::mv(actorId)),
    .useRealTimers = true,
    .actorLoopback = kj::refcounted<StubLoopback>(stats),
    .holderToken = holderToken,
  };
}

// Create a HibernationManager. The constructor (and setTimerChannel) don't need an IoContext;
// production typically constructs the HM inside one only because the trigger — a JS call to
// state.acceptWebSocket — runs in one. The HM itself is IoContext-independent and this test
// pattern keeps that explicit so any inadvertent dependency growth shows up.
kj::Own<Worker::Actor::HibernationManager> makeTestHm(TestFixture& fixture) {
  auto hm = kj::refcounted<LegacyHibernationManagerImpl>(fixture.getActor().getLoopback(), 0);
  hm->setTimerChannel(fixture.getTimerChannel());
  return hm;
}

// Same, but configure auto-response. Both `autoRequest` and `autoResponse` are required.
kj::Own<Worker::Actor::HibernationManager> makeTestHm(
    TestFixture& fixture, kj::StringPtr autoRequest, kj::StringPtr autoResponse) {
  auto hm = makeTestHm(fixture);
  hm->setWebSocketAutoResponse(autoRequest, autoResponse);
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

KJ_TEST("HibernationManager: event delivery resolves the manager that registered the event") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("event-routing-local")));
  auto hm = makeTestHm(fixture);
  auto& legacyHm = kj::downcast<LegacyHibernationManagerImpl>(*hm);
  auto request = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(fixture, *request, *hm);
  fixture.getActor().setHibernationManager(hm->addRef());

  constexpr kj::StringPtr websocketId = "local-event"_kj;
  LegacyHibernationManagerTestAccess::registerOnlyWebSocketForEvent(legacyHm, kj::str(websocketId));
  KJ_ASSERT(LegacyHibernationManagerTestAccess::findManagerForEventMatches(websocketId, legacyHm));

  fixture.enterContext(*request, [&](const TestFixture::Environment&) {
    KJ_ASSERT(
        LegacyHibernationManagerTestAccess::takeWebSocketForEventMatches(websocketId, legacyHm));
  });

  // Claiming deregisters the event, and every delivery path claims unconditionally.
  KJ_ASSERT(!LegacyHibernationManagerTestAccess::hasEventRegistration(websocketId));
  KJ_ASSERT(LegacyHibernationManagerTestAccess::registeredEventCount(legacyHm) == 0);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: a dispatch that never settles does not keep the manager alive") {
  DispatchStats stats;
  stats.hangCustomEvents = true;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("never-settling-dispatch")));
  auto request = fixture.newIncomingRequest();

  {
    auto hm = makeTestHm(fixture);
    auto end1 = acceptNewWebSocket(fixture, *request, *hm);

    // Start a dispatch and leave it pending. The manager owns the task waiting on it, so anything
    // the dispatch holds that leads back to the manager would make it immortal.
    end1->send("hello"_kj).wait(fixture.getWaitScope());
    fixture.pollEventLoop();
    KJ_ASSERT(stats.customEventCalls == 1, stats.customEventCalls);

    // `hm` is deliberately never handed to the actor, so it holds the manager's only reference, and
    // a dispatch that led back to the manager would show up here as a second one. The native
    // WebSocket outlives the manager while a dispatch is in flight, so whether the eyeball end
    // closes says nothing about the manager's lifetime.
    KJ_ASSERT(!hm->isShared(), "a pending dispatch holds a reference back to the manager");
  }

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: events wait for the replacement loopback during handoff") {
  DispatchStats oldStats;
  DispatchStats replacementStats;
  TestFixture fixture(stubLoopbackParams(oldStats, kj::str("loopback-handoff")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  auto handoff = hm->beginLoopbackHandoff();
  end1->send("during handoff"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();

  // The message found no loopback to dispatch through. Only `oldStats` is worth asserting on:
  // nothing references `replacementStats` until its StubLoopback exists, below.
  KJ_ASSERT(oldStats.getWorkerCalls == 0, oldStats.getWorkerCalls);

  hm->setLoopback(kj::refcounted<StubLoopback>(replacementStats));
  fixture.pollEventLoop();

  KJ_ASSERT(replacementStats.getWorkerCalls == 1, replacementStats.getWorkerCalls);
  KJ_ASSERT(replacementStats.customEventCalls == 1, replacementStats.customEventCalls);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: dropping the handoff handle restores the previous loopback") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("loopback-handoff-drop")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  {
    auto handoff = hm->beginLoopbackHandoff();
    end1->send("during handoff"_kj).wait(fixture.getWaitScope());
    fixture.pollEventLoop();

    // No replacement actor has attached, so the event has nothing to dispatch through.
    KJ_ASSERT(stats.getWorkerCalls == 0, stats.getWorkerCalls);
  }

  // Dropping the handle puts the outgoing generation's loopback back, so the event that was
  // waiting is served by the actor that supplied it.
  fixture.pollEventLoop();
  KJ_ASSERT(stats.getWorkerCalls == 1, stats.getWorkerCalls);
  KJ_ASSERT(stats.customEventCalls == 1, stats.customEventCalls);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: a stale handoff handle leaves a later handoff alone") {
  DispatchStats firstStats;
  DispatchStats secondStats;
  DispatchStats thirdStats;
  TestFixture fixture(stubLoopbackParams(firstStats, kj::str("loopback-handoff-stale")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  // One code update completes, so the handle it began no longer owns the handoff in progress.
  auto staleHandoff = hm->beginLoopbackHandoff();
  hm->setLoopback(kj::refcounted<StubLoopback>(secondStats));

  // A second code update parks the loopback the first one installed.
  auto liveHandoff = hm->beginLoopbackHandoff();
  { auto drop = kj::mv(staleHandoff); }

  end1->send("during second handoff"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();

  // Dropping the stale handle did not end the second handoff, so the event is still waiting rather
  // than dispatched through the loopback that handoff parked.
  KJ_ASSERT(secondStats.getWorkerCalls == 0, secondStats.getWorkerCalls);
  KJ_ASSERT(firstStats.getWorkerCalls == 0, firstStats.getWorkerCalls);

  hm->setLoopback(kj::refcounted<StubLoopback>(thirdStats));
  fixture.pollEventLoop();

  KJ_ASSERT(thirdStats.getWorkerCalls == 1, thirdStats.getWorkerCalls);
  KJ_ASSERT(thirdStats.customEventCalls == 1, thirdStats.customEventCalls);
  KJ_ASSERT(secondStats.getWorkerCalls == 0, secondStats.getWorkerCalls);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: waiting for a replacement loopback is bounded") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("loopback-handoff-timeout")));
  auto hm = makeTestHm(fixture);
  auto& legacyHm = kj::downcast<LegacyHibernationManagerImpl>(*hm);
  ManualTimerChannel manualTimer;
  hm->setTimerChannel(manualTimer);
  auto request = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(fixture, *request, *hm);

  // Once the loopback has been handed off, an event has nothing to dispatch through, so it is given
  // a WorkerInterface that waits for the replacement to arrive.
  auto handoff = hm->beginLoopbackHandoff();
  auto worker = LegacyHibernationManagerTestAccess::getWorkerForOnlyWebSocket(legacyHm);
  auto promise = worker->prewarm(""_kj);
  KJ_ASSERT(!promise.poll(fixture.getWaitScope()));
  KJ_ASSERT(LegacyHibernationManagerTestAccess::loopbackWaiterCount(legacyHm) == 1);

  // Nothing supplies a replacement loopback and nothing cancels the handoff. The wait must still
  // end: an unbounded one parks this event, and the read loop behind it, for the manager's
  // lifetime.
  manualTimer.fireAll();
  KJ_ASSERT(promise.poll(fixture.getWaitScope()));
  KJ_EXPECT_THROW_MESSAGE(
      "gave up waiting for the replacement actor's loopback", promise.wait(fixture.getWaitScope()));
  KJ_ASSERT(LegacyHibernationManagerTestAccess::loopbackWaiterCount(legacyHm) == 0);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: event naming another actor's manager is rejected") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("event-routing-other-manager")));
  auto actorHm = makeTestHm(fixture);
  auto eventHm = makeTestHm(fixture);
  auto& eventLegacyHm = kj::downcast<LegacyHibernationManagerImpl>(*eventHm);
  auto request = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(fixture, *request, *eventHm);

  // The event ID resolves to a manager, but not the one this actor is using. The ID is the only
  // part of an event not derived from the receiving actor, so this is the shape a misrouted event
  // takes.
  fixture.getActor().setHibernationManager(actorHm->addRef());

  constexpr kj::StringPtr websocketId = "other-manager-event"_kj;
  LegacyHibernationManagerTestAccess::registerOnlyWebSocketForEvent(
      eventLegacyHm, kj::str(websocketId));
  KJ_ASSERT(
      LegacyHibernationManagerTestAccess::findManagerForEventMatches(websocketId, eventLegacyHm));

  capnp::MallocMessageBuilder message;
  auto params =
      message
          .initRoot<rpc::HibernatableWebSocketEventDispatcher::HibernatableWebSocketEventParams>();
  auto eventMessage = params.initMessage();
  eventMessage.initPayload().setText("hello"_kj);
  eventMessage.setWebsocketId(websocketId);
  auto event = kj::refcounted<api::HibernatableWebSocketCustomEvent>(
      0, kj::heap<api::HibernationReader>(params.asReader()));

  auto exception = kj::runCatchingExceptions([&]() {
    api::HibernatableWebSocketCustomEventTestAccess::ensureHibernationManagerForEvent(
        *event, fixture.getActor(), websocketId);
  });
  auto& e = KJ_ASSERT_NONNULL(exception, "expected a foreign manager's event ID to be rejected");
  KJ_ASSERT(e.getDescription().endsWith(
                "hibernatable WebSocket event ID names a different hibernation manager than the "
                "receiving actor's"_kj),
      e);

  // The rejected event leaves the registration alone: it belongs to the manager that made it, which
  // is still holding the socket for whoever legitimately claims it.
  KJ_ASSERT(LegacyHibernationManagerTestAccess::hasEventRegistration(websocketId));
  KJ_ASSERT(LegacyHibernationManagerTestAccess::registeredEventCount(eventLegacyHm) == 1);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: claiming a socket owned by another live actor is rejected") {
  // Two actors sharing one event loop, and so one event registry, in which a claim resolves by ID
  // alone.
  DispatchStats ownerStats;
  TestFixture ownerFixture(stubLoopbackParams(ownerStats, kj::str("claim-owner")));

  DispatchStats otherStats;
  TestFixture otherFixture({
    .waitScope = ownerFixture.getWaitScope(),
    .actorId = Worker::Actor::Id(kj::str("claim-other")),
    .actorLoopback = kj::refcounted<StubLoopback>(otherStats),
  });

  auto hm = makeTestHm(ownerFixture);
  auto& legacyHm = kj::downcast<LegacyHibernationManagerImpl>(*hm);
  auto ownerRequest = ownerFixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(ownerFixture, *ownerRequest, *hm);
  ownerFixture.getActor().setHibernationManager(hm->addRef());

  constexpr kj::StringPtr websocketId = "cross-actor-claim"_kj;
  LegacyHibernationManagerTestAccess::registerOnlyWebSocketForEvent(legacyHm, kj::str(websocketId));

  // Nothing in an ID says whose socket it names, so the other actor can reach the registration.
  // Claiming it would hand this actor a jsg::Ref belonging to the owner's isolate.
  auto otherRequest = otherFixture.newIncomingRequest();
  kj::Maybe<kj::Exception> exception;
  otherFixture.enterContext(*otherRequest, [&](const TestFixture::Environment&) {
    exception = kj::runCatchingExceptions([&]() {
      LegacyHibernationManagerTestAccess::takeWebSocketForEventMatches(websocketId, legacyHm);
    });
  });
  auto& e = KJ_ASSERT_NONNULL(exception, "expected a cross-actor claim to be rejected");
  KJ_ASSERT(e.getDescription().endsWith(
                "hibernatable WebSocket event ID names a socket owned by a different actor"_kj),
      e);

  // The rejected claim leaves the registration for the actor that does own it.
  KJ_ASSERT(LegacyHibernationManagerTestAccess::hasEventRegistration(websocketId));

  otherFixture.drainAndDestroy(kj::mv(otherRequest));
  ownerFixture.drainAndDestroy(kj::mv(ownerRequest));
}

KJ_TEST("HibernationManager: claiming a socket owned by a live actor sharing its ID is rejected") {
  // Both actors carry the same ID, so the ID comparison admits the claim and only the instance
  // check stands between the claimant and a jsg::Ref minted in the owner's isolate.
  constexpr kj::StringPtr sharedId = "claim-shared-id"_kj;
  DispatchStats ownerStats;
  TestFixture ownerFixture(stubLoopbackParams(ownerStats, kj::str(sharedId)));

  DispatchStats otherStats;
  TestFixture otherFixture({
    .waitScope = ownerFixture.getWaitScope(),
    .actorId = Worker::Actor::Id(kj::str(sharedId)),
    .actorLoopback = kj::refcounted<StubLoopback>(otherStats),
  });

  auto hm = makeTestHm(ownerFixture);
  auto& legacyHm = kj::downcast<LegacyHibernationManagerImpl>(*hm);
  auto ownerRequest = ownerFixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(ownerFixture, *ownerRequest, *hm);
  ownerFixture.getActor().setHibernationManager(hm->addRef());

  constexpr kj::StringPtr websocketId = "same-id-claim"_kj;
  LegacyHibernationManagerTestAccess::registerOnlyWebSocketForEvent(legacyHm, kj::str(websocketId));

  auto otherRequest = otherFixture.newIncomingRequest();
  kj::Maybe<kj::Exception> exception;
  otherFixture.enterContext(*otherRequest, [&](const TestFixture::Environment&) {
    exception = kj::runCatchingExceptions([&]() {
      LegacyHibernationManagerTestAccess::takeWebSocketForEventMatches(websocketId, legacyHm);
    });
  });
  auto& e = KJ_ASSERT_NONNULL(exception, "expected a same-ID claim to be rejected");
  KJ_ASSERT(
      e.getDescription().endsWith(
          "hibernatable WebSocket event ID names a socket owned by a different live actor"_kj),
      e);

  // The rejected claim leaves the registration for the actor that does own it.
  KJ_ASSERT(LegacyHibernationManagerTestAccess::hasEventRegistration(websocketId));

  otherFixture.drainAndDestroy(kj::mv(otherRequest));
  ownerFixture.drainAndDestroy(kj::mv(ownerRequest));
}

KJ_TEST("HibernationManager: an actor handed a running actor's manager does not take it") {
  // Whatever holds a manager across generations does not necessarily hold one per actor, so a
  // manager can be supplied to an actor the sockets do not belong to: a facet constructed while
  // the actor that accepted them is still running, for instance.
  DispatchStats ownerStats;
  TestFixture ownerFixture(stubLoopbackParams(ownerStats, kj::str("adopt-owner")));
  auto hm = makeTestHm(ownerFixture);
  auto ownerRequest = ownerFixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(ownerFixture, *ownerRequest, *hm);
  ownerFixture.getActor().setHibernationManager(hm->addRef());

  DispatchStats otherStats;
  TestFixture otherFixture({
    .waitScope = ownerFixture.getWaitScope(),
    .actorId = Worker::Actor::Id(kj::str("adopt-other")),
    .actorLoopback = kj::refcounted<StubLoopback>(otherStats),
    .hibernationManager = hm->addRef(),
  });

  // The manager stayed with the actor it belongs to, and the other actor kept nothing.
  KJ_ASSERT(otherFixture.getActor().getHibernationManager() == kj::none);
  KJ_ASSERT(&KJ_ASSERT_NONNULL(hm->getOwningActor()) == &ownerFixture.getActor());

  // So a message on the socket still reaches the actor that accepted it.
  end1->send("still the owner's"_kj).wait(ownerFixture.getWaitScope());
  ownerFixture.pollEventLoop();
  KJ_ASSERT(ownerStats.getWorkerCalls == 1, ownerStats.getWorkerCalls);
  KJ_ASSERT(otherStats.getWorkerCalls == 0, otherStats.getWorkerCalls);

  ownerFixture.drainAndDestroy(kj::mv(ownerRequest));
}

KJ_TEST("HibernationManager: a manager-less actor adopts the event's manager") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("event-routing-adopt-manager")));
  auto eventHm = makeTestHm(fixture);
  auto& eventLegacyHm = kj::downcast<LegacyHibernationManagerImpl>(*eventHm);
  auto request = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(fixture, *request, *eventHm);

  // The actor a code-update wake creates has no manager of its own yet, and must take on the one
  // that has been holding its sockets rather than building a second one.
  KJ_ASSERT(fixture.getActor().getHibernationManager() == kj::none);

  constexpr kj::StringPtr websocketId = "adopt-manager-event"_kj;
  LegacyHibernationManagerTestAccess::registerOnlyWebSocketForEvent(
      eventLegacyHm, kj::str(websocketId));

  capnp::MallocMessageBuilder message;
  auto params =
      message
          .initRoot<rpc::HibernatableWebSocketEventDispatcher::HibernatableWebSocketEventParams>();
  auto eventMessage = params.initMessage();
  eventMessage.initPayload().setText("hello"_kj);
  eventMessage.setWebsocketId(websocketId);
  auto event = kj::refcounted<api::HibernatableWebSocketCustomEvent>(
      0, kj::heap<api::HibernationReader>(params.asReader()));

  api::HibernatableWebSocketCustomEventTestAccess::ensureHibernationManagerForEvent(
      *event, fixture.getActor(), websocketId);

  auto& adopted = KJ_ASSERT_NONNULL(fixture.getActor().getHibernationManager());
  KJ_ASSERT(&adopted == eventHm.get());

  // Having adopted it, the actor now passes the same check the delivery path applies, so the event
  // that follows can claim its socket.
  api::HibernatableWebSocketCustomEventTestAccess::ensureHibernationManagerForEvent(
      *event, fixture.getActor(), websocketId);
  fixture.enterContext(*request, [&](const TestFixture::Environment&) {
    KJ_ASSERT(LegacyHibernationManagerTestAccess::takeWebSocketForEventMatches(
        websocketId, eventLegacyHm));
  });
  KJ_ASSERT(!LegacyHibernationManagerTestAccess::hasEventRegistration(websocketId));

  // Adoption is what gives the manager an owner: afterwards it names this actor, and an event
  // reaching a different actor is rejected rather than handing it these sockets.
  auto& owner = KJ_ASSERT_NONNULL(eventHm->getOwningActor());
  KJ_ASSERT(&owner == &fixture.getActor());

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: a sibling holder sharing the actor ID does not take an orphan") {
  // Facets use their parent's actor ID by default, so an ID match does not mean the claimant is a
  // later actor of the holder the sockets belong to.
  DispatchStats ownerStats;
  TestFixture ownerFixture(stubLoopbackParams(ownerStats, kj::str("sibling-shared-id"), 1));
  auto hm = makeTestHm(ownerFixture);
  auto ownerRequest = ownerFixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(ownerFixture, *ownerRequest, *hm);
  ownerFixture.getActor().setHibernationManager(hm->addRef());

  // Orphan the manager, as hibernating the owning actor does.
  ownerFixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });
  ownerFixture.drainAndDestroy(kj::mv(ownerRequest));
  ownerFixture.resetActor();
  KJ_ASSERT(hm->getOwningActor() == kj::none);
  KJ_ASSERT(hm->getOwningActorId() != kj::none);

  DispatchStats siblingStats;
  TestFixture siblingFixture({
    .waitScope = ownerFixture.getWaitScope(),
    .actorId = Worker::Actor::Id(kj::str("sibling-shared-id")),
    .actorLoopback = kj::refcounted<StubLoopback>(siblingStats),
    .hibernationManager = hm->addRef(),
    .holderToken = 2,
  });

  // The IDs match, so only the differing holder token stands between the sibling and the sockets.
  KJ_ASSERT(siblingFixture.getActor().getHibernationManager() == kj::none);
  KJ_ASSERT(hm->getOwningActor() == kj::none);
  KJ_ASSERT(KJ_ASSERT_NONNULL(hm->getOwningHolderToken()) == 1);
}

KJ_TEST("HibernationManager: an event does not hand a sibling holder an orphan") {
  // An event resolves its manager from a registry shared by the whole event loop, so it reaches an
  // actor that was never offered the manager at construction. The holder token has to be checked
  // here too, and it is the last chance to check it: adopting stamps the manager with the adopting
  // actor, which is what every check downstream reads.
  DispatchStats ownerStats;
  TestFixture ownerFixture(stubLoopbackParams(ownerStats, kj::str("sibling-event-shared-id"), 1));
  auto hm = makeTestHm(ownerFixture);
  auto& legacyHm = kj::downcast<LegacyHibernationManagerImpl>(*hm);
  auto ownerRequest = ownerFixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(ownerFixture, *ownerRequest, *hm);
  ownerFixture.getActor().setHibernationManager(hm->addRef());

  // Orphan the manager, as hibernating the owning actor does.
  ownerFixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });
  ownerFixture.drainAndDestroy(kj::mv(ownerRequest));
  ownerFixture.resetActor();
  KJ_ASSERT(hm->getOwningActor() == kj::none);

  constexpr kj::StringPtr websocketId = "sibling-holder-event"_kj;
  LegacyHibernationManagerTestAccess::registerOnlyWebSocketForEvent(legacyHm, kj::str(websocketId));

  // A sibling facet, which shares the owner's ID and so passes every check the ID supports. It is
  // built without the manager, so it reaches the adopt path with nothing of its own to compare.
  DispatchStats siblingStats;
  TestFixture siblingFixture({
    .waitScope = ownerFixture.getWaitScope(),
    .actorId = Worker::Actor::Id(kj::str("sibling-event-shared-id")),
    .actorLoopback = kj::refcounted<StubLoopback>(siblingStats),
    .holderToken = 2,
  });
  KJ_ASSERT(siblingFixture.getActor().getHibernationManager() == kj::none);

  capnp::MallocMessageBuilder message;
  auto params =
      message
          .initRoot<rpc::HibernatableWebSocketEventDispatcher::HibernatableWebSocketEventParams>();
  auto eventMessage = params.initMessage();
  eventMessage.initPayload().setText("hello"_kj);
  eventMessage.setWebsocketId(websocketId);
  auto event = kj::refcounted<api::HibernatableWebSocketCustomEvent>(
      0, kj::heap<api::HibernationReader>(params.asReader()));

  auto exception = kj::runCatchingExceptions([&]() {
    api::HibernatableWebSocketCustomEventTestAccess::ensureHibernationManagerForEvent(
        *event, siblingFixture.getActor(), websocketId);
  });
  auto& e = KJ_ASSERT_NONNULL(exception, "expected a sibling holder's event to be rejected");
  KJ_ASSERT(e.getDescription().endsWith(
                "hibernatable WebSocket event names a hibernation manager owned by a different "
                "holder"_kj),
      e);

  // Refusing leaves the manager named by the holder it belongs to, so that holder's own next
  // generation still adopts it, and leaves the registration for that generation to claim.
  KJ_ASSERT(siblingFixture.getActor().getHibernationManager() == kj::none);
  KJ_ASSERT(KJ_ASSERT_NONNULL(hm->getOwningHolderToken()) == 1);
  KJ_ASSERT(LegacyHibernationManagerTestAccess::hasEventRegistration(websocketId));
}

KJ_TEST("HibernationManager: a manager that forgot its holder is adopted by another") {
  // Handing a manager to a replacement generation cannot name the holder that will adopt it, since
  // that holder does not exist yet. Forgetting the token leaves the ID as the identity to check.
  DispatchStats ownerStats;
  TestFixture ownerFixture(stubLoopbackParams(ownerStats, kj::str("forgotten-holder"), 1));
  auto hm = makeTestHm(ownerFixture);
  auto ownerRequest = ownerFixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(ownerFixture, *ownerRequest, *hm);
  ownerFixture.getActor().setHibernationManager(hm->addRef());

  // Orphan the manager, as hibernating the owning actor does.
  ownerFixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });
  ownerFixture.drainAndDestroy(kj::mv(ownerRequest));
  ownerFixture.resetActor();
  KJ_ASSERT(KJ_ASSERT_NONNULL(hm->getOwningHolderToken()) == 1);

  hm->forgetOwningHolder();
  KJ_ASSERT(hm->getOwningHolderToken() == kj::none);

  DispatchStats replacementStats;
  TestFixture replacementFixture({
    .waitScope = ownerFixture.getWaitScope(),
    .actorId = Worker::Actor::Id(kj::str("forgotten-holder")),
    .actorLoopback = kj::refcounted<StubLoopback>(replacementStats),
    .hibernationManager = hm->addRef(),
    .holderToken = 2,
  });

  // The token a sibling was refused for is gone, so this holder takes the sockets.
  auto& adopted = KJ_ASSERT_NONNULL(replacementFixture.getActor().getHibernationManager());
  KJ_ASSERT(&adopted == hm.get());

  // Adopting names the holder now running, so the next sibling is refused again.
  auto& newOwner = KJ_ASSERT_NONNULL(hm->getOwningActor());
  KJ_ASSERT(&newOwner == &replacementFixture.getActor());
  KJ_ASSERT(KJ_ASSERT_NONNULL(hm->getOwningHolderToken()) == 2);
}

KJ_TEST("HibernationManager: a new generation of the owning actor adopts its manager") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("adopt-same-actor-id")));
  auto hm = makeTestHm(fixture);
  auto& legacyHm = kj::downcast<LegacyHibernationManagerImpl>(*hm);
  auto request1 = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(fixture, *request1, *hm);
  fixture.getActor().setHibernationManager(hm->addRef());

  // Hibernate and replace the actor, as a code update does. The manager outlives the actor that
  // owned it, leaving the copied ID as the only identity it can still be checked against.
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });
  fixture.drainAndDestroy(kj::mv(request1));
  fixture.resetActor();
  KJ_ASSERT(hm->getOwningActor() == kj::none);
  KJ_ASSERT(hm->getOwningActorId() != kj::none);

  constexpr kj::StringPtr websocketId = "adopt-same-actor-id-event"_kj;
  LegacyHibernationManagerTestAccess::registerOnlyWebSocketForEvent(legacyHm, kj::str(websocketId));

  capnp::MallocMessageBuilder message;
  auto params =
      message
          .initRoot<rpc::HibernatableWebSocketEventDispatcher::HibernatableWebSocketEventParams>();
  auto eventMessage = params.initMessage();
  eventMessage.initPayload().setText("hello"_kj);
  eventMessage.setWebsocketId(websocketId);
  auto event = kj::refcounted<api::HibernatableWebSocketCustomEvent>(
      0, kj::heap<api::HibernationReader>(params.asReader()));

  // Same Durable Object, so this generation is entitled to the sockets the last one left behind.
  api::HibernatableWebSocketCustomEventTestAccess::ensureHibernationManagerForEvent(
      *event, fixture.getActor(), websocketId);

  auto& adopted = KJ_ASSERT_NONNULL(fixture.getActor().getHibernationManager());
  KJ_ASSERT(&adopted == hm.get());

  // Adopting re-points the manager at the generation now running.
  auto& newOwner = KJ_ASSERT_NONNULL(hm->getOwningActor());
  KJ_ASSERT(&newOwner == &fixture.getActor());
}

KJ_TEST("HibernationManager: a different Durable Object cannot adopt an orphaned manager") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("adopt-owner-id")));
  auto hm = makeTestHm(fixture);
  auto& legacyHm = kj::downcast<LegacyHibernationManagerImpl>(*hm);
  auto request1 = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(fixture, *request1, *hm);
  fixture.getActor().setHibernationManager(hm->addRef());

  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });
  fixture.drainAndDestroy(kj::mv(request1));

  // A different Durable Object, not a new generation of the same one. The owner is gone, so the
  // instance check has nothing to compare and the copied ID is all that stands in the way.
  fixture.resetActor(Worker::Actor::Id(kj::str("adopt-interloper-id")));
  KJ_ASSERT(hm->getOwningActor() == kj::none);

  constexpr kj::StringPtr websocketId = "adopt-different-actor-id-event"_kj;
  LegacyHibernationManagerTestAccess::registerOnlyWebSocketForEvent(legacyHm, kj::str(websocketId));

  capnp::MallocMessageBuilder message;
  auto params =
      message
          .initRoot<rpc::HibernatableWebSocketEventDispatcher::HibernatableWebSocketEventParams>();
  auto eventMessage = params.initMessage();
  eventMessage.initPayload().setText("hello"_kj);
  eventMessage.setWebsocketId(websocketId);
  auto event = kj::refcounted<api::HibernatableWebSocketCustomEvent>(
      0, kj::heap<api::HibernationReader>(params.asReader()));

  auto exception = kj::runCatchingExceptions([&]() {
    api::HibernatableWebSocketCustomEventTestAccess::ensureHibernationManagerForEvent(
        *event, fixture.getActor(), websocketId);
  });
  auto& e =
      KJ_ASSERT_NONNULL(exception, "expected an adoption by a different Durable Object to fail");
  KJ_ASSERT(e.getDescription().endsWith(
                "hibernatable WebSocket event names a hibernation manager owned by a "
                "different actor"_kj),
      e);

  // The rejected actor is left with no manager at all, rather than holding the owner's sockets.
  KJ_ASSERT(fixture.getActor().getHibernationManager() == kj::none);
  KJ_ASSERT(LegacyHibernationManagerTestAccess::hasEventRegistration(websocketId));
}

KJ_TEST("HibernationManager: an event re-resolves the pipeline only when no actor owns it") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("force-fresh-unowned")));
  auto hm = makeTestHm(fixture);
  auto& legacyHm = kj::downcast<LegacyHibernationManagerImpl>(*hm);
  auto request = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(fixture, *request, *hm);

  // No actor has claimed this manager, as when the actor holding it goes away. The loopback it
  // holds can name a version that is no longer current, so the pipeline is re-resolved from its
  // script ID.
  KJ_ASSERT(hm->getOwningActor() == kj::none);
  auto worker1 KJ_UNUSED = LegacyHibernationManagerTestAccess::getWorkerForOnlyWebSocket(legacyHm);
  KJ_ASSERT(KJ_ASSERT_NONNULL(stats.lastReresolveActorPipeline) == ReresolveActorPipeline::YES);

  // A live owning actor holds a current pipeline, so reusing its loopback's is correct.
  fixture.getActor().setHibernationManager(hm->addRef());
  auto worker2 KJ_UNUSED = LegacyHibernationManagerTestAccess::getWorkerForOnlyWebSocket(legacyHm);
  KJ_ASSERT(KJ_ASSERT_NONNULL(stats.lastReresolveActorPipeline) == ReresolveActorPipeline::NO);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: a hibernated socket on a running actor reuses the pipeline") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("force-fresh-packaged")));
  auto hm = makeTestHm(fixture);
  auto& legacyHm = kj::downcast<LegacyHibernationManagerImpl>(*hm);
  auto request = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(fixture, *request, *hm);
  fixture.getActor().setHibernationManager(hm->addRef());

  // Package the sockets away, as an idle actor does. Packaging is not the signal: un-packaging is
  // per-socket, so keying on it would re-resolve the pipeline for every socket a woken actor has
  // not yet un-packaged.
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });

  auto worker KJ_UNUSED = LegacyHibernationManagerTestAccess::getWorkerForOnlyWebSocket(legacyHm);
  KJ_ASSERT(KJ_ASSERT_NONNULL(stats.lastReresolveActorPipeline) == ReresolveActorPipeline::NO);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: delivering one event leaves other hibernated WebSockets registered") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("event-routing-multi-socket")));
  auto hm = makeTestHm(fixture);
  auto& legacyHm = kj::downcast<LegacyHibernationManagerImpl>(*hm);
  auto request = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(fixture, *request, *hm, "first"_kj);
  auto end2 KJ_UNUSED = acceptNewWebSocket(fixture, *request, *hm, "second"_kj);
  fixture.getActor().setHibernationManager(hm->addRef());

  constexpr kj::StringPtr firstId = "multi-socket-event-first"_kj;
  constexpr kj::StringPtr secondId = "multi-socket-event-second"_kj;
  LegacyHibernationManagerTestAccess::registerWebSocketForEvent(legacyHm, kj::str(firstId), 0);
  LegacyHibernationManagerTestAccess::registerWebSocketForEvent(legacyHm, kj::str(secondId), 1);
  KJ_ASSERT(LegacyHibernationManagerTestAccess::registeredEventCount(legacyHm) == 2);

  // Deliver only the first event. An actor holds many hibernated WebSockets at once, so waking one
  // of them must leave the rest routable.
  fixture.enterContext(*request, [&](const TestFixture::Environment&) {
    KJ_ASSERT(
        LegacyHibernationManagerTestAccess::takeWebSocketForEventMatches(firstId, legacyHm, 0));
  });

  KJ_ASSERT(!LegacyHibernationManagerTestAccess::hasEventRegistration(firstId));
  KJ_ASSERT(LegacyHibernationManagerTestAccess::findManagerForEventMatches(secondId, legacyHm));
  KJ_ASSERT(LegacyHibernationManagerTestAccess::registeredEventCount(legacyHm) == 1);

  fixture.enterContext(*request, [&](const TestFixture::Environment&) {
    KJ_ASSERT(
        LegacyHibernationManagerTestAccess::takeWebSocketForEventMatches(secondId, legacyHm, 1));
  });
  KJ_ASSERT(!LegacyHibernationManagerTestAccess::hasEventRegistration(secondId));
  KJ_ASSERT(LegacyHibernationManagerTestAccess::registeredEventCount(legacyHm) == 0);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: destroying a manager removes its event registrations") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("event-routing-destroyed-manager")));
  auto request = fixture.newIncomingRequest();

  constexpr kj::StringPtr websocketId = "destroyed-manager-event"_kj;
  kj::Own<kj::WebSocket> end1;
  {
    auto hm = makeTestHm(fixture);
    auto& legacyHm = kj::downcast<LegacyHibernationManagerImpl>(*hm);
    end1 = acceptNewWebSocket(fixture, *request, *hm);

    LegacyHibernationManagerTestAccess::registerOnlyWebSocketForEvent(
        legacyHm, kj::str(websocketId));
    KJ_ASSERT(LegacyHibernationManagerTestAccess::hasEventRegistration(websocketId));

    // Deliberately not handed to the actor, so `hm` holds the only reference and the manager is
    // destroyed with the event still registered.
  }

  // The registry doesn't own the manager, so the manager can go away while an event is registered.
  // Its destructor must take the registration with it, or the next event with this ID would be
  // routed into freed memory.
  KJ_ASSERT(!LegacyHibernationManagerTestAccess::hasEventRegistration(websocketId));

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: cancelling an event deregisters it") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("event-routing-cancel")));
  auto hm = makeTestHm(fixture);
  auto& legacyHm = kj::downcast<LegacyHibernationManagerImpl>(*hm);
  auto request = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(fixture, *request, *hm);
  fixture.getActor().setHibernationManager(hm->addRef());

  constexpr kj::StringPtr websocketId = "cancel-event"_kj;
  LegacyHibernationManagerTestAccess::registerOnlyWebSocketForEvent(legacyHm, kj::str(websocketId));
  KJ_ASSERT(LegacyHibernationManagerTestAccess::hasEventRegistration(websocketId));

  LegacyHibernationManagerTestAccess::cancelEvent(legacyHm, websocketId);

  KJ_ASSERT(!LegacyHibernationManagerTestAccess::hasEventRegistration(websocketId));
  KJ_ASSERT(LegacyHibernationManagerTestAccess::registeredEventCount(legacyHm) == 0);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: cancelling an already-claimed event is a no-op") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("event-routing-cancel-claimed")));
  auto hm = makeTestHm(fixture);
  auto& legacyHm = kj::downcast<LegacyHibernationManagerImpl>(*hm);
  auto request = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(fixture, *request, *hm);
  fixture.getActor().setHibernationManager(hm->addRef());

  constexpr kj::StringPtr websocketId = "cancel-claimed-event"_kj;
  LegacyHibernationManagerTestAccess::registerOnlyWebSocketForEvent(legacyHm, kj::str(websocketId));

  // Cleanup after a dispatch runs unconditionally, and normally has nothing left to do because the
  // handler already claimed the WebSocket.
  fixture.enterContext(*request, [&](const TestFixture::Environment&) {
    KJ_ASSERT(
        LegacyHibernationManagerTestAccess::takeWebSocketForEventMatches(websocketId, legacyHm));
  });
  KJ_ASSERT(!LegacyHibernationManagerTestAccess::hasEventRegistration(websocketId));

  LegacyHibernationManagerTestAccess::cancelEvent(legacyHm, websocketId);

  KJ_ASSERT(!LegacyHibernationManagerTestAccess::hasEventRegistration(websocketId));
  KJ_ASSERT(LegacyHibernationManagerTestAccess::registeredEventCount(legacyHm) == 0);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: unresolvable event ID fails to claim a WebSocket") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("event-routing-unresolvable")));
  auto hm = makeTestHm(fixture);
  auto& legacyHm = kj::downcast<LegacyHibernationManagerImpl>(*hm);
  auto request = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(fixture, *request, *hm);
  fixture.getActor().setHibernationManager(hm->addRef());

  constexpr kj::StringPtr websocketId = "unresolvable-event"_kj;

  kj::Maybe<kj::Exception> exception;
  fixture.enterContext(*request, [&](const TestFixture::Environment&) {
    exception = kj::runCatchingExceptions([&]() {
      LegacyHibernationManagerTestAccess::takeWebSocketForEventMatches(websocketId, legacyHm);
    });
  });
  KJ_ASSERT(exception != kj::none, "expected the unresolvable event ID to fail");

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: rejected RPC dispatch leaves the event registration to its owner") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("event-routing-rpc-rejected")));
  auto hm = makeTestHm(fixture);
  auto& legacyHm = kj::downcast<LegacyHibernationManagerImpl>(*hm);
  auto request = fixture.newIncomingRequest();
  auto end1 KJ_UNUSED = acceptNewWebSocket(fixture, *request, *hm);
  fixture.getActor().setHibernationManager(hm->addRef());

  constexpr kj::StringPtr websocketId = "rejected-rpc-event"_kj;
  LegacyHibernationManagerTestAccess::registerOnlyWebSocketForEvent(legacyHm, kj::str(websocketId));

  capnp::ByteStreamFactory byteStreamFactory;
  kj::HttpHeaderTable::Builder headerTableBuilder;
  capnp::HttpOverCapnpFactory httpOverCapnpFactory(byteStreamFactory,
      capnp::HttpOverCapnpFactory::HeaderIdBundle(headerTableBuilder),
      capnp::HttpOverCapnpFactory::LEVEL_2);

  auto paf = kj::newPromiseAndFulfiller<void>();
  kj::String receivedWebsocketId;
  bool dispatcherCalled = false;
  auto dispatcher = rpc::HibernatableWebSocketEventDispatcher::Client(
      kj::heap<ControlledHibernatableEventDispatcher>(
          kj::mv(paf.promise), receivedWebsocketId, dispatcherCalled))
                        .castAs<rpc::EventDispatcher>();

  auto event = kj::refcounted<api::HibernatableWebSocketCustomEvent>(
      0, api::HibernatableSocketParams(kj::str("hello"), kj::str(websocketId)));
  auto rpcPromise = event->sendRpc(httpOverCapnpFactory, byteStreamFactory,
      getUnsupportedFrankenvalueHandler(), kj::mv(dispatcher));

  fixture.pollEventLoop();
  KJ_ASSERT(dispatcherCalled);
  KJ_ASSERT(receivedWebsocketId == websocketId);
  KJ_ASSERT(LegacyHibernationManagerTestAccess::findManagerForEventMatches(websocketId, legacyHm));

  paf.fulfiller->reject(KJ_EXCEPTION(FAILED, "test RPC rejection before claim"));
  auto exception = kj::runCatchingExceptions([&]() { rpcPromise.wait(fixture.getWaitScope()); });
  KJ_ASSERT(exception != kj::none, "expected RPC rejection");

  // The dispatch attempt does not own the registration, so a rejection leaves it for the task that
  // registered it to clean up. Deregistering here would race that task's cleanup.
  KJ_ASSERT(LegacyHibernationManagerTestAccess::findManagerForEventMatches(websocketId, legacyHm));

  LegacyHibernationManagerTestAccess::cancelEvent(legacyHm, websocketId);
  KJ_ASSERT(!LegacyHibernationManagerTestAccess::hasEventRegistration(websocketId));

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: event without a registered manager returns exception") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("event-routing-missing-manager")));

  capnp::MallocMessageBuilder message;
  auto params =
      message
          .initRoot<rpc::HibernatableWebSocketEventDispatcher::HibernatableWebSocketEventParams>();
  auto eventMessage = params.initMessage();
  auto payload = eventMessage.initPayload();
  payload.setText("hello"_kj);
  eventMessage.setWebsocketId("missing-manager-event"_kj);

  auto event = kj::refcounted<api::HibernatableWebSocketCustomEvent>(
      0, kj::heap<api::HibernationReader>(params.asReader()));

  auto exception = kj::runCatchingExceptions([&]() {
    api::HibernatableWebSocketCustomEventTestAccess::ensureHibernationManagerForEvent(
        *event, fixture.getActor(), "missing-manager-event"_kj);
  });
  auto& e = KJ_ASSERT_NONNULL(exception, "expected missing manager to throw");
  KJ_ASSERT(e.getType() == kj::Exception::Type::FAILED, e);
  KJ_ASSERT(e.getDescription().endsWith(
                "hibernatable WebSocket event manager was not found for this event ID"_kj),
      e);
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

  KJ_ASSERT(stats.customEventCalls == 1, "expected exactly one customEvent dispatch",
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

  KJ_ASSERT(stats.customEventCalls == 1, "expected exactly one customEvent dispatch",
      stats.customEventCalls);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: failed event dispatches remove WebSocket") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("failed-termination-dispatch")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm, "terminated"_kj);

  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });

  stats.rejectCustomEvents = true;
  end1->send("message"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();
  KJ_ASSERT(stats.customEventCalls == 2, stats.customEventCalls);

  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    KJ_ASSERT(hm->getWebSockets(env.js, kj::none).size() == 0);
    KJ_ASSERT(hm->getWebSockets(env.js, "terminated"_kj).size() == 0);
  });
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: retained native WebSocket tags outlive manager teardown") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("retained-tags")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  constexpr kj::StringPtr tag =
      "a-long-hibernatable-websocket-tag-that-must-remain-valid-after-manager-teardown"_kj;
  auto end1 KJ_UNUSED = acceptNewWebSocket(fixture, *request, *hm, tag);

  kj::Maybe<jsg::Ref<api::WebSocket>> retained;
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto websockets = hm->getWebSockets(env.js, tag);
    KJ_ASSERT(websockets.size() == 1);
    auto tags = websockets[0]->getHibernatableTags();
    KJ_ASSERT(tags.size() == 1);
    KJ_ASSERT(tags[0] == tag, tags[0]);
    retained = websockets[0].addRef();
  });

  // The native WebSocket can be retained independently of the manager. Its tags must remain
  // readable after the manager removes the final socket and destroys the corresponding bucket.
  fixture.enterContext(*request, [&](const TestFixture::Environment&) {
    hm = nullptr;

    auto tags = KJ_REQUIRE_NONNULL(retained)->getHibernatableTags();
    KJ_ASSERT(tags.size() == 1);
    KJ_ASSERT(tags[0] == tag, tags[0]);
  });

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
  KJ_ASSERT(stats.customEventCalls == 1, "expected exactly one customEvent dispatch",
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

KJ_TEST("HibernationManager: in-flight auto-response survives repeated hibernation before close") {
  // Each replacement api::WebSocket must wait for an auto-response started by the original
  // instance before sending its close.
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("ew-10817-autoresp")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  // Send ping → readLoop → sendAutoResponse → BlockedSend.
  end1->send("ping"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();

  // Hibernate, revive without consuming the pong, then hibernate again. The manager must retain
  // its own branch of the pending send when it gives the first replacement a branch.
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto websockets = hm->getWebSockets(env.js, kj::none);
    KJ_ASSERT(websockets.size() == 1);
  });
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });

  // Revive again and queue a close behind the in-flight auto-response.
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto websockets = hm->getWebSockets(js, kj::none);
    KJ_ASSERT(websockets.size() == 1);
    websockets[0]->close(js, 1001, jsg::USVString(kj::str("after-pong")));
  });
  fixture.pollEventLoop();

  auto pong = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(pong.is<kj::String>() && pong.get<kj::String>() == "pong"_kj);

  auto closePromise = end1->receive();
  KJ_ASSERT(closePromise.poll(fixture.getWaitScope()), "close did not follow auto-response");
  auto closeMessage = closePromise.wait(fixture.getWaitScope());
  KJ_ASSERT(closeMessage.is<kj::WebSocket::Close>());
  auto& close = closeMessage.get<kj::WebSocket::Close>();
  KJ_ASSERT(close.code == 1001, close.code);
  KJ_ASSERT(close.reason == "after-pong"_kj, close.reason);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: active auto-response after revival waits for old pump") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("revived-active-autoresp")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  sendFromDo(fixture, *request, *hm, "before-hibernation"_kj);
  fixture.pollEventLoop();
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });

  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    KJ_ASSERT(hm->getWebSockets(env.js, kj::none).size() == 1);
  });
  end1->send("ping"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();

  auto first = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(first.is<kj::String>() && first.get<kj::String>() == "before-hibernation"_kj);
  auto pong = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(pong.is<kj::String>() && pong.get<kj::String>() == "pong"_kj);
  KJ_ASSERT(stats.customEventCalls == 0, stats.customEventCalls);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: manager teardown cancels deferred auto-response") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("teardown-deferred-autoresp")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  sendFromDo(fixture, *request, *hm, "blocked"_kj);
  fixture.pollEventLoop();
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });

  kj::Maybe<jsg::Ref<api::WebSocket>> retained;
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto websockets = hm->getWebSockets(env.js, kj::none);
    KJ_ASSERT(websockets.size() == 1);
    retained = websockets[0].addRef();
  });

  // Queue an auto-response behind the old pump, then keep its fork alive through the revived API
  // WebSocket while destroying the manager and its native WebSocket.
  end1->send("ping"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();
  fixture.enterContext(*request, [&](const TestFixture::Environment&) { hm = nullptr; });
  fixture.pollEventLoop();

  KJ_ASSERT(stats.customEventCalls == 0, stats.customEventCalls);
  fixture.enterContext(*request, [&](const TestFixture::Environment&) { retained = kj::none; });
  end1 = nullptr;
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: packaged in-flight auto-response finishes before close") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("packaged-autoresp")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });

  // The manager sends the pong directly while the api::WebSocket is packaged.
  end1->send("ping"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();

  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto websockets = hm->getWebSockets(env.js, kj::none);
    KJ_ASSERT(websockets.size() == 1);
    websockets[0]->close(env.js, 1001, jsg::USVString(kj::str("after-packaged-pong")));
  });
  fixture.pollEventLoop();

  auto pong = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(pong.is<kj::String>() && pong.get<kj::String>() == "pong"_kj);

  auto closeMessage = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(closeMessage.is<kj::WebSocket::Close>());
  auto& close = closeMessage.get<kj::WebSocket::Close>();
  KJ_ASSERT(close.code == 1001, close.code);
  KJ_ASSERT(close.reason == "after-packaged-pong"_kj, close.reason);
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: rejected packaged auto-response removes revived WebSocket") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("rejected-packaged-autoresp")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm, "pending"_kj);

  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });
  end1->send("ping"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();

  // Revival gives the replacement adapter a branch of the blocked send. Disconnecting the peer
  // rejects both branches and must terminate the manager's socket exactly once.
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto websockets = hm->getWebSockets(env.js, kj::none);
    KJ_ASSERT(websockets.size() == 1);
  });
  end1 = nullptr;
  fixture.pollEventLoop();

  KJ_ASSERT(stats.customEventCalls == 1, stats.customEventCalls);
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    KJ_ASSERT(hm->getWebSockets(env.js, kj::none).size() == 0);
    KJ_ASSERT(hm->getWebSockets(env.js, "pending"_kj).size() == 0);
  });
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: revived sends wait across repeated hibernation") {
  // A DO-side send can remain blocked after hibernation while the revived api::WebSocket queues
  // another operation on the same native WebSocket. The revived pump must wait for the old pump
  // so both operations reach the peer in order.
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("ew-10817-dosend")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  sendFromDo(fixture, *request, *hm, "hello from DO"_kj);

  fixture.pollEventLoop();

  // Hibernate.
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });

  sendFromDo(fixture, *request, *hm, "after-first-hibernation"_kj);
  fixture.pollEventLoop();

  // Package the replacement adapter while its pump is waiting for the original send.
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });

  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto websockets = hm->getWebSockets(js, kj::none);
    KJ_ASSERT(websockets.size() == 1);
    websockets[0]->close(js, 1001, jsg::USVString(kj::str("after-hibernation")));
  });
  fixture.pollEventLoop();

  auto message = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(message.is<kj::String>() && message.get<kj::String>() == "hello from DO"_kj);

  auto revivedMessage = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(revivedMessage.is<kj::String>() &&
      revivedMessage.get<kj::String>() == "after-first-hibernation"_kj);

  auto closePromise = end1->receive();
  fixture.pollEventLoop();
  KJ_ASSERT(closePromise.poll(fixture.getWaitScope()), "revived close remained blocked");
  auto closeMessage = closePromise.wait(fixture.getWaitScope());
  KJ_ASSERT(closeMessage.is<kj::WebSocket::Close>());
  auto& close = closeMessage.get<kj::WebSocket::Close>();
  KJ_ASSERT(close.code == 1001, close.code);
  KJ_ASSERT(close.reason == "after-hibernation"_kj, close.reason);
  fixture.drainAndDestroy(kj::mv(request));
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
  // Dropping the IR below (without draining — the pump task is stuck in waitUntilTasks, so
  // drain() would hang) triggers a "failed to invoke drain()" warning. The block scope
  // around that drop captures the warning so the test output stays clean.
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
  {
    KJ_EXPECT_LOG(WARNING, "failed to invoke drain() on IncomingRequest before destroying it");
    request1 = nullptr;
  }
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

KJ_TEST("HibernationManager: in-flight DO send lost across IoContext destruction") {
  // Data-frame sibling of the close-race test above. Same physics: pump stuck on a BlockedSend
  // → IoContext destruction cancels mid-send → bytes gone. Both are flavors of the
  // silent-message-drop bug class, and both are incidentally fixed by the unified-queue
  // refactor (queue lives on the adapter, persists across IoContexts).
  //
  // Unlike the close case there's no closedOutgoing equivalent to prevent further sends — on
  // revival the DO's new api::WebSocket has a fresh queue and ws.send() resumes working
  // normally, but the doomed message is permanently lost and the DO has no error path
  // indicating non-delivery.
  //
  // Dropping the IR below (without draining — the pump task is stuck in waitUntilTasks, so
  // drain() would hang) triggers a "failed to invoke drain()" warning. The block scope
  // around that drop captures the warning so the test output stays clean.
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("send-loss-cross-ioc")));
  auto hm = makeTestHm(fixture);
  auto request1 = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request1, *hm);

  sendFromDo(fixture, *request1, *hm, "doomed-message"_kj);
  fixture.pollEventLoop();

  // Hibernate, then drop the IR (destroying the IoContext). The pump's in-flight ws.send()
  // is canceled.
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });
  {
    KJ_EXPECT_LOG(WARNING, "failed to invoke drain() on IncomingRequest before destroying it");
    request1 = nullptr;
  }
  fixture.resetActor();

  // The eyeball's receive promise should NOT resolve — the data frame was canceled mid-send.
  // Verify by polling: receive should not be ready immediately. (We can't easily assert
  // "never resolves" in a test, so we observe the not-yet-ready state and move on.)
  auto receivePromise = end1->receive();
  fixture.pollEventLoop();
  KJ_ASSERT(!receivePromise.poll(fixture.getWaitScope()),
      "data frame was silently dropped across IoContext destruction; eyeball receives nothing");
}

KJ_TEST("HibernationManager: in-flight auto-response finishes before close across actor eviction") {
  // sendAutoResponse runs from the HM's readLoop, outside the IoContext. Its in-flight send must
  // remain visible after hibernation so a revived api::WebSocket waits before sending its close.
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("ew-10817-cross-autoresp")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);

  auto request1 = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request1, *hm);

  // Eyeball sends ping → HM readLoop → sendAutoResponse → BlockedSend on the pipe.
  end1->send("ping"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();

  // Hibernate, drop the IR (IoContext1 is destroyed; the BlockedSend survives because
  // sendAutoResponse runs outside any IoContext). Then evict the actor.
  //
  // Unlike the in-flight DO-close / DO-send variants above, no "failed to invoke drain()
  // on IncomingRequest" warning fires here: sendAutoResponse runs on the HM's TaskSet and
  // does not enqueue a waitUntil task on the IR, so the IR has nothing to drain at
  // destruction. If a regression were to plumb sendAutoResponse through the IR's
  // waitUntilTasks, that warning would start firing and this test would need updating.
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });
  request1 = nullptr;
  fixture.resetActor();

  // Under a brand-new actor + IoContext, queue a close behind the in-flight auto-response.
  auto request2 = fixture.newIncomingRequest();
  fixture.enterContext(*request2, [&](const TestFixture::Environment& env) {
    auto& js = env.js;
    auto websockets = hm->getWebSockets(js, kj::none);
    KJ_ASSERT(websockets.size() == 1);
    websockets[0]->close(js, 1001, jsg::USVString(kj::str("post-evict")));
  });
  fixture.pollEventLoop();

  auto pong = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(pong.is<kj::String>() && pong.get<kj::String>() == "pong"_kj);

  auto closePromise = end1->receive();
  KJ_ASSERT(closePromise.poll(fixture.getWaitScope()), "close did not follow auto-response");
  auto closeMessage = closePromise.wait(fixture.getWaitScope());
  KJ_ASSERT(closeMessage.is<kj::WebSocket::Close>());
  auto& close = closeMessage.get<kj::WebSocket::Close>();
  KJ_ASSERT(close.code == 1001, close.code);
  KJ_ASSERT(close.reason == "post-evict"_kj, close.reason);
  fixture.drainAndDestroy(kj::mv(request2));
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

KJ_TEST("HibernationManager: auto-response is skipped after close is queued") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("autoresp-after-close")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  auto paf = kj::newPromiseAndFulfiller<void>();
  auto blocker = fixture.getActor().getOutputGate().lockWhile(kj::mv(paf.promise), nullptr);
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto websockets = hm->getWebSockets(env.js, kj::none);
    KJ_ASSERT(websockets.size() == 1);
    websockets[0]->close(env.js, 1001, jsg::USVString(kj::str("already-closing")));
  });

  // The pump is blocked before sending close. A later ping must not queue a pong which will be
  // discarded when close is eventually sent.
  end1->send("ping"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();

  paf.fulfiller->fulfill();
  auto message = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(message.is<kj::WebSocket::Close>());
  KJ_ASSERT(message.get<kj::WebSocket::Close>().reason == "already-closing"_kj);
  fixture.pollEventLoop();
  KJ_ASSERT(stats.customEventCalls == 0, stats.customEventCalls);

  blocker.wait(fixture.getWaitScope());
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST(
    "HibernationManager: queued auto-response survives hibernation while pump is gate-blocked") {
  // When the pump is already running (isPumping == true) and stalled on the output gate for a
  // queued DO message, an arriving auto-response request causes sendAutoResponse to push the
  // pong onto pendingAutoResponseDeque. The pump only drains that deque after it finishes the
  // outer outgoingMessages loop, so the pong waits for the gate to release transitively.
  //
  // Order at the eyeball: the gated DO message arrives first after the gate releases, followed by
  // the pong and any write queued by a replacement api::WebSocket.
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

  // The replacement queues its close behind the pong's completion while the original pump remains
  // blocked on the output gate.
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto websockets = hm->getWebSockets(env.js, kj::none);
    KJ_ASSERT(websockets.size() == 1);
    websockets[0]->close(env.js, 1001, jsg::USVString(kj::str("after-queued-pong")));
  });

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
  auto closeMessage = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(closeMessage.is<kj::WebSocket::Close>());
  auto& close = closeMessage.get<kj::WebSocket::Close>();
  KJ_ASSERT(close.code == 1001, close.code);
  KJ_ASSERT(close.reason == "after-queued-pong"_kj, close.reason);

  blocker.wait(fixture.getWaitScope());
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: canceled queued auto-response preserves revived WebSocket") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("canceled-queued-autoresp")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto request1 = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request1, *hm, "pending"_kj);

  auto paf = kj::newPromiseAndFulfiller<void>();
  auto blocker = fixture.getActor().getOutputGate().lockWhile(kj::mv(paf.promise), nullptr);
  sendFromDo(fixture, *request1, *hm, "blocked"_kj);
  end1->send("ping"_kj).wait(fixture.getWaitScope());

  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });
  fixture.enterContext(*request1, [&](const TestFixture::Environment& env) {
    KJ_ASSERT(hm->getWebSockets(env.js, kj::none).size() == 1);
  });

  // Destroying the IoContext cancels the original pump. As before this fix, its queued pong is
  // dropped without terminating the manager's WebSocket.
  {
    KJ_EXPECT_LOG(WARNING, "failed to invoke drain() on IncomingRequest before destroying it");
    request1 = nullptr;
  }
  paf.fulfiller->fulfill();
  blocker.wait(fixture.getWaitScope());
  fixture.pollEventLoop();

  KJ_ASSERT(stats.customEventCalls == 0, stats.customEventCalls);
  fixture.resetActor();
  auto request2 = fixture.newIncomingRequest();
  fixture.enterContext(*request2, [&](const TestFixture::Environment& env) {
    KJ_ASSERT(hm->getWebSockets(env.js, kj::none).size() == 1);
    KJ_ASSERT(hm->getWebSockets(env.js, "pending"_kj).size() == 1);
  });
  fixture.drainAndDestroy(kj::mv(request2));
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

// Regression test for VULN-136638.
//
// In the hibernated branch of readLoop, an auto-response was sent by passing the borrowed
// kj::ArrayPtr from autoResponsePair->response directly into ws.send(), then suspending on
// co_await. kj::WebSocket::send() borrows that ArrayPtr across the suspension (http.h:633:
// "The underlying buffer must remain valid ... until the returned promise resolves"). A
// concurrent setWebSocketAutoResponse() call from JS would free the borrowed buffer mid-send.
//
// This test parks the readLoop at the co_await with the borrow in flight, then calls
// setWebSocketAutoResponse(kj::none, kj::none) to free the kj::String backing the borrowed
// pointer, then drains the eyeball. Under ASAN, the pipe's receive reading through the
// freed pointer trips a use-after-free report. With the fix in place (a coroutine-local
// kj::str(...) copy in the hibernated branch), the receive returns the original bytes
// cleanly and the assertion passes.
//
// Outside ASAN this test only catches the bug probabilistically — the freed bytes may still
// be readable. CI runs ASAN, so the regression is caught there.
KJ_TEST("HibernationManager: hibernated auto-response copies buffer before suspending send "
        "(regression VULN-136638)") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("vuln-136638-autoresp-uaf")));

  // Use a distinct, non-trivial response so the comparison at the end is unambiguous and any
  // partial overwrite under non-ASAN is more likely to be detectable.
  constexpr kj::StringPtr kResponse = "AUTO-RESPONSE-PAYLOAD-VULN-136638"_kj;

  auto hm = makeTestHm(fixture, "ping"_kj, kResponse);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });

  // Eyeball sends ping. end1->send().wait() returns once the readLoop's ws.receive() has
  // consumed the message, but the readLoop may not yet have reached the
  // ws.send(...).fork() / co_await p inside the hibernated branch — drive the event loop
  // until it does. After this point, the readLoop is parked at co_await p with a
  // BlockedSend on the pipe holding (under the bug) a borrowed pointer into
  // autoResponsePair->response's heap buffer.
  end1->send("ping"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();

  // Free the borrowed buffer by clearing the auto-response pair. Production reaches this
  // synchronously from actor-state.c++:setWebSocketAutoResponse, which would race with the
  // parked readLoop. Here we call it directly while the readLoop is suspended — same effect,
  // deterministic.
  hm->setWebSocketAutoResponse(kj::none, kj::none);

  // Drain the eyeball. With the fix, the pipe reads the coroutine-local copy and we receive
  // the original bytes. Without the fix and under ASAN, the pipe reads freed memory and ASAN
  // fails the test with a use-after-free report.
  auto msg = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(msg.is<kj::String>(), "expected auto-response string message");
  KJ_ASSERT(msg.get<kj::String>() == kResponse, "auto-response bytes were corrupted",
      msg.get<kj::String>(), kResponse);

  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: GC collects WebSocket with in-flight auto-response") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("gc-in-flight-autoresp")));
  auto hm = makeTestHm(fixture, "ping"_kj, "pong"_kj);
  auto request = fixture.newIncomingRequest();

  // The shared helper intentionally leaks a ref. This test instead creates the V8 wrapper that
  // js.alloc() omits so GC owns the last reference after hibernation.
  kj::Own<kj::WebSocket> end1;
  jsg::WeakRef<api::WebSocket> weakApiWs = nullptr;
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    auto pipe = kj::newWebSocketPipe();
    end1 = kj::mv(pipe.ends[0]);
    auto apiWs = env.js.alloc<api::WebSocket>(env.js, kj::mv(pipe.ends[1]));
    weakApiWs = apiWs.getWeakRef(env.js);
    auto& handler = KJ_ASSERT_NONNULL(env.js.tryGetTypeHandler<jsg::Ref<api::WebSocket>>());
    auto wrapper KJ_UNUSED = handler.wrap(env.js, apiWs.addRef());
    hm->acceptWebSocket(kj::mv(apiWs), nullptr);
  });

  end1->send("ping"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();
  fixture.enterWorkerLock([&](Worker::Lock& lock) { hm->hibernateWebSockets(lock); });
  KJ_ASSERT(weakApiWs.isAlive());

  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    env.isolate->LowMemoryNotification();
    KJ_ASSERT(!weakApiWs.isAlive());
  });

  end1 = nullptr;
  fixture.pollEventLoop();
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST("HibernationManager: running pump retains socket after manager removal") {
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("pump-socket-lifetime")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm, "socket"_kj);

  auto paf = kj::newPromiseAndFulfiller<void>();
  auto blocker = fixture.getActor().getOutputGate().lockWhile(kj::mv(paf.promise), nullptr);

  // The pump borrows the native socket and suspends on the output gate.
  sendFromDo(fixture, *request, *hm, "pending"_kj);

  // Rejecting an inbound event makes the manager remove its socket entry.
  stats.rejectCustomEvents = true;
  end1->send("terminate"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();

  KJ_ASSERT(stats.customEventCalls == 2, stats.customEventCalls);
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    KJ_ASSERT(hm->getWebSockets(env.js, kj::none).size() == 0);
    KJ_ASSERT(hm->getWebSockets(env.js, "socket"_kj).size() == 0);
  });

  // Without the ownership fix, this resumes into ws.send() using the
  // manager's destroyed socket.
  paf.fulfiller->fulfill();

  auto message = end1->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(message.is<kj::String>());
  KJ_ASSERT(message.get<kj::String>() == "pending"_kj);

  blocker.wait(fixture.getWaitScope());
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST(
    "HibernationManager: failed termination dispatch retains socket under a gate-blocked pump") {
  // A failed event dispatch removes the manager entry while the API pump is blocked on the output
  // gate. The adapter keeps the native socket alive until the queued operation completes.
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("failed-termination-gated-pump")));
  auto hm = makeTestHm(fixture);
  auto request = fixture.newIncomingRequest();
  auto end1 = acceptNewWebSocket(fixture, *request, *hm);

  // Lock the output gate so the pump parks at its `co_await gatedMessage.outputLock`.
  auto paf = kj::newPromiseAndFulfiller<void>();
  auto blocker = fixture.getActor().getOutputGate().lockWhile(kj::mv(paf.promise), nullptr);

  sendFromDo(fixture, *request, *hm, "gated"_kj);
  auto receivePromise = end1->receive();
  fixture.pollEventLoop();
  KJ_ASSERT(!receivePromise.poll(fixture.getWaitScope()), "pump should be parked on the gate");

  // The eyeball sends a message, but the DO is too overloaded to accept either it or the error
  // event that follows, so the HibernationManager removes its entry.
  stats.rejectCustomEvents = true;
  end1->send("message"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();
  KJ_ASSERT(stats.customEventCalls == 2, stats.customEventCalls);
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    KJ_ASSERT(hm->getWebSockets(env.js, kj::none).size() == 0);
  });

  // The adapter still owns the socket, so the pump can deliver the queued message.
  paf.fulfiller->fulfill();
  auto message = receivePromise.wait(fixture.getWaitScope());
  KJ_ASSERT(message.is<kj::String>());
  KJ_ASSERT(message.get<kj::String>() == "gated"_kj);

  blocker.wait(fixture.getWaitScope());
  fixture.drainAndDestroy(kj::mv(request));
}

KJ_TEST(
    "HibernationManager: failed termination dispatch retains socket under a gate-blocked close") {
  // Companion to the test above for the Close branch of the pump's send loop.
  DispatchStats stats;
  TestFixture fixture(stubLoopbackParams(stats, kj::str("failed-termination-gated-close")));
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
  KJ_ASSERT(!receivePromise.poll(fixture.getWaitScope()), "pump should be parked on the gate");

  stats.rejectCustomEvents = true;
  end1->send("message"_kj).wait(fixture.getWaitScope());
  fixture.pollEventLoop();
  KJ_ASSERT(stats.customEventCalls == 2, stats.customEventCalls);
  fixture.enterContext(*request, [&](const TestFixture::Environment& env) {
    KJ_ASSERT(hm->getWebSockets(env.js, kj::none).size() == 0);
  });

  // The adapter still owns the socket, so the pump can deliver the queued close.
  paf.fulfiller->fulfill();
  auto message = receivePromise.wait(fixture.getWaitScope());
  KJ_ASSERT(message.is<kj::WebSocket::Close>());
  auto& close = message.get<kj::WebSocket::Close>();
  KJ_ASSERT(close.code == 1001, close.code);
  KJ_ASSERT(close.reason == "gated-bye"_kj, close.reason);

  blocker.wait(fixture.getWaitScope());
  fixture.drainAndDestroy(kj::mv(request));
}

}  // namespace
}  // namespace workerd
