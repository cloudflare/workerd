// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-entrypoint.h"

#include <workerd/jsg/util.h>
#include <workerd/tests/test-fixture.h>

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
    return kj::heap<kj::NullStream>();
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    KJ_FAIL_ASSERT("request unexpectedly returned a WebSocket");
  }
};

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

class PredecessorRejectedObserver final: public RequestObserver {
 public:
  void claimRetryTokenBeforeUserCode() override {
    auto exception = KJ_EXCEPTION(DISCONNECTED, "request rejected before user code");
    jsg::markActorRequestNotDelivered(exception);
    exception.setDetail(jsg::ACTOR_PREDECESSOR_REJECTED_DETAIL_ID, kj::heapArray<kj::byte>(0));
    kj::throwFatalException(kj::mv(exception));
  }
};

KJ_TEST("actor fetch preserves a predecessor rejection before user code") {
  TestFixture fixture(TestFixture::SetupParams{
    .actorId = Worker::Actor::Id(kj::str("not-delivered-test")),
    .requestObserverFactory = kj::Function<kj::Own<RequestObserver>()>(
        []() -> kj::Own<RequestObserver> { return kj::refcounted<PredecessorRejectedObserver>(); }),
  });
  auto entrypoint = fixture.makeWorkerEntrypoint();
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);
  kj::NullStream requestBody;
  TestResponse response;

  auto exception = kj::runCatchingExceptions([&]() {
    entrypoint->request(kj::HttpMethod::GET, "https://example.com", headers, requestBody, response)
        .wait(fixture.getWaitScope());
  });

  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_EXPECT(e.getType() == kj::Exception::Type::DISCONNECTED, e);
  KJ_EXPECT(e.getDetail(jsg::REQUEST_NOT_DELIVERED_TO_ACTOR_DETAIL_ID) != kj::none, e);
  KJ_EXPECT(e.getDetail(jsg::REQUEST_DELIVERED_TO_ACTOR_DETAIL_ID) == kj::none, e);
  KJ_EXPECT(e.getDetail(jsg::ACTOR_PREDECESSOR_REJECTED_DETAIL_ID) != kj::none, e);
}

class UnqualifiedNotDeliveredObserver final: public RequestObserver {
 public:
  void claimRetryTokenBeforeUserCode() override {
    auto exception = KJ_EXCEPTION(DISCONNECTED, "request rejected before user code");
    jsg::markActorRequestNotDelivered(exception);
    kj::throwFatalException(kj::mv(exception));
  }
};

KJ_TEST("actor fetch does not preserve an unqualified not-delivered marker") {
  TestFixture fixture(TestFixture::SetupParams{
    .actorId = Worker::Actor::Id(kj::str("not-delivered-test")),
    .requestObserverFactory =
        kj::Function<kj::Own<RequestObserver>()>([]() -> kj::Own<RequestObserver> {
    return kj::refcounted<UnqualifiedNotDeliveredObserver>();
  }),
  });
  auto entrypoint = fixture.makeWorkerEntrypoint();
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);
  kj::NullStream requestBody;
  TestResponse response;

  auto exception = kj::runCatchingExceptions([&]() {
    entrypoint->request(kj::HttpMethod::GET, "https://example.com", headers, requestBody, response)
        .wait(fixture.getWaitScope());
  });

  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_EXPECT(e.getType() == kj::Exception::Type::DISCONNECTED, e);
  KJ_EXPECT(e.getDetail(jsg::REQUEST_NOT_DELIVERED_TO_ACTOR_DETAIL_ID) == kj::none, e);
  KJ_EXPECT(e.getDetail(jsg::REQUEST_DELIVERED_TO_ACTOR_DETAIL_ID) != kj::none, e);
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
