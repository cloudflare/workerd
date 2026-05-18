// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/frankenvalue.h>
#include <workerd/io/io-context.h>
#include <workerd/io/worker.h>
#include <workerd/rust/worker/bridge.h>
#include <workerd/rust/worker/error.rs.h>
#include <workerd/rust/worker/kill_switch.rs.h>
#include <workerd/rust/worker/ok.rs.h>
#include <workerd/util/exception.h>

#include <kj-rs/kj-rs.h>

#include <kj/async.h>
#include <kj/compat/http.h>
#include <kj/test.h>

using namespace workerd::rust::worker;
using namespace kj_rs;

namespace workerd {
namespace {

kj::Own<kj::HttpClient> newClient(::rust::Box<RustWorkerInterface::Impl> impl) {
  auto worker = kj::from<Rust>(kj::mv(impl));
  return kj::newHttpClient(*worker).attach(kj::mv(worker));
}

KJ_TEST("kill_switch worker") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto client = newClient(new_kill_switch_worker());
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);
  auto exception = kj::runCatchingExceptions([&]() {
    client->request(kj::HttpMethod::GET, "http://test/"_kj, headers).response.wait(waitScope);
  });

  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_ASSERT(e.getType() == kj::Exception::Type::OVERLOADED);
  KJ_ASSERT(e.getDescription() == "jsg.Error: This script has been killed.");
  KJ_ASSERT(e.getDetail(SCRIPT_KILLED_DETAIL_ID) != kj::none);
}

KJ_TEST("ok worker request") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto client = newClient(new_ok_worker());
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);

  auto response =
      client->request(kj::HttpMethod::GET, "http://test/"_kj, headers).response.wait(waitScope);

  KJ_ASSERT(response.statusCode == 200);
  KJ_ASSERT(response.statusText == "OK");
  KJ_ASSERT(response.body->readAllText().wait(waitScope) == "OK");
}

KJ_TEST("kill_switch worker connect") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto client = newClient(new_kill_switch_worker());
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);
  auto exception = kj::runCatchingExceptions([&]() {
    client->connect("example.com"_kj, headers, kj::HttpConnectSettings{}).status.wait(waitScope);
  });

  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_ASSERT(e.getType() == kj::Exception::Type::OVERLOADED);
  KJ_ASSERT(e.getDescription() == "jsg.Error: This script has been killed.");
  KJ_ASSERT(e.getDetail(SCRIPT_KILLED_DETAIL_ID) != kj::none);
}

KJ_TEST("kill_switch worker prewarm") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto worker = kj::from<Rust>(new_kill_switch_worker());

  // Prewarm should succeed (default implementation)
  kj::StringPtr url = "/test";

  // Should not throw
  worker->prewarm(url).wait(waitScope);
}

KJ_TEST("kill_switch worker runScheduled") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto worker = kj::from<Rust>(new_kill_switch_worker());

  kj::Date scheduledTime = kj::UNIX_EPOCH + 1000 * kj::SECONDS;
  kj::StringPtr cron = "0 0 * * *";

  auto result = worker->runScheduled(scheduledTime, cron).wait(waitScope);

  KJ_ASSERT(result.retry == false);
  KJ_ASSERT(result.outcome == workerd::EventOutcome::KILL_SWITCH);
}

KJ_TEST("kill_switch worker runAlarm") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto worker = kj::from<Rust>(new_kill_switch_worker());

  kj::Date scheduledTime = kj::UNIX_EPOCH + 2000 * kj::SECONDS;
  uint32_t retryCount = 3;

  auto result = worker->runAlarm(scheduledTime, retryCount).wait(waitScope);

  KJ_ASSERT(result.retry == false);
  KJ_ASSERT(result.retryCountsAgainstLimit == true);
  KJ_ASSERT(result.outcome == workerd::EventOutcome::KILL_SWITCH);
}

KJ_TEST("kill_switch worker customEvent") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto worker = kj::from<Rust>(new_kill_switch_worker());

  // Create a minimal custom event mock
  class TestCustomEvent: public workerd::WorkerInterface::CustomEvent {
   public:
    virtual ~TestCustomEvent() = default;

    kj::Promise<Result> run(kj::Own<workerd::IoContext_IncomingRequest> incomingRequest,
        kj::Maybe<kj::StringPtr> entrypointName,
        kj::Maybe<workerd::Worker::VersionInfo> versionInfo,
        workerd::Frankenvalue props,
        kj::TaskSet& waitUntilTasks,
        bool) override {
      KJ_UNIMPLEMENTED();
    }

    kj::Promise<Result> sendRpc(capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
        capnp::ByteStreamFactory& byteStreamFactory,
        workerd::rpc::EventDispatcher::Client dispatcher) override {
      KJ_UNIMPLEMENTED();
    }

    kj::Promise<Result> notSupported() override {
      KJ_UNIMPLEMENTED();
    };

    uint16_t getType() override {
      return 42;
    }

    workerd::tracing::EventInfo getEventInfo() const override {
      return workerd::tracing::CustomEventInfo();
    }
  };

  auto event = kj::heap<TestCustomEvent>();

  auto result = worker->customEvent(kj::mv(event)).wait(waitScope);

  KJ_ASSERT(result.outcome == workerd::EventOutcome::KILL_SWITCH);
}

KJ_TEST("kill_switch worker test") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto worker = kj::from<Rust>(new_kill_switch_worker());

  auto p = worker->test();
  bool result = p.wait(waitScope);

  KJ_ASSERT(result == false);
}

KJ_TEST("error worker request") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto client = newClient(new_error_worker("Test error message"));
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);
  auto exception = kj::runCatchingExceptions([&]() {
    client->request(kj::HttpMethod::GET, "http://test/test"_kj, headers).response.wait(waitScope);
  });

  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_ASSERT(e.getType() == kj::Exception::Type::FAILED);
  KJ_ASSERT(e.getDescription() == "jsg.Error: Test error message");
}

KJ_TEST("error worker connect") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto client = newClient(new_error_worker("Connection failed"));
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);
  auto exception = kj::runCatchingExceptions([&]() {
    client->connect("example.com"_kj, headers, kj::HttpConnectSettings{}).status.wait(waitScope);
  });

  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_ASSERT(e.getType() == kj::Exception::Type::FAILED);
  KJ_ASSERT(e.getDescription() == "jsg.Error: Connection failed");
}

KJ_TEST("error worker runScheduled") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto worker = kj::from<Rust>(new_error_worker("Scheduled task failed"));

  kj::Date scheduledTime = kj::UNIX_EPOCH + 1000 * kj::SECONDS;
  kj::StringPtr cron = "0 0 * * *";

  kj::Maybe<kj::Exception> exception;

  try {
    worker->runScheduled(scheduledTime, cron).wait(waitScope);
  } catch (...) {
    exception = kj::getCaughtExceptionAsKj();
  }

  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_ASSERT(e.getType() == kj::Exception::Type::FAILED);
  KJ_ASSERT(e.getDescription() == "jsg.Error: Scheduled task failed");
}

KJ_TEST("error worker runAlarm") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto worker = kj::from<Rust>(new_error_worker("Alarm execution failed"));

  kj::Date scheduledTime = kj::UNIX_EPOCH + 2000 * kj::SECONDS;
  uint32_t retryCount = 5;

  kj::Maybe<kj::Exception> exception;

  try {
    worker->runAlarm(scheduledTime, retryCount).wait(waitScope);
  } catch (...) {
    exception = kj::getCaughtExceptionAsKj();
  }

  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_ASSERT(e.getType() == kj::Exception::Type::FAILED);
  KJ_ASSERT(e.getDescription() == "jsg.Error: Alarm execution failed");
}

KJ_TEST("error worker customEvent") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto worker = kj::from<Rust>(new_error_worker("Custom event error"));

  // Create a minimal custom event mock
  class TestCustomEvent: public workerd::WorkerInterface::CustomEvent {
   public:
    virtual ~TestCustomEvent() = default;

    kj::Promise<workerd::WorkerInterface::CustomEvent::Result> run(
        kj::Own<workerd::IoContext_IncomingRequest> incomingRequest,
        kj::Maybe<kj::StringPtr> entrypointName,
        kj::Maybe<workerd::Worker::VersionInfo> versionInfo,
        workerd::Frankenvalue props,
        kj::TaskSet& waitUntilTasks,
        bool) override {
      KJ_UNIMPLEMENTED();
    }

    kj::Promise<workerd::WorkerInterface::CustomEvent::Result> sendRpc(
        capnp::HttpOverCapnpFactory& httpOverCapnpFactory,
        capnp::ByteStreamFactory& byteStreamFactory,
        workerd::rpc::EventDispatcher::Client dispatcher) override {
      KJ_UNIMPLEMENTED();
    }

    kj::Promise<workerd::WorkerInterface::CustomEvent::Result> notSupported() override {
      KJ_UNIMPLEMENTED();
    };

    uint16_t getType() override {
      return 123;
    }

    workerd::tracing::EventInfo getEventInfo() const override {
      return workerd::tracing::CustomEventInfo();
    }
  };

  auto event = kj::heap<TestCustomEvent>();

  kj::Maybe<kj::Exception> exception;

  try {
    worker->customEvent(kj::mv(event)).wait(waitScope);
  } catch (...) {
    exception = kj::getCaughtExceptionAsKj();
  }

  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_ASSERT(e.getType() == kj::Exception::Type::FAILED);
  KJ_ASSERT(e.getDescription() == "jsg.Error: Custom event error");
}

KJ_TEST("error worker test") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto worker = kj::from<Rust>(new_error_worker("Test method failed"));

  KJ_ASSERT(!worker->test().wait(waitScope));
}

}  // namespace
}  // namespace workerd
