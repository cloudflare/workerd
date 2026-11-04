// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/web-socket.h>
#include <workerd/io/legacy-hibernation-manager.h>
#include <workerd/io/worker-interface.h>
#include <workerd/io/worker.h>
#include <workerd/tests/test-fixture.h>

#include <kj/compat/http.h>

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
      return &kj::downcast<LegacyHibernationManagerImpl>(*manager) == &expectedManager;
    }
    return false;
  }

  static void cancelEvent(LegacyHibernationManagerImpl& manager, kj::StringPtr websocketId) {
    manager.cancelEvent(websocketId);
  }

  static kj::Own<WorkerInterface> getWorkerForOnlyWebSocket(LegacyHibernationManagerImpl& manager) {
    return manager.getWorkerForEvent(getOnlyWebSocket(manager), EndsWebSocket::NO);
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

  static kj::Rc<HibernatableTransport> getOnlyTransport(LegacyHibernationManagerImpl& manager) {
    return getOnlyWebSocket(manager).transport.addRef();
  }

  static bool hasPendingReceive(LegacyHibernationManagerImpl& manager) {
    return getOnlyWebSocket(manager).pendingReceive != kj::none;
  }

  static bool isDetaching(Worker::Actor::HibernationManager& manager) {
    return kj::downcast<LegacyHibernationManagerImpl>(manager).detachment != kj::none;
  }

  // Each WebSocket's tags joined with "+", in the order the WebSockets were accepted, joined
  // with ",".
  static kj::String describeTagsInAcceptOrder(LegacyHibernationManagerImpl& manager) {
    kj::Vector<kj::String> sockets;
    for (auto it = manager.allWs.rbegin(); it != manager.allWs.rend(); ++it) {
      sockets.add(kj::strArray((*it)->getTags(), "+"));
    }
    return kj::strArray(sockets, ",");
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

namespace test {

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

// Helpers below are intentionally split so the HibernationManager can outlive any single
// IncomingRequest, which matters for tests that span multiple IRs. makeTestHm() needs no
// IoContext; acceptNewWebSocket() and sendFromDo() do (the api::WebSocket constructor stores
// IoOwn members, and ws.send() is delivered through the IoContext's pump).

// SetupParams builder that installs a StubLoopback on the actor referencing `stats`. The
// caller MUST keep `stats` alive for the lifetime of the resulting TestFixture (declare it
// before the fixture). The same StubLoopback is later retrieved via actor.getLoopback() and
// handed to the HM, so actor and HM share a single Loopback (mirroring production).
inline TestFixture::SetupParams stubLoopbackParams(
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
inline kj::Rc<Worker::Actor::HibernationManager> makeTestHm(TestFixture& fixture) {
  auto hm = kj::rc<LegacyHibernationManagerImpl>(fixture.getActor().getLoopback(), 0);
  hm->setTimerChannel(fixture.getTimerChannel());
  return hm;
}

// Same, but configure auto-response. Both `autoRequest` and `autoResponse` are required.
inline kj::Rc<Worker::Actor::HibernationManager> makeTestHm(
    TestFixture& fixture, kj::StringPtr autoRequest, kj::StringPtr autoResponse) {
  auto hm = makeTestHm(fixture);
  hm->setWebSocketAutoResponse(autoRequest, autoResponse);
  return hm;
}

// Create an api::WebSocket, accept it into the HM under `tag` (or untagged if `tag` is empty),
// and return the eyeball end of the new pipe. Tests can call this multiple times to attach
// multiple concurrent WebSockets; pass distinct tags to identify them later via getWebSockets.
// With `transport`, the WebSocket is carried by the transport that the handle stands for, as if
// coupled to a request that supplied it.
//
// Needs an IoContext (the api::WebSocket constructor stores IoOwn members), supplied by the
// IR. Test code should pick an IR whose IoContext should "own" this api::WebSocket.
inline kj::Own<kj::WebSocket> acceptNewWebSocket(TestFixture& fixture,
    IoContext::IncomingRequest& request,
    Worker::Actor::HibernationManager& hm,
    kj::StringPtr tag = ""_kj,
    kj::Maybe<kj::Own<HibernatableTransportHandle>> transport = kj::none) {
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
    hm.acceptWebSocket(apiWs.addRef(), tags);
    KJ_IF_SOME(t, transport) {
      KJ_ASSERT_NONNULL(apiWs->getHibernatableTransport()).attach(kj::mv(t));
    }
  });
  return eyeball;
}

// Send a string message from the DO side, on the WebSocket identified by `tag` (or the only
// untagged one if `tag` is empty). Enters the supplied IR's IoContext for the duration of
// the send setup; the actual ws.send happens asynchronously after the lock is released.
inline void sendFromDo(TestFixture& fixture,
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

}  // namespace test
}  // namespace workerd
