// Copyright (c) 2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/api/memory-cache.h>
#include <workerd/io/io-context.h>
#include <workerd/io/worker.h>
#include <workerd/jsg/jsg.h>
#include <workerd/server/workerd.capnp.h>

#include <capnp/message.h>
#include <kj/function.h>
#include <kj/test.h>

namespace workerd {

// TestFixture is responsible for creating workerd environment during tests.
// All the infrastructure is started in the constructor. It is accessed through run() method.
struct TestFixture {
  struct SetupParams {
    // waitScope of outer IO loop. New IO will be set up if missing.
    kj::Maybe<kj::WaitScope&> waitScope;
    kj::Maybe<CompatibilityFlags::Reader> featureFlags;
    kj::Maybe<kj::StringPtr> mainModuleSource;
    // If set, make a stub of an Actor with the given id.
    kj::Maybe<Worker::Actor::Id> actorId;
    // If true, use real timers instead of mock timers that never advance.
    // Requires waitScope to be kj::none (so that the fixture creates its own AsyncIoContext).
    bool useRealTimers;
    // If set, used instead of the default DummyIoChannelFactory when creating incoming requests.
    // The factory receives the TimerChannel reference.
    kj::Maybe<kj::Function<kj::Own<IoChannelFactory>(TimerChannel&)>> ioChannelFactory;
    // If set, used as the actor's Loopback (only meaningful when actorId is set). Defaults to a
    // MockActorLoopback that throws on getWorker(). Tests that need to intercept hibernation
    // event dispatch can supply a custom Loopback here, then later retrieve it (or a fresh ref
    // to it) via actor.getLoopback(). This way the actor and the HibernationManager share a
    // single Loopback, mirroring production.
    kj::Maybe<kj::Own<Worker::Actor::Loopback>> actorLoopback;
    // If set, called to create the RequestObserver for each IncomingRequest instead of the default
    // no-op base RequestObserver. Lets tests observe metrics hooks (e.g. recording the values
    // passed to setNextSubrequestBodyRewindable()).
    kj::Maybe<kj::Function<kj::Own<RequestObserver>()>> requestObserverFactory;
  };

  TestFixture(SetupParams&& params = {.useRealTimers = false});

  struct V8Environment {
    v8::Isolate* isolate;
  };

  struct Environment: public V8Environment {
    IoContext& context;
    Worker::Lock& lock;
    jsg::Lock& js;
    CompatibilityFlags::Reader features;
  };

  template <typename T>
  struct RunReturnType {
    using Type = T;
  };
  template <typename T>
  struct RunReturnType<kj::Promise<T>> {
    using Type = T;
  };

  // Setup the incoming request and run given callback in worker's IO context.
  // callback should accept const Environment& parameter and return Promise<T>|void.
  // For void callbacks run waits for their completion, for promises waits for their resolution
  // and returns the result.
  template <typename Callback>
  auto runInIoContext(Callback&& callback)
      -> RunReturnType<decltype(callback(kj::instance<const Environment&>()))>::Type {
    auto request = newIncomingRequest();
    kj::WaitScope* waitScope;
    KJ_IF_SOME(ws, this->waitScope) {
      waitScope = &ws;
    } else {
      waitScope = &KJ_REQUIRE_NONNULL(io).waitScope;
    }

    auto& context = request->getContext();
    return context
        .run([&](Worker::Lock& lock) {
      // auto features = workerBundle.getFeatureFlags();
      auto& js = jsg::Lock::from(lock.getIsolate());
      Environment env = {{.isolate = lock.getIsolate()}, context, lock, js};
      KJ_ASSERT(env.isolate == v8::Isolate::TryGetCurrent());
      return callback(env);
    }).wait(*waitScope);
  }

  // Special void version of runInIoContext that ignores exceptions with given descriptions.
  void runInIoContext(kj::Function<kj::Promise<void>(const Environment&)>&& callback,
      kj::ArrayPtr<const kj::StringPtr> errorsToIgnore);

  struct Response {
    uint statusCode;
    kj::String body;
  };

  // Performs HTTP request on the default module handler, and waits for full response.
  Response runRequest(kj::HttpMethod method, kj::StringPtr url, kj::StringPtr body);

  // Create a new IoContext, owned by the caller. Use this when you need an IoContext that
  // outlives a single IncomingRequest, e.g. to model an actor receiving multiple requests.
  kj::Own<IoContext> newIoContext();

  // Create a new IncomingRequest bound to a fresh IoContext. The returned IncomingRequest is the
  // sole owner of that IoContext (via kj refcounting): destroying the IR destroys the IoContext.
  // If you need the IoContext to outlive the IR (e.g. to model multiple sequential or overlapping
  // IRs against one actor), call newIoContext() first and use the two-arg overload below. Use
  // enterContext() to run code within this context.
  kj::Own<IoContext::IncomingRequest> newIncomingRequest();

  // Create a new IncomingRequest bound to an existing IoContext. Use this to model multiple
  // IncomingRequests against the same actor (and hence the same IoContext). The IoContext must
  // outlive the returned IncomingRequest.
  kj::Own<IoContext::IncomingRequest> newIncomingRequest(IoContext& context);

  // Enter an IoContext. Callback receives Environment& and must return void (NOT a
  // Promise — the Worker::Lock is only valid for the synchronous duration of the
  // callback). The context is NOT destroyed afterwards — the caller still owns the
  // IncomingRequest.
  template <typename Callback>
  void enterContext(IoContext::IncomingRequest& request, Callback&& callback) {
    auto& context = request.getContext();
    context
        .run([&](Worker::Lock& lock) {
      auto& js = jsg::Lock::from(lock.getIsolate());
      Environment env = {{.isolate = lock.getIsolate()}, context, lock, js};
      callback(env);
    }).wait(getWaitScope());
  }

  // Acquire a Worker::Lock without an IoContext. Useful for operations that need
  // the V8 isolate lock but not a request context (e.g., hibernateWebSockets).
  template <typename Callback>
  void enterWorkerLock(Callback&& callback) {
    auto asyncLock = worker->takeAsyncLockWithoutRequest(nullptr).wait(getWaitScope());
    worker->runInLockScope(asyncLock, [&](Worker::Lock& lock) { callback(lock); });
  }

  kj::WaitScope& getWaitScope() {
    KJ_IF_SOME(ws, waitScope) {
      return ws;
    } else {
      return KJ_REQUIRE_NONNULL(io).waitScope;
    }
  }

  // Drive the event loop for a duration. Useful when test progress depends on a real timer
  // firing. For tests that just need pending work to run, prefer pollEventLoop() — it's
  // deterministic and faster.
  //
  // Requires SetupParams::useRealTimers = true; will fail at runtime otherwise because the
  // provider's timer is only set up when real timers are enabled.
  void pumpEventLoop(kj::Duration duration) {
    KJ_REQUIRE_NONNULL(io).provider->getTimer().afterDelay(duration).wait(getWaitScope());
  }

  // Run any work pending on the event loop until idle (no blocking). Returns the number of
  // events processed. Use this to deterministically drive background tasks (e.g. HM's
  // readLoop) to a stable point.
  uint pollEventLoop() {
    return getWaitScope().poll();
  }

  // Drain an IncomingRequest (waiting on its waitUntil tasks) and then destroy it. Tests should
  // use this rather than letting the IncomingRequest's Own go out of scope, otherwise the
  // IncomingRequest destructor logs a warning about un-drained waitUntil tasks. Production code
  // paths always drain.
  //
  // For actor IncomingRequests, drain() returns when all waitUntil tasks are empty, the actor is
  // shut down, or a new IncomingRequest takes over. In tests the second is unlikely so we mostly
  // rely on the first.
  void drainAndDestroy(kj::Own<IoContext::IncomingRequest> request) {
    request->drain(waitUntilTasks, kj::mv(request));
    waitUntilTasks.onEmpty().wait(getWaitScope());
  }

  // Accessors for tests that want to construct objects (e.g. HibernationManagerImpl) outside any
  // IoContext, to keep their construction paths free of ambient state. Production usually
  // constructs such objects lazily inside an IoContext just because the trigger (a JS handler)
  // happens to run there, but the constructors themselves don't need one.
  Worker::Actor& getActor() {
    return *KJ_ASSERT_NONNULL(actor);
  }
  TimerChannel& getTimerChannel() {
    return *timerChannel;
  }

  // Destroy the current Worker::Actor and construct a fresh one with the same id and Loopback.
  // Useful for simulating actor eviction: after this call, getActor() returns a different Actor
  // with a fresh InputGate / OutputGate, so a new IoContext can be constructed against it. The
  // previous IoContext (and any IncomingRequests still tied to it) MUST be torn down via
  // drainAndDestroy before calling this; otherwise the old IoContext's non-owning Actor reference
  // becomes dangling.
  //
  // Production has the actor's owning namespace pull the HibernationManager off the dying actor
  // and pass it to the new actor's constructor (see Server's actor namespace handling). Tests
  // here typically hold the HM directly and don't need to plumb it through the actor — the HM
  // outlives the actor by virtue of the test holding it.
  void resetActor();

 private:
  kj::Maybe<kj::WaitScope&> waitScope;
  capnp::MallocMessageBuilder configArena;
  workerd::server::config::Worker::Reader config;
  kj::Maybe<kj::AsyncIoContext> io;
  capnp::MallocMessageBuilder workerBundleArena;
  kj::Own<kj::Timer> timer;
  kj::Own<TimerChannel> timerChannel;
  kj::Own<kj::EntropySource> entropySource;
  kj::Maybe<kj::Own<Worker::Actor>> actor;
  // Saved so resetActor() can construct a new actor with the same Loopback (mirroring production,
  // where the namespace's Loopback outlives any single actor instance). Held via addRef so we
  // can hand fresh refs to actors as we reconstruct them.
  kj::Maybe<kj::Own<Worker::Actor::Loopback>> savedActorLoopback;
  capnp::ByteStreamFactory byteStreamFactory;
  kj::HttpHeaderTable::Builder headerTableBuilder;
  ThreadContext::HeaderIdBundle threadContextHeaderBundle;
  capnp::HttpOverCapnpFactory httpOverCapnpFactory;
  ThreadContext threadContext;
  kj::Own<Worker::ValidationErrorReporter> errorReporter;
  kj::Own<api::MemoryCacheProvider> memoryCacheProvider;
  v8::IsolateGroup isolateGroup;
  kj::Own<Worker::Api> api;
  kj::Own<Worker::Isolate> workerIsolate;
  kj::Own<Worker::Script> workerScript;
  kj::Own<Worker> worker;
  kj::Own<kj::TaskSet::ErrorHandler> errorHandler;
  kj::TaskSet waitUntilTasks;
  kj::Own<kj::HttpHeaderTable> headerTable;
  kj::Maybe<kj::Function<kj::Own<IoChannelFactory>(TimerChannel&)>> ioChannelFactory;
  kj::Maybe<kj::Function<kj::Own<RequestObserver>()>> requestObserverFactory;

  // Construct a fresh Worker::Actor with the given id, using the saved Loopback.
  kj::Own<Worker::Actor> makeActor(Worker::Actor::Id id);

 public:
  // Default IoChannelFactory used by tests. Exposed so tests can subclass it
  // and override individual methods (e.g. startSubrequest for socket connect tests).
  struct DummyIoChannelFactory: public IoChannelFactory {
    virtual ~DummyIoChannelFactory() = default;
    DummyIoChannelFactory(TimerChannel& timer): timer(timer) {}

    void abortIsolate(kj::StringPtr reason) override {
      KJ_FAIL_ASSERT("no abortIsolate");
    }

    kj::Own<WorkerInterface> startSubrequest(uint channel, SubrequestMetadata metadata) override {
      KJ_FAIL_ASSERT("no subrequests");
    }
    kj::Own<SubrequestChannel> getSubrequestChannelResolved(uint channel,
        kj::Maybe<Frankenvalue> props,
        kj::Maybe<VersionRequest> versionRequest) override {
      KJ_FAIL_ASSERT("no subrequests");
    }
    kj::Own<ActorClassChannel> getActorClassResolved(
        uint channel, kj::Maybe<Frankenvalue> props) override {
      KJ_FAIL_ASSERT("no actor classes");
    }
    capnp::Capability::Client getCapability(uint channel) override {
      KJ_FAIL_ASSERT("no capabilities");
    }
    // Out-of-line because it references file-local MockCacheClient in test-fixture.c++.
    kj::Own<CacheClient> getCache() override;
    TimerChannel& getTimer() override {
      return timer;
    }
    kj::Promise<void> writeLogfwdr(
        uint channel, kj::FunctionParam<void(capnp::AnyPointer::Builder)> buildMessage) override {
      KJ_FAIL_ASSERT("no log channels");
    }
    kj::Own<ActorChannel> getGlobalActor(uint channel,
        const ActorIdFactory::ActorId& id,
        kj::Maybe<kj::String> locationHint,
        ActorGetMode mode,
        bool enableReplicaRouting,
        ActorRoutingMode routingMode,
        SpanParent parentSpan,
        kj::Maybe<ActorVersion> version) override {
      KJ_FAIL_REQUIRE("no actor channels");
    }
    kj::Own<ActorChannel> getColoLocalActor(
        uint channel, kj::StringPtr id, SpanParent parentSpan) override {
      KJ_FAIL_REQUIRE("no actor channels");
    }

    kj::Own<void> addRef() override {
      KJ_FAIL_REQUIRE("not used");
    }

    TimerChannel& timer;
  };
};

}  // namespace workerd
