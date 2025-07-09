// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "rust-worker-interface.h"
#include "workerd/rust/worker/lib.rs.h"

#include <kj/async-io.h>
#include <kj/compat/http.h>
#include <kj/debug.h>
#include <kj/test.h>

namespace workerd::rust::worker {
namespace {

KJ_TEST("RustWorkerInterface async HTTP request test") {
  // Set up proper event loop and wait scope
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  // Create exception worker
  auto worker = RustWorkerInterfaceWrapper(
      create_exception_worker("HTTP request async test error"));

  // Set up HTTP header table
  kj::HttpHeaderTable::Builder headerTableBuilder;
  auto headerTable = headerTableBuilder.build();
  kj::HttpHeaders headers(*headerTable);
  headers.set(kj::HttpHeaderId::HOST, "example.com");
  // headers.set(kj::HttpHeaderId::USER_AGENT, "test-client");

  // Create async I/O infrastructure
  auto io = kj::setupAsyncIo();
  auto pipe = kj::newOneWayPipe();

  // Mock response that captures calls
  class TestResponse final: public kj::HttpService::Response {
   public:
    bool sendCalled = false;
    bool acceptWebSocketCalled = false;

    kj::Own<kj::AsyncOutputStream> send(kj::uint statusCode,
        kj::StringPtr statusText,
        const kj::HttpHeaders& headers,
        kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
      sendCalled = true;
      KJ_FAIL_ASSERT("Should not reach here - exception worker should fail before this");
    }

    kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
      acceptWebSocketCalled = true;
      KJ_FAIL_ASSERT("Should not reach here - exception worker should fail before this");
    }
  };

  TestResponse response;

  // Test async HTTP request - should throw because it's not implemented
  auto requestPromise =
      worker.request(kj::HttpMethod::GET, "http://example.com/test", headers, *pipe.in, response);

  // The promise should resolve with an exception
  KJ_EXPECT_THROW_MESSAGE(
      "Async Rust functions not yet implemented in CXX bridge", requestPromise.wait(ws));

  // Verify that the response methods were never called
  KJ_ASSERT(!response.sendCalled);
  KJ_ASSERT(!response.acceptWebSocketCalled);
}

KJ_TEST("RustWorkerInterface async connect test") {
  // Set up proper event loop and wait scope
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  // Create exception worker
  auto worker = RustWorkerInterfaceWrapper(create_exception_worker("Connect async test error"));

  // Set up HTTP header table
  kj::HttpHeaderTable::Builder headerTableBuilder;
  auto headerTable = headerTableBuilder.build();
  kj::HttpHeaders headers(*headerTable);
  headers.set(kj::HttpHeaderId::HOST, "example.com");

  // Create async I/O infrastructure
  auto io = kj::setupAsyncIo();
  auto pipe = kj::newTwoWayPipe();

  // Mock connect response
  class TestConnectResponse final: public kj::HttpService::ConnectResponse {
   public:
    bool acceptCalled = false;
    bool rejectCalled = false;

    void accept(
        kj::uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers) override {
      acceptCalled = true;
      KJ_FAIL_ASSERT("Should not reach here - exception worker should fail before this");
    }

    kj::Own<kj::AsyncOutputStream> reject(kj::uint statusCode,
        kj::StringPtr statusText,
        const kj::HttpHeaders& headers,
        kj::Maybe<uint64_t> expectedBodySize = kj::none) override {
      rejectCalled = true;
      KJ_FAIL_ASSERT("Should not reach here - exception worker should fail before this");
    }
  };

  TestConnectResponse connectResponse;
  kj::HttpConnectSettings settings;

  // Test async connect - should throw because it's not implemented
  auto connectPromise =
      worker.connect("example.com", headers, *pipe.ends[0], connectResponse, settings);

  // The promise should resolve with an exception
  KJ_EXPECT_THROW_MESSAGE(
      "Async Rust functions not yet implemented in CXX bridge", connectPromise.wait(ws));

  // Verify that the connect response methods were never called
  KJ_ASSERT(!connectResponse.acceptCalled);
  KJ_ASSERT(!connectResponse.rejectCalled);
}

KJ_TEST("RustWorkerInterface async prewarm test") {
  // Set up proper event loop and wait scope
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  // Create exception worker
  auto worker = RustWorkerInterfaceWrapper(create_exception_worker("Prewarm async test error"));

  // Test async prewarm - should throw because it's not implemented
  auto prewarmPromise = worker.prewarm("http://example.com/prewarm");

  // The promise should resolve with an exception
  KJ_EXPECT_THROW_MESSAGE(
      "Async Rust functions not yet implemented in CXX bridge", prewarmPromise.wait(ws));
}

KJ_TEST("RustWorkerInterface async test method") {
  // Set up proper event loop and wait scope
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  // Create exception worker
  auto worker = RustWorkerInterfaceWrapper(create_exception_worker("Test method async error"));

  // Test async test method - should throw because it's not implemented
  auto testPromise = worker.test();

  // The promise should resolve with an exception
  KJ_EXPECT_THROW_MESSAGE(
      "Async Rust functions not yet implemented in CXX bridge", testPromise.wait(ws));
}

KJ_TEST("RustWorkerInterface async runScheduled test") {
  // Set up proper event loop and wait scope
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  // Create exception worker
  auto worker = RustWorkerInterfaceWrapper(create_exception_worker("RunScheduled async test error"));

  // Test async runScheduled - should throw because it's not implemented
  kj::Date scheduledTime = kj::UNIX_EPOCH + 1000 * kj::SECONDS;
  auto scheduledPromise = worker.runScheduled(scheduledTime, "0 0 * * *");

  // The promise should resolve with an exception
  KJ_EXPECT_THROW_MESSAGE(
      "Async Rust functions not yet implemented in CXX bridge", scheduledPromise.wait(ws));
}

KJ_TEST("RustWorkerInterface async runAlarm test") {
  // Set up proper event loop and wait scope
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  // Create exception worker
  auto worker = RustWorkerInterfaceWrapper(create_exception_worker("RunAlarm async test error"));

  // Test async runAlarm - should throw because it's not implemented
  kj::Date scheduledTime = kj::UNIX_EPOCH + 2000 * kj::SECONDS;
  auto alarmPromise = worker.runAlarm(scheduledTime, 5);

  // The promise should resolve with an exception
  KJ_EXPECT_THROW_MESSAGE(
      "Async Rust functions not yet implemented in CXX bridge", alarmPromise.wait(ws));
}

KJ_TEST("RustWorkerInterface multiple async operations") {
  // Set up proper event loop and wait scope
  kj::EventLoop loop;
  kj::WaitScope ws(loop);

  // Create multiple exception workers
  auto worker1 = RustWorkerInterfaceWrapper(create_exception_worker("Multi-op test error 1"));
  auto worker2 = RustWorkerInterfaceWrapper(create_exception_worker("Multi-op test error 2"));

  // Test multiple async operations in sequence
  auto prewarmPromise1 = worker1.prewarm("http://example1.com");
  auto prewarmPromise2 = worker2.prewarm("http://example2.com");

  // Both should fail with the same exception
  KJ_EXPECT_THROW_MESSAGE(
      "Async Rust functions not yet implemented in CXX bridge", prewarmPromise1.wait(ws));
  KJ_EXPECT_THROW_MESSAGE(
      "Async Rust functions not yet implemented in CXX bridge", prewarmPromise2.wait(ws));
}

}  // namespace
}  // namespace workerd::rust::worker
