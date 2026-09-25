// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-entrypoint.h"

#include <workerd/jsg/util.h>
#include <workerd/tests/test-fixture.h>
#include <workerd/util/autogate.h>

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
    kj::throwFatalException(KJ_EXCEPTION(FAILED, "channel creation failed"));
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
    kj::throwFatalException(KJ_EXCEPTION(FAILED, "connect rejection failed"));
  }
};

class TestResponse final: public kj::HttpService::Response {
 public:
  kj::Own<kj::AsyncOutputStream> send(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    this->statusCode = statusCode;
    return kj::heap<kj::NullStream>();
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    KJ_FAIL_ASSERT("request unexpectedly returned a WebSocket");
  }

  uint statusCode = 0;
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
      kj::throwFatalException(e.clone());
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
    kj::throwFatalException(KJ_EXCEPTION(FAILED, "response send failed"));
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
    kj::throwFatalException(KJ_EXCEPTION(FAILED, "custom event failed"));
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
      kj::throwFatalException(kj::mv(exception));
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

  kj::Maybe<EventOutcome> outcome;

  uint failureCount = 0;

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

// workerd does not transform decorator syntax, so the script calls the decorator the way a
// bundler's standard-decorator output does.
constexpr kj::StringPtr RETRYABLE_FETCH_ACTOR_SOURCE = R"SCRIPT(
  import { DurableObject, retryable } from "cloudflare:durable-objects";
  class Actor extends DurableObject {
    async fetch() {
      return new Response("OK");
    }
  }
  retryable(Actor.prototype.fetch, { kind: "method", name: "fetch", static: false, private: false });
  export default Actor;
)SCRIPT"_kj;

IsRetryableHandler claimRetryableForDecoratedFetch(kj::ArrayPtr<const kj::StringPtr> autogates) {
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

KJ_TEST("a @retryable fetch claims as retryable when the userland gate is enabled") {
  auto gates = kj::arr("durable-object-retries-userland"_kj);
  KJ_EXPECT(claimRetryableForDecoratedFetch(gates) == IsRetryableHandler::YES);
}

KJ_TEST("a @retryable fetch claims as not retryable when the userland gate is disabled") {
  KJ_EXPECT(claimRetryableForDecoratedFetch(nullptr) == IsRetryableHandler::NO);
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
