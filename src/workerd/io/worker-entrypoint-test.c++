// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-entrypoint.h"

#include <workerd/io/legacy-hibernation-manager.h>
#include <workerd/jsg/util.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/autogate.h>
#include <workerd/util/exception.h>

#include <kj/async-io.h>
#include <kj/sticky-flag.h>
#include <kj/test.h>

namespace workerd {
namespace {

class FailingIoChannelFactory final: public TestFixture::DummyIoChannelFactory {
 public:
  FailingIoChannelFactory(TimerChannel& timer): DummyIoChannelFactory(timer) {}

  kj::Own<WorkerInterface> startSubrequest(uint channel, SubrequestMetadata metadata) override {
    return WorkerInterface::fromException(KJ_EXCEPTION(FAILED, "connect failed"));
  }
};

class ThrowingIoChannelFactory final: public TestFixture::DummyIoChannelFactory {
 public:
  ThrowingIoChannelFactory(TimerChannel& timer): DummyIoChannelFactory(timer) {}

  kj::Own<WorkerInterface> startSubrequest(uint channel, SubrequestMetadata metadata) override {
    kj::throwException(KJ_EXCEPTION(FAILED, "channel creation failed"));
  }
};

class RejectingTimerChannel final: public TimerChannel {
 public:
  void syncTime() override {}

  kj::Date now(kj::Maybe<kj::Date>) override {
    return kj::UNIX_EPOCH;
  }

  kj::Promise<void> atTime(kj::Date) override {
    return KJ_EXCEPTION(FAILED, "alarm scheduling failed");
  }

  kj::Promise<void> afterLimitTimeout(kj::Duration) override {
    return kj::NEVER_DONE;
  }
};

class TestConnectResponse final: public kj::HttpService::ConnectResponse {
 public:
  void accept(uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers) override {
    KJ_FAIL_ASSERT("connect unexpectedly succeeded");
  }

  kj::Own<kj::AsyncOutputStream> reject(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    KJ_FAIL_ASSERT("connect unexpectedly returned an HTTP error");
  }
};

class ThrowingConnectResponse final: public kj::HttpService::ConnectResponse {
 public:
  void accept(uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers) override {
    KJ_FAIL_ASSERT("connect unexpectedly succeeded");
  }

  kj::Own<kj::AsyncOutputStream> reject(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    kj::throwException(KJ_EXCEPTION(FAILED, "connect rejection failed"));
  }
};

class TestResponse final: public kj::HttpService::Response {
 public:
  explicit TestResponse(kj::Own<kj::WebSocket> webSocket = {}): webSocket(kj::mv(webSocket)) {}

  kj::Own<kj::AsyncOutputStream> send(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    this->statusCode = statusCode;
    return kj::heap<kj::NullStream>();
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    KJ_REQUIRE(webSocket.get() != nullptr, "request unexpectedly returned a WebSocket");
    statusCode = 101;
    return kj::mv(webSocket);
  }

  uint statusCode = 0;

 private:
  kj::Own<kj::WebSocket> webSocket;
};

// Actor scripts in these tests append to `globalThis.events` from their constructor and handler.
constexpr kj::StringPtr RECORDING_ACTOR_SOURCE = R"SCRIPT(
  import { DurableObject } from "cloudflare:workers";
  globalThis.events = "";
  export default class extends DurableObject {
    constructor(ctx, env) {
      super(ctx, env);
      globalThis.events += "constructor;";
    }
    async fetch() {
      globalThis.events += "fetch;";
      return new Response("OK");
    }
  }
)SCRIPT"_kj;

kj::String getJsEvents(jsg::Lock& js) {
  return js.withinHandleScope([&]() { return js.global().get(js, "events"_kj).toString(js); });
}

kj::String getJsEvents(TestFixture& fixture) {
  kj::String events;
  fixture.enterWorkerLock([&](Worker::Lock& lock) {
    jsg::Lock& js = lock;
    js.withinHandleScope([&]() {
      v8::Context::Scope contextScope(lock.getContext());
      events = getJsEvents(js);
    });
  });
  return events;
}

// Records whether the claim follows delivery, and the script's events when the claim fires.
class RetryClaimObserver final: public RequestObserver {
 public:
  void delivered() override {
    ++deliveredCount;
  }

  void claimRetryTokenBeforeUserCode(IsRetryableHandler retryable) override {
    ++claimCount;
    KJ_EXPECT(deliveredCount == 1);
    retryableAtClaim = retryable;
    jsEventsAtClaim = getJsEvents(jsg::Lock::current());
    KJ_IF_SOME(e, rejection) {
      kj::throwException(e.clone());
    }
  }

  uint deliveredCount = 0;
  uint claimCount = 0;
  IsRetryableHandler retryableAtClaim = IsRetryableHandler::NO;
  kj::String jsEventsAtClaim;
  kj::Maybe<kj::Exception> rejection;
};

kj::Exception makeClaimRejection() {
  auto exception = KJ_EXCEPTION(FAILED, "claim rejected");
  exception.setDetail(jsg::ACTOR_RETRY_CLAIM_REJECTED_DETAIL_ID, kj::heapArray<kj::byte>(0));
  return exception;
}

kj::Exception makePredecessorRejection() {
  auto exception = KJ_EXCEPTION(DISCONNECTED, "request rejected before user code");
  jsg::markActorRequestNotDelivered(exception);
  exception.setDetail(jsg::ACTOR_PREDECESSOR_REJECTED_DETAIL_ID, kj::heapArray<kj::byte>(0));
  return exception;
}

class ThrowingResponse final: public kj::HttpService::Response {
 public:
  kj::Own<kj::AsyncOutputStream> send(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    kj::throwException(KJ_EXCEPTION(FAILED, "response send failed"));
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    KJ_FAIL_ASSERT("request unexpectedly returned a WebSocket");
  }
};

class FailingCustomEvent final: public WorkerInterface::CustomEvent {
 public:
  kj::Promise<Result> run(kj::Own<IoContext_IncomingRequest> incomingRequest,
      kj::Maybe<kj::StringPtr> entrypointName,
      kj::Maybe<Worker::VersionInfo> versionInfo,
      Frankenvalue props,
      kj::TaskSet& waitUntilTasks,
      bool isDynamicDispatch) override {
    incomingRequest->delivered();
    incomingRequest->drain(waitUntilTasks, kj::mv(incomingRequest));
    kj::throwException(KJ_EXCEPTION(FAILED, "custom event failed"));
  }

  kj::Promise<Result> sendRpc(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
      capnp::ByteStreamFactory& byteStreamFactory,
      FrankenvalueHandler& frankenvalueHandler,
      rpc::EventDispatcher::Client dispatcher) override {
    KJ_UNIMPLEMENTED();
  }

  kj::Promise<Result> notSupported() override {
    KJ_UNIMPLEMENTED();
  }

  uint16_t getType() override {
    return 123;
  }

  tracing::EventInfo getEventInfo() const override {
    return tracing::CustomEventInfo();
  }
};

class RecordingObserver final: public RequestObserver, public WorkerInterface {
 public:
  WorkerInterface& wrapWorkerInterface(WorkerInterface& worker) override {
    inner = worker;
    return *this;
  }

  void reportFailure(const kj::Exception& exception, FailureSource source) override {
    ++failureCount;
    if (outcome == kj::none) {
      outcome = outcomeFromException(exception, source);
    }
  }

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    try {
      co_await KJ_ASSERT_NONNULL(inner).request(method, url, headers, requestBody, response);
    } catch (...) {
      auto exception = kj::getCaughtExceptionAsKj();
      reportFailure(exception, FailureSource::OTHER);
      kj::throwException(kj::mv(exception));
    }
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    KJ_UNIMPLEMENTED();
  }

  kj::Promise<void> prewarm(kj::StringPtr url) override {
    KJ_UNIMPLEMENTED();
  }

  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    KJ_UNIMPLEMENTED();
  }

  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    KJ_UNIMPLEMENTED();
  }

  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    KJ_UNIMPLEMENTED();
  }

  kj::Maybe<kj::Own<WebSocketObserver>> tryCreateWebSocketObserver() override {
    // Coupling a locally accepted WebSocket asks for its observer while the request's IoContext is
    // current.
    coupledContext = IoContext::current();
    return kj::none;
  }

  kj::Maybe<EventOutcome> outcome;

  uint failureCount = 0;

  // The IoContext of the request that coupled a WebSocket. Valid only while that request is pending.
  kj::Maybe<IoContext&> coupledContext;

 private:
  kj::Maybe<WorkerInterface&> inner;
};

TestFixture::SetupParams recordingActorParams(RetryClaimObserver& observer) {
  return {
    .mainModuleSource = RECORDING_ACTOR_SOURCE,
    .actorId = Worker::Actor::Id(kj::str("retry-claim-test")),
    .actorClassName = "default"_kj,
    .requestObserverFactory = kj::Function<kj::Own<RequestObserver>()>(
        [&observer]() -> kj::Own<RequestObserver> { return kj::addRef(observer); }),
  };
}

kj::Maybe<kj::Exception> sendFetch(TestFixture& fixture, TestResponse& response) {
  auto entrypoint = fixture.makeWorkerEntrypoint();
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);
  kj::NullStream requestBody;
  return kj::runCatchingExceptions([&]() {
    entrypoint->request(kj::HttpMethod::GET, "https://example.com", headers, requestBody, response)
        .wait(fixture.getWaitScope());
  });
}

// Accepts the server end of a WebSocketPair, records its error and close events, and returns the
// client end in a 101. With an X-Close header, the server end also starts closing immediately. A
// "throw" message makes its message listener throw. "throw-retryable" throws a retryable Error,
// which JSG converts to DISCONNECTED, and makes the close listener close the server end.
constexpr kj::StringPtr LOCAL_WEB_SOCKET_SOURCE = R"(
  globalThis.events = "";
  export default {
    fetch(request) {
      const [client, server] = Object.values(new WebSocketPair());
      server.accept();
      let closeOnClose = false;
      server.addEventListener("error", () => {
        globalThis.events += "error;";
      });
      server.addEventListener("close", (event) => {
        globalThis.events += `close:${event.code}:${event.wasClean};`;
        if (closeOnClose) {
          server.close();
        }
      });
      server.addEventListener("message", (event) => {
        if (event.data === "throw") {
          throw new Error("listener failed");
        } else if (event.data === "throw-retryable") {
          closeOnClose = true;
          const error = new Error("listener failed");
          error.retryable = true;
          throw error;
        }
      });
      if (request.headers.get("X-Close") !== null) {
        server.close(1000, "done");
      }
      return new Response(null, { status: 101, webSocket: client });
    }
  };
)"_kj;

// Whether the Worker closes its end of the WebSocket as soon as it accepts it.
enum class WorkerClose { NONE, ON_ACCEPT };

// Whether the Worker runs statelessly or as a Durable Object, which has an output gate.
enum class WorkerHost { STATELESS, ACTOR };

// An upgrade request to LOCAL_WEB_SOCKET_SOURCE whose response accepts `transport`. The request
// is started and the event loop polled once on construction.
class LocalWebSocketRequest {
 public:
  explicit LocalWebSocketRequest(kj::Own<kj::WebSocket> transport,
      WorkerClose workerClose = WorkerClose::NONE,
      WorkerHost host = WorkerHost::STATELESS)
      : fixture(TestFixture::SetupParams{
          .mainModuleSource = LOCAL_WEB_SOCKET_SOURCE,
          .actorId = host == WorkerHost::ACTOR
              ? kj::Maybe<Worker::Actor::Id>(kj::str("local-web-socket-test"))
              : kj::none,
          .useRealTimers = false,
          .requestObserverFactory = kj::Function<kj::Own<RequestObserver>()>(
              [this]() -> kj::Own<RequestObserver> { return observer.addRef().toOwn(); }),
        }),
        entrypoint(fixture.makeWorkerEntrypoint()),
        headers(headerTable),
        response(kj::mv(transport)) {
    headers.setPtr(kj::HttpHeaderId::UPGRADE, "websocket");
    if (workerClose == WorkerClose::ON_ACCEPT) {
      headers.addPtrPtr("X-Close", "1");
    }
    request =
        entrypoint
            ->request(kj::HttpMethod::GET, "https://example.com", headers, requestBody, response)
            .eagerlyEvaluate(nullptr);
    fixture.pollEventLoop();
  }

  // Waits for the request to complete, then lets its wait-until tasks run.
  void finish() {
    request.wait(fixture.getWaitScope());
    fixture.pollEventLoop();
  }

  EventOutcome outcome() {
    return observer->outcome.orDefault(EventOutcome::OK);
  }

  kj::Rc<RecordingObserver> observer = kj::rc<RecordingObserver>();
  TestFixture fixture;
  kj::Own<WorkerInterface> entrypoint;
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers;
  kj::NullStream requestBody;
  TestResponse response;
  kj::Promise<void> request = nullptr;
};

// A transport whose close() completes when the test settles `closeResult`, and which never
// receives a message.
class ControlledWebSocket final: public kj::WebSocket {
 public:
  explicit ControlledWebSocket(bool& destroyed): destroyed(destroyed) {}
  ~ControlledWebSocket() noexcept(false) {
    destroyed = true;
  }

  kj::Promise<void> send(kj::ArrayPtr<const kj::byte> message) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> send(kj::ArrayPtr<const char> message) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> close(uint16_t code, kj::StringPtr reason) override {
    return closeResult.whenSignaled();
  }
  void disconnect() override {}
  void abort() override {}
  kj::Promise<void> whenAborted() override {
    return kj::NEVER_DONE;
  }
  kj::Promise<Message> receive(size_t maxSize) override {
    return kj::NEVER_DONE;
  }
  uint64_t sentByteCount() override {
    return 0;
  }
  uint64_t receivedByteCount() override {
    return 0;
  }
  kj::Maybe<kj::String> getPreferredExtensions(ExtensionsContext ctx) override {
    return kj::none;
  }

  kj::StickyFlag closeResult;

 private:
  bool& destroyed;
};

// A Durable Object that accepts the server end of a WebSocketPair for hibernation, records close
// events, and echoes the Close.
constexpr kj::StringPtr HIBERNATABLE_WEB_SOCKET_SOURCE = R"SCRIPT(
  import { DurableObject } from "cloudflare:workers";
  globalThis.events = "";
  export default class extends DurableObject {
    fetch() {
      const [client, server] = Object.values(new WebSocketPair());
      this.ctx.acceptWebSocket(server);
      return new Response(null, { status: 101, webSocket: client });
    }
    webSocketClose(ws, code, reason, wasClean) {
      globalThis.events += `close:${code}:${wasClean};`;
      ws.close(code, reason);
    }
  }
)SCRIPT"_kj;

// Delivers hibernation events to the fixture's own actor, as production's loopback does.
class FixtureLoopback final: public Worker::Actor::Loopback, public kj::Refcounted {
 public:
  kj::Own<WorkerInterface> getWorker(IoChannelFactory::SubrequestMetadata metadata) override {
    return KJ_ASSERT_NONNULL(fixture).makeWorkerEntrypoint();
  }

  kj::Own<Worker::Actor::Loopback> addRef() override {
    return kj::addRef(*this);
  }

  kj::Maybe<TestFixture&> fixture;
};

KJ_TEST("actor fetch claims after construction and before the fetch handler") {
  auto observer = kj::refcounted<RetryClaimObserver>();
  TestFixture fixture(recordingActorParams(*observer));
  TestResponse response;

  KJ_EXPECT(sendFetch(fixture, response) == kj::none);

  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(observer->claimCount == 1);
  KJ_EXPECT(observer->retryableAtClaim == IsRetryableHandler::NO);
  KJ_EXPECT(observer->jsEventsAtClaim == "constructor;", observer->jsEventsAtClaim);
  KJ_EXPECT(getJsEvents(fixture) == "constructor;fetch;");
}

constexpr kj::StringPtr RETRYABLE_FETCH_ACTOR_SOURCE = R"SCRIPT(
  import { DurableObject } from "cloudflare:workers";
  import { retryable } from "cloudflare:durable-objects";
  class Actor extends DurableObject {
    static {
      retryable(this.prototype.fetch);
    }
    async fetch() {
      return new Response("OK");
    }
  }
  export default Actor;
)SCRIPT"_kj;

IsRetryableHandler claimRetryableForMarkedFetch(kj::ArrayPtr<const kj::StringPtr> autogates) {
  auto observer = kj::refcounted<RetryClaimObserver>();
  auto params = recordingActorParams(*observer);
  params.mainModuleSource = RETRYABLE_FETCH_ACTOR_SOURCE;
  TestFixture fixture(kj::mv(params));
  // Set after the fixture, which initializes autogates, and ignore the @all-autogates variant so the
  // disabled case stays disabled.
  util::Autogate::initAutogateNamesForTest(autogates, util::IgnoreAllAutogatesEnv::YES);
  TestResponse response;

  KJ_EXPECT(sendFetch(fixture, response) == kj::none);

  KJ_EXPECT(response.statusCode == 200);
  KJ_EXPECT(observer->claimCount == 1);
  return observer->retryableAtClaim;
}

KJ_TEST("a retryable() fetch claims as retryable when the userland gate is enabled") {
  auto gates = kj::arr("durable-object-retries-userland"_kj);
  KJ_EXPECT(claimRetryableForMarkedFetch(gates) == IsRetryableHandler::YES);
}

KJ_TEST("a retryable() fetch claims as not retryable when the userland gate is disabled") {
  KJ_EXPECT(claimRetryableForMarkedFetch(nullptr) == IsRetryableHandler::NO);
}

KJ_TEST("a rejected actor fetch claim runs the constructor but not the handler") {
  auto observer = kj::refcounted<RetryClaimObserver>();
  observer->rejection = makeClaimRejection();
  TestFixture fixture(recordingActorParams(*observer));
  TestResponse response;

  auto e = KJ_ASSERT_NONNULL(sendFetch(fixture, response));

  KJ_EXPECT(e.getDetail(jsg::ACTOR_RETRY_CLAIM_REJECTED_DETAIL_ID) != kj::none, e);
  KJ_EXPECT(response.statusCode == 0);
  KJ_EXPECT(observer->claimCount == 1);
  KJ_EXPECT(getJsEvents(fixture) == "constructor;");
}

KJ_TEST("a rejected actor fetch claim is not replaced by an output gate failure") {
  auto observer = kj::refcounted<RetryClaimObserver>();
  observer->rejection = makeClaimRejection();
  TestFixture fixture(recordingActorParams(*observer));
  auto gatePaf = kj::newPromiseAndFulfiller<void>();
  auto blocker = fixture.getActor().getOutputGate().lockWhile(kj::mv(gatePaf.promise), nullptr);
  TestResponse response;
  auto entrypoint = fixture.makeWorkerEntrypoint();
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);
  kj::NullStream requestBody;
  auto request =
      entrypoint->request(
                    kj::HttpMethod::GET, "https://example.com", headers, requestBody, response)
          .eagerlyEvaluate(nullptr);

  fixture.pollEventLoop();
  gatePaf.fulfiller->reject(KJ_EXCEPTION(OVERLOADED, "output gate failed"));

  auto e =
      KJ_ASSERT_NONNULL(kj::runCatchingExceptions([&]() { request.wait(fixture.getWaitScope()); }));
  KJ_EXPECT(e.getDetail(jsg::ACTOR_RETRY_CLAIM_REJECTED_DETAIL_ID) != kj::none, e);
  KJ_EXPECT_THROW_MESSAGE("output gate failed", blocker.wait(fixture.getWaitScope()));
}

KJ_TEST("actor fetch preserves not-delivered for a predecessor rejection") {
  // The caller retries a predecessor rejection against the replacement actor, but only while it is
  // marked not delivered. Failures after delivered() are otherwise marked delivered, so this one
  // must be exempt.
  auto observer = kj::refcounted<RetryClaimObserver>();
  observer->rejection = makePredecessorRejection();
  TestFixture fixture(recordingActorParams(*observer));
  TestResponse response;

  auto e = KJ_ASSERT_NONNULL(sendFetch(fixture, response));

  KJ_EXPECT(e.getType() == kj::Exception::Type::DISCONNECTED, e);
  KJ_EXPECT(e.getDetail(jsg::REQUEST_NOT_DELIVERED_TO_ACTOR_DETAIL_ID) != kj::none, e);
  KJ_EXPECT(e.getDetail(jsg::REQUEST_DELIVERED_TO_ACTOR_DETAIL_ID) == kj::none, e);
  KJ_EXPECT(e.getDetail(jsg::ACTOR_PREDECESSOR_REJECTED_DETAIL_ID) != kj::none, e);
  KJ_EXPECT(getJsEvents(fixture) == "constructor;");
}

KJ_TEST("actor fetch marks other not-delivered rejections from the claim as delivered") {
  // The predecessor exemption is keyed on its own detail. By the time of the claim the constructor
  // may have run, so any other not-delivered failure is marked delivered and not retried.
  auto observer = kj::refcounted<RetryClaimObserver>();
  auto rejection = KJ_EXCEPTION(DISCONNECTED, "request rejected");
  jsg::markActorRequestNotDelivered(rejection);
  observer->rejection = kj::mv(rejection);
  TestFixture fixture(recordingActorParams(*observer));
  TestResponse response;

  auto e = KJ_ASSERT_NONNULL(sendFetch(fixture, response));

  KJ_EXPECT(e.getDetail(jsg::REQUEST_NOT_DELIVERED_TO_ACTOR_DETAIL_ID) == kj::none, e);
  KJ_EXPECT(e.getDetail(jsg::REQUEST_DELIVERED_TO_ACTOR_DETAIL_ID) != kj::none, e);
}

KJ_TEST("actor fetch marks retryable failures of any type as delivered") {
  // A retryable failure that passes through the actor must not make its caller retry the actor.
  auto observer = kj::refcounted<RetryClaimObserver>();
  auto rejection = KJ_EXCEPTION(FAILED, "jsg.Error: retryable failure");
  jsg::markTransient(rejection);
  jsg::markActorRequestNotDelivered(rejection);
  observer->rejection = kj::mv(rejection);
  TestFixture fixture(recordingActorParams(*observer));
  TestResponse response;

  auto e = KJ_ASSERT_NONNULL(sendFetch(fixture, response));

  KJ_EXPECT(e.getType() == kj::Exception::Type::FAILED, e);
  KJ_EXPECT(jsg::isRetryable(e), e);
  KJ_EXPECT(e.getDetail(jsg::REQUEST_NOT_DELIVERED_TO_ACTOR_DETAIL_ID) == kj::none, e);
  KJ_EXPECT(e.getDetail(jsg::REQUEST_DELIVERED_TO_ACTOR_DETAIL_ID) != kj::none, e);
}

// A Durable Object whose fetch handler never returns.
constexpr kj::StringPtr HANGING_FETCH_ACTOR_SOURCE = R"SCRIPT(
  import { DurableObject } from "cloudflare:workers";
  globalThis.events = "";
  export default class extends DurableObject {
    fetch() {
      globalThis.events += "fetch;";
      return new Promise(() => {});
    }
  }
)SCRIPT"_kj;

KJ_TEST("aborting an actor fails its in-flight events as delivered") {
  TestFixture fixture({
    .mainModuleSource = HANGING_FETCH_ACTOR_SOURCE,
    .actorId = Worker::Actor::Id(kj::str("abort-test")),
    .actorClassName = "default"_kj,
  });
  auto entrypoint = fixture.makeWorkerEntrypoint();
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);
  kj::NullStream requestBody;
  TestResponse response;
  auto request =
      entrypoint->request(
                    kj::HttpMethod::GET, "https://example.com", headers, requestBody, response)
          .eagerlyEvaluate(nullptr);
  fixture.getWaitScope().poll();
  KJ_EXPECT(getJsEvents(fixture) == "fetch;");

  auto reason = KJ_EXCEPTION(DISCONNECTED, "jsg.Error: superseded");
  // A position carried by the reason describes some other request, not the canceled one.
  jsg::markActorRequestNotDelivered(reason);
  fixture.getActor().abort(reason);

  auto e =
      KJ_ASSERT_NONNULL(kj::runCatchingExceptions([&]() { request.wait(fixture.getWaitScope()); }));
  KJ_EXPECT(e.getType() == kj::Exception::Type::DISCONNECTED, e);
  KJ_EXPECT(e.getDescription() == "jsg.Error: superseded", e);
  KJ_EXPECT(e.getDetail(jsg::REQUEST_DELIVERED_TO_ACTOR_DETAIL_ID) != kj::none, e);
  KJ_EXPECT(e.getDetail(jsg::REQUEST_NOT_DELIVERED_TO_ACTOR_DETAIL_ID) == kj::none, e);
}

KJ_TEST("fetch claims once even without a fetch handler") {
  // The claim precedes the handler lookup, so a worker without a fetch handler still claims, once,
  // before the request fails.
  auto observer = kj::refcounted<RetryClaimObserver>();
  TestFixture fixture(TestFixture::SetupParams{
    .mainModuleSource = "export default {};"_kj,
    .requestObserverFactory = kj::Function<kj::Own<RequestObserver>()>(
        [&observer]() -> kj::Own<RequestObserver> { return kj::addRef(*observer); }),
  });
  TestResponse response;

  KJ_EXPECT(sendFetch(fixture, response) == kj::none);

  KJ_EXPECT(response.statusCode == 500);
  KJ_EXPECT(observer->claimCount == 1);
}

KJ_TEST("connect pass-through tags failures after delivery") {
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setConnectPassThrough(true);

  TestFixture fixture(TestFixture::SetupParams{
    .featureFlags = flags.asReader(),
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<FailingIoChannelFactory>(timer);
  }),
  });
  auto entrypoint = fixture.makeWorkerEntrypoint();
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);
  kj::NullStream connection;
  TestConnectResponse response;

  auto exception = kj::runCatchingExceptions([&]() {
    entrypoint->connect("example.com", headers, connection, response, {})
        .wait(fixture.getWaitScope());
  });

  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_EXPECT(e.getDescription().contains("connect failed"));
  KJ_EXPECT(e.getDetail(WORKER_REQUEST_DELIVERED_DETAIL_ID) != kj::none);
}

KJ_TEST("connect pass-through tags channel creation failures after delivery") {
  capnp::MallocMessageBuilder flagsMessage;
  auto flags = flagsMessage.initRoot<CompatibilityFlags>();
  flags.setConnectPassThrough(true);

  TestFixture fixture(TestFixture::SetupParams{
    .featureFlags = flags.asReader(),
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [](TimerChannel& timer) -> kj::Rc<IoChannelFactory> {
    return kj::rc<ThrowingIoChannelFactory>(timer);
  }),
  });
  auto entrypoint = fixture.makeWorkerEntrypoint();
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);
  kj::NullStream connection;
  TestConnectResponse response;

  auto exception = kj::runCatchingExceptions([&]() {
    entrypoint->connect("example.com", headers, connection, response, {})
        .wait(fixture.getWaitScope());
  });

  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_EXPECT(e.getDescription().contains("channel creation failed"));
  KJ_EXPECT(e.getDetail(WORKER_REQUEST_DELIVERED_DETAIL_ID) != kj::none);
}

KJ_TEST("connect rejection failures stay tagged after delivery") {
  TestFixture fixture;
  auto entrypoint = fixture.makeWorkerEntrypoint();
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);
  kj::NullStream connection;
  ThrowingConnectResponse response;

  auto exception = kj::runCatchingExceptions([&]() {
    entrypoint->connect("example.com", headers, connection, response, {})
        .wait(fixture.getWaitScope());
  });

  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_EXPECT(e.getDescription().contains("connect rejection failed"));
  KJ_EXPECT(e.getDetail(WORKER_REQUEST_DELIVERED_DETAIL_ID) != kj::none);
}

// Fails an actor CONNECT with `gateFailure`. An output gate failure replaces the handler's, so it
// reaches the entrypoint's catch.
kj::Exception failActorConnectAtOutputGate(kj::Exception gateFailure) {
  TestFixture fixture(TestFixture::SetupParams{
    .mainModuleSource = RECORDING_ACTOR_SOURCE,
    .actorId = Worker::Actor::Id(kj::str("connect-test")),
    .actorClassName = "default"_kj,
  });
  auto gatePaf = kj::newPromiseAndFulfiller<void>();
  auto blocker = fixture.getActor().getOutputGate().lockWhile(kj::mv(gatePaf.promise), nullptr);
  auto entrypoint = fixture.makeWorkerEntrypoint();
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);
  kj::NullStream connection;
  TestConnectResponse response;
  auto connect = entrypoint->connect("example.com", headers, connection, response, {})
                     .eagerlyEvaluate(nullptr);

  fixture.pollEventLoop();
  gatePaf.fulfiller->reject(kj::mv(gateFailure));

  auto e =
      KJ_ASSERT_NONNULL(kj::runCatchingExceptions([&]() { connect.wait(fixture.getWaitScope()); }));
  KJ_EXPECT(kj::runCatchingExceptions([&]() { blocker.wait(fixture.getWaitScope()); }) != kj::none);
  return e;
}

KJ_TEST("actor connect marks a not-delivered disconnect as delivered") {
  auto gateFailure = KJ_EXCEPTION(DISCONNECTED, "output gate failed");
  jsg::markActorRequestNotDelivered(gateFailure);

  auto e = failActorConnectAtOutputGate(kj::mv(gateFailure));

  KJ_EXPECT(e.getDetail(jsg::REQUEST_NOT_DELIVERED_TO_ACTOR_DETAIL_ID) == kj::none, e);
  KJ_EXPECT(e.getDetail(jsg::REQUEST_DELIVERED_TO_ACTOR_DETAIL_ID) != kj::none, e);
}

KJ_TEST("actor connect marks retryable failures of any type as delivered") {
  auto gateFailure = KJ_EXCEPTION(FAILED, "output gate failed");
  jsg::markTransient(gateFailure);
  jsg::markActorRequestNotDelivered(gateFailure);

  auto e = failActorConnectAtOutputGate(kj::mv(gateFailure));

  KJ_EXPECT(e.getType() == kj::Exception::Type::FAILED, e);
  KJ_EXPECT(jsg::isRetryable(e), e);
  KJ_EXPECT(e.getDetail(jsg::REQUEST_NOT_DELIVERED_TO_ACTOR_DETAIL_ID) == kj::none, e);
  KJ_EXPECT(e.getDetail(jsg::REQUEST_DELIVERED_TO_ACTOR_DETAIL_ID) != kj::none, e);
}

KJ_TEST("actor connect preserves not-delivered for a predecessor rejection") {
  auto e = failActorConnectAtOutputGate(makePredecessorRejection());

  KJ_EXPECT(e.getDetail(jsg::REQUEST_NOT_DELIVERED_TO_ACTOR_DETAIL_ID) != kj::none, e);
  KJ_EXPECT(e.getDetail(jsg::REQUEST_DELIVERED_TO_ACTOR_DETAIL_ID) == kj::none, e);
  KJ_EXPECT(e.getDetail(jsg::ACTOR_PREDECESSOR_REJECTED_DETAIL_ID) != kj::none, e);
}

KJ_TEST("alarm scheduling tags failures after delivery") {
  RejectingTimerChannel timer;
  TestFixture fixture(TestFixture::SetupParams{
    .actorId = Worker::Actor::Id(kj::str("alarm-test")),
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Rc<IoChannelFactory>(TimerChannel&)>(
        [&timer](TimerChannel&) -> kj::Rc<IoChannelFactory> {
    return kj::rc<TestFixture::DummyIoChannelFactory>(timer);
  }),
  });
  auto scheduledTime = kj::UNIX_EPOCH + kj::SECONDS;
  auto entrypoint = fixture.makeWorkerEntrypoint();

  auto exception = kj::runCatchingExceptions(
      [&]() { entrypoint->runAlarm(scheduledTime, 0).wait(fixture.getWaitScope()); });

  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_EXPECT(e.getDescription().contains("alarm scheduling failed"));
  KJ_EXPECT(e.getDetail(WORKER_REQUEST_DELIVERED_DETAIL_ID) != kj::none);
}

KJ_TEST("custom events tag failures after delivery") {
  TestFixture fixture;
  auto entrypoint = fixture.makeWorkerEntrypoint();

  auto exception = kj::runCatchingExceptions([&]() {
    entrypoint->customEvent(kj::heap<FailingCustomEvent>()).wait(fixture.getWaitScope());
  });

  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_EXPECT(e.getDescription().contains("custom event failed"));
  KJ_EXPECT(e.getDetail(WORKER_REQUEST_DELIVERED_DETAIL_ID) != kj::none);
}

KJ_TEST("synthetic response failures stay tagged after delivery") {
  static constexpr auto source = R"(
    export default {
      fetch() { throw new Error("handler failed"); }
    };
  )"_kj;

  TestFixture fixture(TestFixture::SetupParams{.mainModuleSource = source});
  auto entrypoint = fixture.makeWorkerEntrypoint();
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);
  kj::NullStream requestBody;
  ThrowingResponse response;

  auto exception = kj::runCatchingExceptions([&]() {
    entrypoint->request(kj::HttpMethod::GET, "https://example.com", headers, requestBody, response)
        .wait(fixture.getWaitScope());
  });

  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_EXPECT(e.getDescription().contains("response send failed"));
  KJ_EXPECT(e.getDetail(WORKER_REQUEST_DELIVERED_DETAIL_ID) != kj::none);
}

KJ_TEST("local WebSocket clean close is reported as a stream disconnect") {
  auto pipe = kj::newTwoWayPipe();
  LocalWebSocketRequest test(
      kj::newWebSocket(kj::mv(pipe.ends[0]), kj::none), WorkerClose::ON_ACCEPT);
  auto peer = kj::newWebSocket(kj::mv(pipe.ends[1]), kj::none);
  KJ_ASSERT(test.response.statusCode == 101);

  // The Worker closes first and the peer replies. The session still ends with a disconnect, when
  // the Worker releases its end, as for a hibernatable WebSocket.
  auto message = peer->receive().wait(test.fixture.getWaitScope());
  KJ_ASSERT(message.is<kj::WebSocket::Close>());
  peer->close(1000, "done").wait(test.fixture.getWaitScope());
  test.finish();

  KJ_EXPECT(getJsEvents(test.fixture) == "close:1000:true;");
  KJ_EXPECT(test.outcome() == EventOutcome::RESPONSE_STREAM_DISCONNECTED);
}

KJ_TEST("hibernatable WebSocket clean close is reported as a stream disconnect") {
  // Only the upgrade request is recorded. RecordingObserver cannot wrap the custom events that
  // deliver hibernation events, and they are separate invocations with their own outcomes.
  auto observer = kj::rc<RecordingObserver>();
  bool recordRequests = true;
  auto loopback = kj::refcounted<FixtureLoopback>();
  TestFixture fixture(TestFixture::SetupParams{
    .mainModuleSource = HIBERNATABLE_WEB_SOCKET_SOURCE,
    .actorId = Worker::Actor::Id(kj::str("hibernatable-web-socket-test")),
    .actorClassName = "default"_kj,
    .useRealTimers = false,
    .actorLoopback = kj::addRef(*loopback),
    .requestObserverFactory =
        kj::Function<kj::Own<RequestObserver>()>([&]() -> kj::Own<RequestObserver> {
    if (recordRequests) {
      return observer.addRef().toOwn();
    }
    return kj::refcounted<RequestObserver>();
  }),
  });
  loopback->fixture = fixture;
  auto& actor = fixture.getActor();
  actor.setHibernationManager(
      kj::refcounted<LegacyHibernationManagerImpl>(actor.getLoopback(), /*eventType=*/0));

  auto entrypoint = fixture.makeWorkerEntrypoint();
  recordRequests = false;
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);
  headers.setPtr(kj::HttpHeaderId::UPGRADE, "websocket");
  kj::NullStream requestBody;
  auto pipe = kj::newTwoWayPipe();
  TestResponse response(kj::newWebSocket(kj::mv(pipe.ends[0]), kj::none));
  auto request =
      entrypoint->request(
                    kj::HttpMethod::GET, "https://example.com", headers, requestBody, response)
          .eagerlyEvaluate(nullptr);
  auto peer = kj::newWebSocket(kj::mv(pipe.ends[1]), kj::none);
  fixture.pollEventLoop();
  KJ_ASSERT(response.statusCode == 101);

  // The peer closes and the Durable Object echoes the Close from its webSocketClose() handler.
  peer->close(1000, "done").wait(fixture.getWaitScope());
  auto message = peer->receive().wait(fixture.getWaitScope());
  KJ_ASSERT(message.is<kj::WebSocket::Close>());

  // A Durable Object returns the failure to its caller.
  auto exception = kj::runCatchingExceptions([&]() { request.wait(fixture.getWaitScope()); });
  KJ_EXPECT(KJ_ASSERT_NONNULL(exception).getType() == kj::Exception::Type::DISCONNECTED);
  fixture.pollEventLoop();
  KJ_EXPECT(getJsEvents(fixture) == "close:1000:true;");
  KJ_EXPECT(
      observer->outcome.orDefault(EventOutcome::OK) == EventOutcome::RESPONSE_STREAM_DISCONNECTED);
}

KJ_TEST("local WebSocket disconnect is not a script exception") {
  auto pipe = kj::newTwoWayPipe();
  LocalWebSocketRequest test(kj::newWebSocket(kj::mv(pipe.ends[0]), kj::none));
  auto peer = kj::mv(pipe.ends[1]);
  KJ_ASSERT(test.response.statusCode == 101);
  KJ_EXPECT(!test.request.poll(test.fixture.getWaitScope()));

  // Drop the transport without a WebSocket Close frame, while keeping the request alive.
  peer = nullptr;
  test.finish();

  KJ_EXPECT(getJsEvents(test.fixture) == "close:1006:false;");
  KJ_EXPECT(test.outcome() == EventOutcome::RESPONSE_STREAM_DISCONNECTED);
}

KJ_TEST("local WebSocket listener exception is a script exception") {
  auto pipe = kj::newTwoWayPipe();
  LocalWebSocketRequest test(kj::newWebSocket(kj::mv(pipe.ends[0]), kj::none));
  auto peer = kj::newWebSocket(kj::mv(pipe.ends[1]), kj::none);
  KJ_ASSERT(test.response.statusCode == 101);
  KJ_ASSERT(!test.request.poll(test.fixture.getWaitScope()));

  // The listener's exception errors the Worker's WebSocket, which then releases its end of the
  // session. The coupled transport sees that as a disconnect, but the session failed in the Worker.
  peer->send("throw"_kj).wait(test.fixture.getWaitScope());
  test.finish();

  KJ_EXPECT(getJsEvents(test.fixture) == "error;");
  KJ_EXPECT(test.outcome() == EventOutcome::EXCEPTION);
}

KJ_TEST("local WebSocket retryable listener exception is a script exception") {
  auto pipe = kj::newTwoWayPipe();
  LocalWebSocketRequest test(kj::newWebSocket(kj::mv(pipe.ends[0]), kj::none));
  auto peer = kj::newWebSocket(kj::mv(pipe.ends[1]), kj::none);
  KJ_ASSERT(test.response.statusCode == 101);
  KJ_ASSERT(!test.request.poll(test.fixture.getWaitScope()));

  // The retryable exception reaches the read loop as DISCONNECTED, so JavaScript sees a 1006 close
  // even though the peer is still connected. The close listener then closes the Worker's end, which
  // the coupled transport again sees as a disconnect.
  auto peerReceive = peer->receive().eagerlyEvaluate(nullptr);
  peer->send("throw-retryable"_kj).wait(test.fixture.getWaitScope());
  test.finish();

  KJ_EXPECT(getJsEvents(test.fixture) == "close:1006:false;");
  KJ_EXPECT(test.outcome() == EventOutcome::EXCEPTION);
}

KJ_TEST("local WebSocket disconnect waits for pending output locks") {
  auto pipe = kj::newTwoWayPipe();
  LocalWebSocketRequest test(
      kj::newWebSocket(kj::mv(pipe.ends[0]), kj::none), WorkerClose::NONE, WorkerHost::ACTOR);
  auto peer = kj::mv(pipe.ends[1]);
  KJ_ASSERT(test.response.statusCode == 101);

  auto lock = kj::newPromiseAndFulfiller<void>();
  auto blocker = test.fixture.getActor().getOutputGate().lockWhile(kj::mv(lock.promise), nullptr);
  peer = nullptr;
  test.fixture.pollEventLoop();
  KJ_EXPECT(!test.request.poll(test.fixture.getWaitScope()));

  lock.fulfiller->fulfill();
  // A Durable Object returns the failure to its caller.
  auto exception =
      kj::runCatchingExceptions([&]() { test.request.wait(test.fixture.getWaitScope()); });
  KJ_EXPECT(KJ_ASSERT_NONNULL(exception).getType() == kj::Exception::Type::DISCONNECTED);
  KJ_EXPECT(test.outcome() == EventOutcome::RESPONSE_STREAM_DISCONNECTED);
  blocker.wait(test.fixture.getWaitScope());
}

KJ_TEST("output gate failure replaces a local WebSocket disconnect") {
  auto pipe = kj::newTwoWayPipe();
  LocalWebSocketRequest test(
      kj::newWebSocket(kj::mv(pipe.ends[0]), kj::none), WorkerClose::NONE, WorkerHost::ACTOR);
  auto peer = kj::mv(pipe.ends[1]);
  KJ_ASSERT(test.response.statusCode == 101);

  // Storage work started after the 101 holds an output lock when the peer disconnects.
  auto lock = kj::newPromiseAndFulfiller<void>();
  auto blocker = test.fixture.getActor().getOutputGate().lockWhile(kj::mv(lock.promise), nullptr);
  peer = nullptr;
  test.fixture.pollEventLoop();
  KJ_ASSERT(!test.request.poll(test.fixture.getWaitScope()));

  // The gate failure ends the request in place of the disconnect, so the request handler logs it.
  KJ_EXPECT_LOG(ERROR, "output gate failed");
  lock.fulfiller->reject(KJ_EXCEPTION(OVERLOADED, "output gate failed"));
  auto exception =
      kj::runCatchingExceptions([&]() { test.request.wait(test.fixture.getWaitScope()); });
  KJ_EXPECT(KJ_ASSERT_NONNULL(exception).getType() == kj::Exception::Type::OVERLOADED);
  KJ_EXPECT(test.outcome() == EventOutcome::INTERNAL_ERROR);
  KJ_EXPECT_THROW_MESSAGE("output gate failed", blocker.wait(test.fixture.getWaitScope()));
}

KJ_TEST("local WebSocket protocol error is reported as an exception") {
  auto pipe = kj::newTwoWayPipe();
  LocalWebSocketRequest test(kj::newWebSocket(kj::mv(pipe.ends[0]), kj::none));
  auto peer = kj::mv(pipe.ends[1]);
  KJ_ASSERT(test.response.statusCode == 101);
  KJ_EXPECT(!test.request.poll(test.fixture.getWaitScope()));

  // Consume the server's Close reply so that its write can complete.
  auto peerOutput = peer->readAllBytes().eagerlyEvaluate(nullptr);

  // The internal error is logged once when converted for the JavaScript error event, and once as
  // the request handler's uncaught exception.
  KJ_EXPECT_LOG(ERROR, "WebSocket protocol error");
  KJ_EXPECT_LOG(ERROR, "WebSocket protocol error");

  // A masked, empty text frame with the reserved RSV2 bit set violates the protocol.
  static constexpr kj::byte invalidFrame[] = {0xa1, 0x80, 0, 0, 0, 0};
  peer->write(kj::arrayPtr(invalidFrame)).wait(test.fixture.getWaitScope());
  test.finish();

  KJ_EXPECT(getJsEvents(test.fixture) == "error;");
  KJ_EXPECT(test.outcome() == EventOutcome::EXCEPTION);
}

KJ_TEST("local WebSocket transport failure ends the request without waiting for the peer") {
  bool transportDestroyed = false;
  auto transportOwn = kj::heap<ControlledWebSocket>(transportDestroyed);
  auto& transport = *transportOwn;
  LocalWebSocketRequest test(kj::mv(transportOwn), WorkerClose::ON_ACCEPT);
  KJ_ASSERT(test.response.statusCode == 101);

  // The Worker's Close reaches the transport, which fails to send it while the incoming pump is
  // still waiting for a message. The failure alone ends the session and releases the transport.
  KJ_EXPECT_LOG(ERROR, "transport close failed");
  KJ_EXPECT_LOG(ERROR, "transport close failed");
  transport.closeResult.reject(KJ_EXCEPTION(FAILED, "transport close failed"));
  test.fixture.pollEventLoop();
  KJ_ASSERT(test.request.poll(test.fixture.getWaitScope()));
  KJ_EXPECT(transportDestroyed);
  test.finish();

  KJ_EXPECT(test.outcome() == EventOutcome::EXCEPTION);
}

// When the peer transport is dropped relative to the abort. Both happen before the event loop
// runs again.
enum class PeerDrop { BEFORE_ABORT, AFTER_ABORT };

// Aborts the IoContext of a live coupled request and returns the request's outcome.
EventOutcome abortCoupledRequest(kj::Exception reason, PeerDrop peerDrop) {
  auto pipe = kj::newTwoWayPipe();
  LocalWebSocketRequest test(kj::newWebSocket(kj::mv(pipe.ends[0]), kj::none));
  auto peer = kj::mv(pipe.ends[1]);
  KJ_ASSERT(test.response.statusCode == 101);
  KJ_ASSERT(!test.request.poll(test.fixture.getWaitScope()));

  auto& context = KJ_ASSERT_NONNULL(test.observer->coupledContext);
  if (peerDrop == PeerDrop::BEFORE_ABORT) {
    peer = nullptr;
  }
  context.abort(kj::mv(reason));
  peer = nullptr;
  test.finish();
  return test.outcome();
}

kj::Exception cpuLimitException() {
  auto exception = KJ_EXCEPTION(OVERLOADED, "worker_do_not_log; script exceeded its CPU limit");
  exception.setDetail(CPU_LIMIT_DETAIL_ID, kj::heapArray<kj::byte>(0));
  return exception;
}

KJ_TEST("aborting a coupled local WebSocket request reports the abort") {
  KJ_EXPECT(abortCoupledRequest(KJ_EXCEPTION(DISCONNECTED, "test abort"), PeerDrop::AFTER_ABORT) ==
      EventOutcome::EXCEPTION);
  KJ_EXPECT(abortCoupledRequest(cpuLimitException(), PeerDrop::AFTER_ABORT) ==
      EventOutcome::EXCEEDED_CPU);
}

KJ_TEST("an abort pending with a peer disconnect reports the abort") {
  KJ_EXPECT(abortCoupledRequest(KJ_EXCEPTION(DISCONNECTED, "test abort"), PeerDrop::BEFORE_ABORT) ==
      EventOutcome::EXCEPTION);
  KJ_EXPECT(abortCoupledRequest(cpuLimitException(), PeerDrop::BEFORE_ABORT) ==
      EventOutcome::EXCEEDED_CPU);
}

// When an actor's output lock is released relative to an abort of its coupled request, which is
// waiting for that lock after a peer disconnect. Both happen before the event loop runs again.
enum class LockRelease { BEFORE_ABORT, AFTER_ABORT };

// Aborts a coupled Durable Object request while its disconnect waits for an output lock, and
// returns the request's outcome.
EventOutcome abortWhileAwaitingOutputLock(kj::Exception reason, LockRelease lockRelease) {
  auto pipe = kj::newTwoWayPipe();
  LocalWebSocketRequest test(
      kj::newWebSocket(kj::mv(pipe.ends[0]), kj::none), WorkerClose::NONE, WorkerHost::ACTOR);
  auto peer = kj::mv(pipe.ends[1]);
  KJ_ASSERT(test.response.statusCode == 101);

  auto lock = kj::newPromiseAndFulfiller<void>();
  auto blocker = test.fixture.getActor().getOutputGate().lockWhile(kj::mv(lock.promise), nullptr);
  peer = nullptr;
  test.fixture.pollEventLoop();
  KJ_ASSERT(!test.request.poll(test.fixture.getWaitScope()));

  auto& context = KJ_ASSERT_NONNULL(test.observer->coupledContext);
  if (lockRelease == LockRelease::BEFORE_ABORT) {
    lock.fulfiller->fulfill();
    context.abort(kj::mv(reason));
  } else {
    context.abort(kj::mv(reason));
    lock.fulfiller->fulfill();
  }

  // A Durable Object returns the failure to its caller.
  KJ_EXPECT(kj::runCatchingExceptions([&]() { test.request.wait(test.fixture.getWaitScope()); }) !=
      kj::none);
  blocker.wait(test.fixture.getWaitScope());
  return test.outcome();
}

KJ_TEST("an abort while a coupled local WebSocket waits for output locks reports the abort") {
  for (auto lockRelease: {LockRelease::BEFORE_ABORT, LockRelease::AFTER_ABORT}) {
    KJ_EXPECT(abortWhileAwaitingOutputLock(KJ_EXCEPTION(DISCONNECTED, "test abort"), lockRelease) ==
        EventOutcome::EXCEPTION);
    KJ_EXPECT(abortWhileAwaitingOutputLock(cpuLimitException(), lockRelease) ==
        EventOutcome::EXCEEDED_CPU);
  }
}

KJ_TEST("output gate failure replaces an earlier handler failure") {
  static constexpr auto source = R"(
    export default {
      fetch() { throw new Error("handler failed"); }
    };
  )"_kj;

  auto observer = kj::refcounted<RecordingObserver>();
  TestFixture fixture(TestFixture::SetupParams{
    .mainModuleSource = source,
    .actorId = Worker::Actor::Id(kj::str("output-gate-test")),
    .useRealTimers = false,
    .requestObserverFactory = kj::Function<kj::Own<RequestObserver>()>(
        [&observer]() -> kj::Own<RequestObserver> { return kj::addRef(*observer); }),
  });

  auto gatePaf = kj::newPromiseAndFulfiller<void>();
  auto blocker = fixture.getActor().getOutputGate().lockWhile(kj::mv(gatePaf.promise), nullptr);
  auto entrypoint = fixture.makeWorkerEntrypoint();
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);
  kj::NullStream requestBody;
  TestResponse response;
  auto request =
      entrypoint->request(
                    kj::HttpMethod::GET, "https://example.com", headers, requestBody, response)
          .eagerlyEvaluate(nullptr);

  fixture.pollEventLoop();
  gatePaf.fulfiller->reject(KJ_EXCEPTION(OVERLOADED, "output gate failed"));

  auto exception = kj::runCatchingExceptions([&]() { request.wait(fixture.getWaitScope()); });

  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_EXPECT(e.getType() == kj::Exception::Type::OVERLOADED);
  KJ_EXPECT(e.getDetail(WORKER_REQUEST_DELIVERED_DETAIL_ID) != kj::none);
  KJ_EXPECT(KJ_ASSERT_NONNULL(observer->outcome) == EventOutcome::INTERNAL_ERROR);
  KJ_EXPECT(observer->failureCount == 1);

  KJ_EXPECT_THROW_MESSAGE("output gate failed", blocker.wait(fixture.getWaitScope()));
}

}  // namespace
}  // namespace workerd
