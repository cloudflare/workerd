// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "queue.h"
#include "web-socket.h"

#include <workerd/io/io-context.h>
#include <workerd/io/worker-interface.h>
#include <workerd/io/worker.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

template <typename T>
class DefaultTypeHandler final: public jsg::TypeHandler<T> {
 public:
  v8::Local<v8::Value> wrap(jsg::Lock& js, T value) const override {
    KJ_UNIMPLEMENTED("not used in this test");
  }

  kj::Maybe<T> tryUnwrap(jsg::Lock& js, v8::Local<v8::Value> handle) const override {
    return T{};
  }
};

class MockHttpWorkerInterface final: public WorkerInterface {
 public:
  MockHttpWorkerInterface(uint& requestCount): requestCount(requestCount) {}

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    // Empty headers bound to the same header table as the request.
    auto responseHeaders = headers.cloneShallow();
    responseHeaders.clear();

    ++requestCount;

    if (headers.isWebSocket()) {
      response.send(500, "Not Upgraded"_kj, responseHeaders, static_cast<uint64_t>(0));
      return kj::READY_NOW;
    }

    return sendQueueResponse(requestBody, response, kj::mv(responseHeaders));
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
  kj::Promise<void> prewarm(kj::StringPtr url) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    return event->notSupported();
  }

 private:
  static kj::Promise<void> sendQueueResponse(kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response,
      kj::HttpHeaders responseHeaders) {
    co_await requestBody.readAllBytes();
    constexpr auto body = R"({"metadata":{"metrics":{}}})"_kj;
    auto output = response.send(200, "OK"_kj, responseHeaders, body.size());
    co_await output->write(body.asBytes());
  }

  uint& requestCount;
};

struct HttpTestIoChannelFactory final: public TestFixture::DummyIoChannelFactory {
  HttpTestIoChannelFactory(TimerChannel& timer, uint& requestCount)
      : DummyIoChannelFactory(timer),
        requestCount(requestCount) {}

  kj::Own<WorkerInterface> startSubrequest(uint channel, SubrequestMetadata metadata) override {
    return kj::heap<MockHttpWorkerInterface>(requestCount);
  }

  uint& requestCount;
};

TestFixture makeFixture(uint& requestCount) {
  return TestFixture(TestFixture::SetupParams{
    .actorId = Worker::Actor::Id(kj::str("test-actor")),
    .useRealTimers = false,
    .ioChannelFactory = kj::Function<kj::Own<IoChannelFactory>(TimerChannel&)>(
        [&](TimerChannel& timer) -> kj::Own<IoChannelFactory> {
    return kj::heap<HttpTestIoChannelFactory>(timer, requestCount);
  }),
  });
}

KJ_TEST("queue sends wait for output gate") {
  uint requestCount = 0;
  auto fixture = makeFixture(requestCount);

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto paf = kj::newPromiseAndFulfiller<void>();
    auto blocker =
        env.context.getActorOrThrow().getOutputGate().lockWhile(kj::mv(paf.promise), nullptr);

    auto queue = env.js.alloc<WorkerQueue>(0);
    static const DefaultTypeHandler<WorkerQueue::SendResponse> sendResponseHandler;
    queue
        ->send(env.js, jsg::JsValue(env.js.str("message"_kj)), kj::none, sendResponseHandler)
        .markAsHandled(env.js);

    auto messages = kj::heapArray<WorkerQueue::MessageSendRequest>(1);
    messages[0].body = jsg::JsValue(env.js.str("batch message"_kj)).addRef(env.js);
    static const DefaultTypeHandler<WorkerQueue::SendBatchResponse> sendBatchResponseHandler;
    queue
        ->sendBatch(env.js, jsg::Sequence<WorkerQueue::MessageSendRequest>(kj::mv(messages)),
            kj::none, sendBatchResponseHandler)
        .markAsHandled(env.js);

    co_await kj::evalLater([]() {});
    KJ_EXPECT(requestCount == 0);

    paf.fulfiller->fulfill();
    co_await kj::evalLater([]() {});
    KJ_EXPECT(requestCount == 2);
  });
}

KJ_TEST("outbound WebSocket handshake waits for output gate") {
  uint requestCount = 0;
  auto fixture = makeFixture(requestCount);

  fixture.runInIoContext([&](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto paf = kj::newPromiseAndFulfiller<void>();
    auto blocker =
        env.context.getActorOrThrow().getOutputGate().lockWhile(kj::mv(paf.promise), nullptr);

    auto webSocket = WebSocket::constructor(env.js, kj::str("wss://example.com"), kj::none);

    co_await kj::evalLater([]() {});
    KJ_EXPECT(requestCount == 0);

    paf.fulfiller->fulfill();
    co_await kj::evalLater([]() {});
    KJ_EXPECT(requestCount == 1);
  });
}

}  // namespace
}  // namespace workerd::api
