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
  KJ_ASSERT(e.getType() == kj::Exception::Type::FAILED);
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
  KJ_ASSERT(e.getType() == kj::Exception::Type::FAILED);
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
        workerd::FrankenvalueHandler& frankenvalueHandler,
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
        workerd::FrankenvalueHandler& frankenvalueHandler,
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

// ======================================================================================
// Reverse direction: a Rust CxxWorkerInterface wrapping (delegating to) a C++ WorkerInterface.

namespace {

// Records the calls it receives, and returns distinctive values so the test can confirm the call
// travelled C++ stub -> Rust CxxWorkerInterface -> back to C++.
class StubWorker final: public WorkerInterface {
 public:
  bool prewarmCalled = false;
  kj::String prewarmUrl;
  bool runScheduledCalled = false;
  bool runAlarmCalled = false;
  bool customEventCalled = false;
  bool testCalled = false;
  bool connectCalled = false;
  kj::String connectHost;

  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    auto body = response.send(200, "OK", headers, kj::str("STUB").size());
    return body->write("STUB"_kjb).attach(kj::mv(body));
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    connectCalled = true;
    connectHost = kj::str(host);
    response.accept(200, "OK", headers);
    return kj::READY_NOW;
  }

  kj::Promise<void> prewarm(kj::StringPtr url) override {
    prewarmCalled = true;
    prewarmUrl = kj::str(url);
    return kj::READY_NOW;
  }

  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    runScheduledCalled = true;
    return ScheduledResult{.retry = true, .outcome = workerd::EventOutcome::OK};
  }

  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    runAlarmCalled = true;
    return AlarmResult{
      .retry = true, .retryCountsAgainstLimit = false, .outcome = workerd::EventOutcome::OK};
  }

  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    customEventCalled = true;
    return CustomEvent::Result{.outcome = workerd::EventOutcome::OK};
  }

  kj::Promise<bool> test() override {
    testCalled = true;
    return true;
  }
};

}  // namespace

KJ_TEST("cxx_worker delegates non-HTTP events to the wrapped C++ worker") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto stub = kj::heap<StubWorker>();
  auto& stubRef = *stub;
  // Wrap the C++ stub as a Rust worker::Interface, then re-expose it to C++.
  auto worker = kj::from<Rust>(new_cxx_worker(kj::mv(stub)));

  worker->prewarm("/warm"_kj).wait(waitScope);
  KJ_ASSERT(stubRef.prewarmCalled);
  KJ_ASSERT(stubRef.prewarmUrl == "/warm");

  auto scheduled =
      worker->runScheduled(kj::UNIX_EPOCH + 1000 * kj::SECONDS, "0 0 * * *"_kj).wait(waitScope);
  KJ_ASSERT(stubRef.runScheduledCalled);
  KJ_ASSERT(scheduled.retry == true);
  KJ_ASSERT(scheduled.outcome == workerd::EventOutcome::OK);

  auto alarm = worker->runAlarm(kj::UNIX_EPOCH + 2000 * kj::SECONDS, 3).wait(waitScope);
  KJ_ASSERT(stubRef.runAlarmCalled);
  KJ_ASSERT(alarm.retry == true);
  KJ_ASSERT(alarm.retryCountsAgainstLimit == false);
  KJ_ASSERT(alarm.outcome == workerd::EventOutcome::OK);

  KJ_ASSERT(worker->test().wait(waitScope));
  KJ_ASSERT(stubRef.testCalled);
}

KJ_TEST("cxx_worker delegates customEvent (ownership transfer) to the wrapped C++ worker") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

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
        workerd::FrankenvalueHandler& frankenvalueHandler,
        workerd::rpc::EventDispatcher::Client dispatcher) override {
      KJ_UNIMPLEMENTED();
    }
    kj::Promise<Result> notSupported() override {
      KJ_UNIMPLEMENTED();
    }
    uint16_t getType() override {
      return 42;
    }
    workerd::tracing::EventInfo getEventInfo() const override {
      return workerd::tracing::CustomEventInfo();
    }
  };

  auto stub = kj::heap<StubWorker>();
  auto& stubRef = *stub;
  auto worker = kj::from<Rust>(new_cxx_worker(kj::mv(stub)));

  auto result = worker->customEvent(kj::heap<TestCustomEvent>()).wait(waitScope);
  KJ_ASSERT(stubRef.customEventCalled);
  KJ_ASSERT(result.outcome == workerd::EventOutcome::OK);
}

KJ_TEST("cxx_worker delegates request to the wrapped C++ worker") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto client = newClient(new_cxx_worker(kj::heap<StubWorker>()));
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);

  auto response =
      client->request(kj::HttpMethod::GET, "http://test/"_kj, headers).response.wait(waitScope);
  KJ_ASSERT(response.statusCode == 200);
  KJ_ASSERT(response.body->readAllText().wait(waitScope) == "STUB");
}

KJ_TEST("cxx_worker delegates connect to the wrapped C++ worker") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto stub = kj::heap<StubWorker>();
  auto& stubRef = *stub;
  auto client = newClient(new_cxx_worker(kj::mv(stub)));
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);

  auto response = client->connect("example.com:443"_kj, headers, kj::HttpConnectSettings{})
                      .status.wait(waitScope);
  KJ_ASSERT(stubRef.connectCalled);
  KJ_ASSERT(stubRef.connectHost == "example.com:443");
  KJ_ASSERT(response.statusCode == 200);
}

}  // namespace
}  // namespace workerd
