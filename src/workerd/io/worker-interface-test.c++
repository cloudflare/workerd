// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "worker-interface.h"

#include <workerd/util/stream-utils.h>
#include <workerd/util/wait-list.h>

#include <kj/test.h>

namespace workerd {
namespace {

// Answers each request by sending an empty response or by accepting a WebSocket, and accepts each
// CONNECT tunnel, holding on to whatever it was given so the test can observe revocation.
class TestWorker final: public WorkerInterface {
 public:
  enum class Mode { SEND, ACCEPT_WEBSOCKET };
  Mode mode = Mode::SEND;

  kj::Vector<kj::Own<kj::WebSocket>> webSockets;
  kj::Maybe<kj::AsyncIoStream&> tunnel;

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    switch (mode) {
      case Mode::SEND:
        response.send(200, "OK"_kj, headers, static_cast<uint64_t>(0));
        break;
      case Mode::ACCEPT_WEBSOCKET:
        webSockets.add(response.acceptWebSocket(headers));
        break;
    }
    return kj::READY_NOW;
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    response.accept(200, "OK"_kj, headers);
    tunnel = connection;
    return kj::NEVER_DONE;
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
};

class TestResponse final: public kj::HttpService::Response {
 public:
  kj::Own<kj::AsyncOutputStream> send(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    return newNullOutputStream();
  }

  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    auto pipe = kj::newWebSocketPipe();
    peers.add(kj::mv(pipe.ends[1]));
    return kj::mv(pipe.ends[0]);
  }

 private:
  kj::Vector<kj::Own<kj::WebSocket>> peers;
};

class TestConnectResponse final: public kj::HttpService::ConnectResponse {
 public:
  void accept(uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers) override {}
  kj::Own<kj::AsyncOutputStream> reject(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    KJ_UNIMPLEMENTED("not used in this test");
  }
};

// Revokes like Pipeline does when it is killed: by waiting on a cross-thread wait list.
struct RevocableFixture {
  kj::EventLoop loop;
  kj::WaitScope waitScope{loop};
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers{headerTable};
  CrossThreadWaitList killed{{.useThreadLocalOptimization = true}};
  // Keeps the peer end of each accepted WebSocket open, so that only revocation ends them.
  TestResponse response;
  uint revokePromisesMade = 0;
  TestWorker* worker = nullptr;

  kj::Promise<void> makeRevokeProm() {
    ++revokePromisesMade;
    return killed.addWaiter().then(
        []() -> kj::Promise<void> { return KJ_EXCEPTION(FAILED, "revoked"); });
  }

  kj::Own<WorkerInterface> makeLazy() {
    auto inner = kj::heap<TestWorker>();
    worker = inner.get();
    return newRevocableWebSocketWorkerInterface(
        kj::mv(inner), [this]() { return makeRevokeProm(); });
  }

  kj::Own<WorkerInterface> makeEager() {
    auto inner = kj::heap<TestWorker>();
    worker = inner.get();
    return newRevocableWebSocketWorkerInterface(kj::mv(inner), makeRevokeProm());
  }

  void sendRequest(WorkerInterface& revocable, TestWorker::Mode mode) {
    worker->mode = mode;
    auto body = newNullInputStream();
    revocable.request(kj::HttpMethod::GET, "/"_kj, headers, *body, response).wait(waitScope);
  }

  void expectRevoked(kj::WebSocket& webSocket) {
    waitScope.poll();
    // A revoked WebSocket throws from receive() itself rather than returning a rejected promise.
    auto received = kj::evalNow([&]() { return webSocket.receive(); });
    KJ_EXPECT(received.poll(waitScope), "WebSocket was not revoked");
    KJ_EXPECT_THROW_MESSAGE("revoked", received.wait(waitScope));
  }
};

KJ_TEST("lazy revocation is never set up for a request that sends an ordinary response") {
  RevocableFixture fixture;
  auto revocable = fixture.makeLazy();

  fixture.sendRequest(*revocable, TestWorker::Mode::SEND);
  fixture.sendRequest(*revocable, TestWorker::Mode::SEND);

  KJ_EXPECT(fixture.revokePromisesMade == 0);
}

KJ_TEST("lazy revocation revokes every WebSocket from one shared revocation promise") {
  RevocableFixture fixture;
  auto revocable = fixture.makeLazy();

  fixture.sendRequest(*revocable, TestWorker::Mode::ACCEPT_WEBSOCKET);
  fixture.sendRequest(*revocable, TestWorker::Mode::ACCEPT_WEBSOCKET);
  KJ_EXPECT(fixture.revokePromisesMade == 1);

  fixture.killed.fulfill();
  for (auto& webSocket: fixture.worker->webSockets) {
    fixture.expectRevoked(*webSocket);
  }
}

KJ_TEST("lazy revocation revokes a WebSocket accepted after the revocation was triggered") {
  RevocableFixture fixture;
  auto revocable = fixture.makeLazy();

  fixture.killed.fulfill();
  fixture.sendRequest(*revocable, TestWorker::Mode::ACCEPT_WEBSOCKET);

  KJ_EXPECT(fixture.revokePromisesMade == 1);
  fixture.expectRevoked(*fixture.worker->webSockets[0]);
}

KJ_TEST("lazy revocation neuters a CONNECT tunnel") {
  RevocableFixture fixture;
  auto revocable = fixture.makeLazy();

  auto connection = newNullIoStream();
  TestConnectResponse response;
  auto connectTask = revocable->connect(
      "example.com:443"_kj, fixture.headers, *connection, response, kj::HttpConnectSettings{});
  KJ_EXPECT(fixture.revokePromisesMade == 1);

  fixture.killed.fulfill();
  fixture.waitScope.poll();
  auto& tunnel = KJ_ASSERT_NONNULL(fixture.worker->tunnel);
  KJ_EXPECT_THROW_MESSAGE("revoked", tunnel.write("x"_kjb).wait(fixture.waitScope));
}

KJ_TEST("eager revocation revokes an accepted WebSocket") {
  RevocableFixture fixture;
  auto revocable = fixture.makeEager();
  KJ_EXPECT(fixture.revokePromisesMade == 1);

  fixture.sendRequest(*revocable, TestWorker::Mode::ACCEPT_WEBSOCKET);

  fixture.killed.fulfill();
  fixture.expectRevoked(*fixture.worker->webSockets[0]);
}

}  // namespace
}  // namespace workerd
