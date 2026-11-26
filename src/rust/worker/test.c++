// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <workerd/io/frankenvalue.h>
#include <workerd/io/io-context.h>
#include <workerd/io/worker.h>
#include <workerd/rust/worker/bridge.h>
#include <workerd/rust/worker/error.rs.h>
#include <workerd/rust/worker/failure.rs.h>
#include <workerd/rust/worker/kill_switch.rs.h>
#include <workerd/rust/worker/ok.rs.h>
#include <workerd/util/exception.h>

#include <kj-rs/kj-rs.h>

#include <capnp/message.h>
#include <kj/async-io.h>
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

KJ_TEST("ok worker customEvent answers with the event's notSupported()") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  // The ok worker does not override Interface::custom_event. The default must call the event's
  // notSupported() and return its result.
  class NotSupportedEvent final: public workerd::WorkerInterface::CustomEvent {
   public:
    explicit NotSupportedEvent(bool& notSupportedCalled): notSupportedCalled(notSupportedCalled) {}

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
      notSupportedCalled = true;
      return Result{.outcome = workerd::EventOutcome::SCRIPT_NOT_FOUND};
    }
    uint16_t getType() override {
      return 42;
    }
    workerd::tracing::EventInfo getEventInfo() const override {
      return workerd::tracing::CustomEventInfo();
    }

   private:
    bool& notSupportedCalled;
  };

  auto worker = kj::from<Rust>(new_ok_worker());
  bool notSupportedCalled = false;

  auto result =
      worker->customEvent(kj::heap<NotSupportedEvent>(notSupportedCalled)).wait(waitScope);

  KJ_ASSERT(notSupportedCalled);
  KJ_ASSERT(result.outcome == workerd::EventOutcome::SCRIPT_NOT_FOUND);
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

  kj::Promise<kj::Maybe<kj::Date>> abandonAlarm(kj::Date scheduledTime) override {
    abandonAlarmCalled = true;
    abandonedAlarmTime = scheduledTime;
    return kj::Maybe<kj::Date>(STORED_ALARM_TIME);
  }

  bool abandonAlarmCalled = false;
  kj::Date abandonedAlarmTime = kj::UNIX_EPOCH;
  static constexpr kj::Date STORED_ALARM_TIME = kj::UNIX_EPOCH + 5000 * kj::SECONDS;

  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    customEventCalled = true;
    return CustomEvent::Result{.outcome = workerd::EventOutcome::OK};
  }

  kj::Promise<bool> test() override {
    testCalled = true;
    return true;
  }

  bool benchCalled = false;
  kj::String benchCaseFilter;
  bool benchQuick = false;
  uint32_t benchSamples = 0;

  // Records the params and fills every part of the group a handler fills.
  kj::Promise<void> bench(
      bench::BenchParams::Reader params, bench::BenchReport::Group::Builder report) override {
    benchCalled = true;
    benchCaseFilter = kj::str(params.getCaseFilter());
    benchQuick = params.getQuick();
    benchSamples = params.getDefaults().getSamples();
    report.setError("stub error");
    report.initOverhead().setSyncNs(1.5);
    auto cases = report.initCases(2);
    cases[0].setName("first");
    cases[0].setIterationsPerSample(7);
    cases[0].initWallNs().initSamples(3).set(2, 42.5);
    cases[1].setName("second");
    cases[1].setStatus(bench::BenchReport::Case::Status::FAILED);
    cases[1].setError("case failed");
    return kj::READY_NOW;
  }
};

// Bench params and a report group, as `workerd bench` provides them.
struct BenchCall {
  capnp::MallocMessageBuilder paramsMessage;
  capnp::MallocMessageBuilder reportMessage;
  bench::BenchParams::Builder params = paramsMessage.initRoot<bench::BenchParams>();
  bench::BenchReport::Group::Builder report = reportMessage.initRoot<bench::BenchReport::Group>();

  BenchCall() {
    params.setCaseFilter("first*");
    params.setQuick(true);
    params.initDefaults().setSamples(9);
    report.setName("caller's name");
    report.setStartup(true);
  }

  kj::Promise<void> run(WorkerInterface& worker) {
    return worker.bench(params.asReader(), report);
  }
};

// Checks that `stub` got `call`'s params and `call`'s report holds the group the stub filled in,
// with the caller's name and startup flag kept.
void expectBenchForwarded(StubWorker& stub, BenchCall& call) {
  KJ_ASSERT(stub.benchCalled);
  KJ_EXPECT(stub.benchCaseFilter == "first*");
  KJ_EXPECT(stub.benchQuick);
  KJ_EXPECT(stub.benchSamples == 9);

  auto report = call.report.asReader();
  KJ_EXPECT(report.getName() == "caller's name");
  KJ_EXPECT(report.getStartup());
  KJ_EXPECT(report.getError() == "stub error");
  KJ_EXPECT(report.getOverhead().getSyncNs() == 1.5);
  auto cases = report.getCases();
  KJ_ASSERT(cases.size() == 2);
  KJ_EXPECT(cases[0].getName() == "first");
  KJ_EXPECT(cases[0].getIterationsPerSample() == 7);
  KJ_ASSERT(cases[0].getWallNs().getSamples().size() == 3);
  KJ_EXPECT(cases[0].getWallNs().getSamples()[2] == 42.5);
  KJ_EXPECT(cases[1].getName() == "second");
  KJ_EXPECT(cases[1].getStatus() == bench::BenchReport::Case::Status::FAILED);
  KJ_EXPECT(cases[1].getError() == "case failed");
}

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

  auto stored = worker->abandonAlarm(kj::UNIX_EPOCH + 3000 * kj::SECONDS).wait(waitScope);
  KJ_ASSERT(stubRef.abandonAlarmCalled);
  KJ_ASSERT(stubRef.abandonedAlarmTime == kj::UNIX_EPOCH + 3000 * kj::SECONDS);
  KJ_ASSERT(KJ_ASSERT_NONNULL(stored) == StubWorker::STORED_ALARM_TIME);

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

KJ_TEST("cxx_worker delegates bench to the wrapped C++ worker") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto stub = kj::heap<StubWorker>();
  auto& stubRef = *stub;
  // The call crosses the C++ wrapper of a Rust interface (RustWorkerInterface), then the Rust
  // wrapper of a C++ interface (CxxWorkerInterface), each encoding and decoding the messages.
  auto worker = kj::from<Rust>(new_cxx_worker(kj::mv(stub)));

  BenchCall call;
  call.run(*worker).wait(waitScope);
  expectBenchForwarded(stubRef, call);
}

KJ_TEST("cxx_worker passes on the wrapped C++ worker's bench failure") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  class NoBenchWorker final: public WorkerInterface {
   public:
    kj::Promise<void> request(kj::HttpMethod method,
        kj::StringPtr url,
        const kj::HttpHeaders& headers,
        kj::AsyncInputStream& requestBody,
        Response& response) override {
      KJ_UNIMPLEMENTED();
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
  };

  auto worker = kj::from<Rust>(new_cxx_worker(kj::heap<NoBenchWorker>()));
  BenchCall call;
  auto exception = kj::runCatchingExceptions([&]() { call.run(*worker).wait(waitScope); });
  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_EXPECT(e.getType() == kj::Exception::Type::UNIMPLEMENTED);
  KJ_EXPECT(e.getDescription() == "this service doesn't support bench()");
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

namespace {

// Stand-ins for the frames of a stack trace. They are compared, never symbolized.
void* const TRACED_FRAMES[] = {reinterpret_cast<void*>(0x1000), reinterpret_cast<void*>(0x2000)};

constexpr uint64_t TRACED_FAILURE_DETAIL_ID = 0x5c2a7e91d3b4f086ull;

// Builds an exception with the parts a KjError has no field for: a detail record, a context, a
// remote trace and a stack trace.
kj::Exception tracedFailure() {
  kj::Exception e(kj::Exception::Type::DISCONNECTED, "peer.c++", 7, kj::str("the peer went away"));
  e.setDetail(TRACED_FAILURE_DETAIL_ID, kj::heapArray("detail"_kjb));
  e.wrapContext("caller.c++", 8, kj::str("while calling the peer"));
  e.setRemoteTrace(kj::str("remote frames"));
  for (auto frame: TRACED_FRAMES) {
    e.addTrace(frame);
  }
  return e;
}

void expectTracedFailure(const kj::Exception& e) {
  KJ_EXPECT(e.getType() == kj::Exception::Type::DISCONNECTED);
  KJ_EXPECT(e.getDescription() == "the peer went away");
  KJ_EXPECT(kj::StringPtr(e.getFile()) == "peer.c++");
  KJ_EXPECT(e.getLine() == 7);
  KJ_EXPECT(KJ_ASSERT_NONNULL(e.getDetail(TRACED_FAILURE_DETAIL_ID)) == "detail"_kjb);
  KJ_EXPECT(e.getRemoteTrace() == "remote frames");
  auto& context = KJ_ASSERT_NONNULL(e.getContext());
  KJ_EXPECT(kj::StringPtr(context.file) == "caller.c++");
  KJ_EXPECT(context.line == 8);
  KJ_EXPECT(context.description == "while calling the peer");
  // The promise machinery appends a frame for every continuation the exception passes through,
  // so the frames it started with are a prefix of its trace.
  auto trace = e.getStackTrace();
  KJ_ASSERT(trace.size() >= kj::size(TRACED_FRAMES));
  KJ_EXPECT(trace.first(kj::size(TRACED_FRAMES)) == kj::arrayPtr(TRACED_FRAMES));
}

// Fails every event with tracedFailure().
class FailingWorker final: public WorkerInterface {
 public:
  kj::Promise<void> request(kj::HttpMethod method,
      kj::StringPtr url,
      const kj::HttpHeaders& headers,
      kj::AsyncInputStream& requestBody,
      Response& response) override {
    return tracedFailure();
  }

  kj::Promise<void> connect(kj::StringPtr host,
      const kj::HttpHeaders& headers,
      kj::AsyncIoStream& connection,
      ConnectResponse& response,
      kj::HttpConnectSettings settings) override {
    return tracedFailure();
  }

  kj::Promise<void> prewarm(kj::StringPtr url) override {
    return tracedFailure();
  }

  kj::Promise<ScheduledResult> runScheduled(kj::Date scheduledTime, kj::StringPtr cron) override {
    return tracedFailure();
  }

  kj::Promise<AlarmResult> runAlarm(kj::Date scheduledTime, uint32_t retryCount) override {
    return tracedFailure();
  }

  kj::Promise<kj::Maybe<kj::Date>> abandonAlarm(kj::Date scheduledTime) override {
    return tracedFailure();
  }

  kj::Promise<CustomEvent::Result> customEvent(kj::Own<CustomEvent> event) override {
    return tracedFailure();
  }

  kj::Promise<bool> test() override {
    return tracedFailure();
  }
};

// The response and connection arguments of request() and connect(). The failing workers never
// use them.
class UnusedResponse final: public kj::HttpService::Response {
 public:
  kj::Own<kj::AsyncOutputStream> send(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    KJ_UNIMPLEMENTED();
  }
  kj::Own<kj::WebSocket> acceptWebSocket(const kj::HttpHeaders& headers) override {
    KJ_UNIMPLEMENTED();
  }
};

class UnusedConnectResponse final: public kj::HttpService::ConnectResponse {
 public:
  void accept(uint statusCode, kj::StringPtr statusText, const kj::HttpHeaders& headers) override {
    KJ_UNIMPLEMENTED();
  }
  kj::Own<kj::AsyncOutputStream> reject(uint statusCode,
      kj::StringPtr statusText,
      const kj::HttpHeaders& headers,
      kj::Maybe<uint64_t> expectedBodySize) override {
    KJ_UNIMPLEMENTED();
  }
};

class UnusedCustomEvent final: public workerd::WorkerInterface::CustomEvent {
 public:
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

// Runs `event` to completion and returns the exception it fails with.
template <typename Func>
kj::Exception failureOf(kj::WaitScope& waitScope, Func&& event) {
  return KJ_ASSERT_NONNULL(kj::runCatchingExceptions([&]() { event().wait(waitScope); }));
}

}  // namespace

KJ_TEST("cxx_worker hands back the wrapped C++ worker's exception whole") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto worker = kj::from<Rust>(new_cxx_worker(kj::heap<FailingWorker>()));
  kj::HttpHeaderTable headerTable;
  kj::HttpHeaders headers(headerTable);

  kj::NullStream body;
  UnusedResponse response;
  expectTracedFailure(failureOf(waitScope, [&]() {
    return worker->request(kj::HttpMethod::GET, "http://test/"_kj, headers, body, response);
  }));

  auto pipe = kj::newTwoWayPipe();
  UnusedConnectResponse connectResponse;
  expectTracedFailure(failureOf(waitScope, [&]() {
    return worker->connect(
        "example.com:443"_kj, headers, *pipe.ends[0], connectResponse, kj::HttpConnectSettings{});
  }));

  expectTracedFailure(failureOf(waitScope, [&]() { return worker->prewarm("/warm"_kj); }));
  expectTracedFailure(
      failureOf(waitScope, [&]() { return worker->runScheduled(kj::UNIX_EPOCH, "0 0 * * *"_kj); }));
  expectTracedFailure(failureOf(waitScope, [&]() { return worker->runAlarm(kj::UNIX_EPOCH, 0); }));
  expectTracedFailure(failureOf(waitScope, [&]() { return worker->abandonAlarm(kj::UNIX_EPOCH); }));
  expectTracedFailure(
      failureOf(waitScope, [&]() { return worker->customEvent(kj::heap<UnusedCustomEvent>()); }));
  expectTracedFailure(failureOf(waitScope, [&]() { return worker->test(); }));
}

// ======================================================================================
// PromisedInterface: a Rust Interface whose C++ target is still being started.

namespace {

// A custom event that passes the exception given to failed() on to `onFailed`.
class RecordingEvent final: public workerd::WorkerInterface::CustomEvent {
 public:
  explicit RecordingEvent(kj::Function<void(const kj::Exception&)> onFailed)
      : onFailed(kj::mv(onFailed)) {}

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
  void failed(const kj::Exception& e) override {
    onFailed(e);
  }
  uint16_t getType() override {
    return 42;
  }
  workerd::tracing::EventInfo getEventInfo() const override {
    return workerd::tracing::CustomEventInfo();
  }

 private:
  kj::Function<void(const kj::Exception&)> onFailed;
};

constexpr uint64_t START_FAILURE_DETAIL_ID = 0x8f3e1d2c4b5a6978ull;

// Builds a startup failure that uses every part of a kj::Exception: type, description, throw
// site, a detail record, a context, a remote trace and a stack trace. Detail records are how CPU,
// memory, wall-time and kill-switch failures are classified. Every event after the first gets a
// clone of this exception, so the test checks that a clone keeps all of it.
kj::Exception startFailure() {
  kj::Exception e(
      kj::Exception::Type::OVERLOADED, "start.c++", 123, kj::str("the worker could not start"));
  e.setDetail(START_FAILURE_DETAIL_ID, kj::heapArray("cpu"_kjb));
  e.wrapContext("loader.c++", 45, kj::str("while starting the worker"));
  e.setRemoteTrace(kj::str("remote start frames"));
  for (auto frame: TRACED_FRAMES) {
    e.addTrace(frame);
  }
  return e;
}

void expectStartFailure(const kj::Exception& e) {
  KJ_EXPECT(e.getType() == kj::Exception::Type::OVERLOADED);
  KJ_EXPECT(e.getDescription() == "the worker could not start");
  KJ_EXPECT(kj::StringPtr(e.getFile()) == "start.c++");
  KJ_EXPECT(e.getLine() == 123);
  KJ_EXPECT(KJ_ASSERT_NONNULL(e.getDetail(START_FAILURE_DETAIL_ID)) == "cpu"_kjb);
  KJ_EXPECT(e.getRemoteTrace() == "remote start frames");
  auto& context = KJ_ASSERT_NONNULL(e.getContext());
  KJ_EXPECT(kj::StringPtr(context.file) == "loader.c++");
  KJ_EXPECT(context.line == 45);
  KJ_EXPECT(context.description == "while starting the worker");
  auto trace = e.getStackTrace();
  KJ_ASSERT(trace.size() >= kj::size(TRACED_FRAMES));
  KJ_EXPECT(trace.first(kj::size(TRACED_FRAMES)) == kj::arrayPtr(TRACED_FRAMES));
}

}  // namespace

KJ_TEST("promised interface reports the startup failure in full to every later event") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto worker = newPromisedInterface(startFailure());

  // The first event reports the failure. Every later event must report the same exception in
  // full, not a copy with only the type and description.
  for (int attempt = 0; attempt < 2; attempt++) {
    auto exception = kj::runCatchingExceptions([&]() { worker->test().wait(waitScope); });
    expectStartFailure(KJ_ASSERT_NONNULL(exception));
  }
  auto exception =
      kj::runCatchingExceptions([&]() { worker->prewarm("/warm"_kj).wait(waitScope); });
  expectStartFailure(KJ_ASSERT_NONNULL(exception));
}

KJ_TEST("promised interface tells a customEvent the full startup failure") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto worker = newPromisedInterface(startFailure());
  bool failedCalled = false;

  auto exception = kj::runCatchingExceptions([&]() {
    worker
        ->customEvent(kj::heap<RecordingEvent>([&](const kj::Exception& e) {
      failedCalled = true;
      expectStartFailure(e);
    })).wait(waitScope);
  });

  expectStartFailure(KJ_ASSERT_NONNULL(exception));
  KJ_ASSERT(failedCalled);
}

KJ_TEST("promised interface rejects a customEvent with the exception its failed() throws") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto worker = newPromisedInterface(startFailure());

  // failed() is virtual, so it may throw. When it does, the event's promise rejects with that
  // exception instead of the startup failure.
  auto exception = kj::runCatchingExceptions([&]() {
    worker
        ->customEvent(kj::heap<RecordingEvent>([](const kj::Exception&) {
      KJ_FAIL_REQUIRE("the event could not be told");
    })).wait(waitScope);
  });

  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_EXPECT(e.getType() == kj::Exception::Type::FAILED);
  KJ_EXPECT(e.getDescription() == "the event could not be told");
}

KJ_TEST("promised interface delegates abandonAlarm before and after its target starts") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto paf = kj::newPromiseAndFulfiller<kj::Own<WorkerInterface>>();
  auto worker = newPromisedInterface(kj::mv(paf.promise));
  auto stub = kj::heap<StubWorker>();
  auto& stubRef = *stub;

  // Before the target has started, the call must wait for it instead of returning the no-op
  // default.
  auto pending = worker->abandonAlarm(kj::UNIX_EPOCH + 1000 * kj::SECONDS);
  KJ_ASSERT(!pending.poll(waitScope));
  KJ_ASSERT(!stubRef.abandonAlarmCalled);
  paf.fulfiller->fulfill(kj::mv(stub));
  auto stored = pending.wait(waitScope);
  KJ_ASSERT(stubRef.abandonAlarmCalled);
  KJ_ASSERT(stubRef.abandonedAlarmTime == kj::UNIX_EPOCH + 1000 * kj::SECONDS);
  KJ_ASSERT(KJ_ASSERT_NONNULL(stored) == StubWorker::STORED_ALARM_TIME);

  // Once started, calls go straight to the target.
  stubRef.abandonAlarmCalled = false;
  stored = worker->abandonAlarm(kj::UNIX_EPOCH + 2000 * kj::SECONDS).wait(waitScope);
  KJ_ASSERT(stubRef.abandonAlarmCalled);
  KJ_ASSERT(stubRef.abandonedAlarmTime == kj::UNIX_EPOCH + 2000 * kj::SECONDS);
  KJ_ASSERT(KJ_ASSERT_NONNULL(stored) == StubWorker::STORED_ALARM_TIME);
}

KJ_TEST("promised interface delegates bench before and after its target starts") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto paf = kj::newPromiseAndFulfiller<kj::Own<WorkerInterface>>();
  auto worker = newPromisedInterface(kj::mv(paf.promise));
  auto stub = kj::heap<StubWorker>();
  auto& stubRef = *stub;

  // Before the target has started, the call must wait for it instead of failing as the default
  // does.
  BenchCall pendingCall;
  auto pending = pendingCall.run(*worker);
  KJ_ASSERT(!pending.poll(waitScope));
  KJ_ASSERT(!stubRef.benchCalled);
  paf.fulfiller->fulfill(kj::mv(stub));
  pending.wait(waitScope);
  expectBenchForwarded(stubRef, pendingCall);

  // Once started, calls go straight to the target.
  stubRef.benchCalled = false;
  BenchCall call;
  call.run(*worker).wait(waitScope);
  expectBenchForwarded(stubRef, call);
}

KJ_TEST("a Rust interface's default bench fails as unimplemented") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto worker = kj::from<Rust>(new_ok_worker());
  BenchCall call;
  auto exception = kj::runCatchingExceptions([&]() { call.run(*worker).wait(waitScope); });
  auto& e = KJ_ASSERT_NONNULL(exception);
  KJ_EXPECT(e.getType() == kj::Exception::Type::UNIMPLEMENTED);
  KJ_EXPECT(e.getDescription() == "this service doesn't support bench()");
  // The report is left as the caller made it.
  KJ_EXPECT(!call.report.hasCases());
  KJ_EXPECT(!call.report.hasError());
}

KJ_TEST("a Rust interface's default abandonAlarm reports no stored alarm") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto worker = kj::from<Rust>(new_ok_worker());
  KJ_ASSERT(worker->abandonAlarm(kj::UNIX_EPOCH).wait(waitScope) == kj::none);
}

// ======================================================================================
// failure worker

// Every event of a WorkerInterface but prewarm() and customEvent(), each called on a worker of
// its own.
kj::Vector<kj::Function<void(WorkerInterface&, kj::WaitScope&)>> failingEvents() {
  kj::Vector<kj::Function<void(WorkerInterface&, kj::WaitScope&)>> events;
  events.add([](WorkerInterface& worker, kj::WaitScope& waitScope) {
    kj::HttpHeaderTable headerTable;
    kj::HttpHeaders headers(headerTable);
    kj::NullStream body;
    UnusedResponse response;
    worker.request(kj::HttpMethod::GET, "http://test/"_kj, headers, body, response).wait(waitScope);
  });
  events.add([](WorkerInterface& worker, kj::WaitScope& waitScope) {
    kj::HttpHeaderTable headerTable;
    kj::HttpHeaders headers(headerTable);
    auto pipe = kj::newTwoWayPipe();
    UnusedConnectResponse response;
    worker.connect("example.com:443"_kj, headers, *pipe.ends[0], response, {}).wait(waitScope);
  });
  events.add([](WorkerInterface& worker, kj::WaitScope& waitScope) {
    worker.runScheduled(kj::UNIX_EPOCH, "* * * * *"_kj).wait(waitScope);
  });
  events.add([](WorkerInterface& worker, kj::WaitScope& waitScope) {
    worker.runAlarm(kj::UNIX_EPOCH, 0).wait(waitScope);
  });
  events.add([](WorkerInterface& worker, kj::WaitScope& waitScope) {
    worker.abandonAlarm(kj::UNIX_EPOCH).wait(waitScope);
  });
  return events;
}

// Expects `worker` to tell a custom event tracedFailure() and then fail with it.
void expectCustomEventFails(WorkerInterface& worker, kj::WaitScope& waitScope) {
  bool failedCalled = false;
  auto exception = kj::runCatchingExceptions([&]() {
    worker
        .customEvent(kj::heap<RecordingEvent>([&](const kj::Exception& e) {
      failedCalled = true;
      expectTracedFailure(e);
    })).wait(waitScope);
  });
  expectTracedFailure(KJ_ASSERT_NONNULL(exception));
  KJ_EXPECT(failedCalled);
}

KJ_TEST("failure worker fails every event with the whole exception, every time") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto worker = kj::from<Rust>(new_failure_worker(tracedFailure()));
  for (int attempt = 0; attempt < 2; attempt++) {
    for (auto& event: failingEvents()) {
      expectTracedFailure(
          KJ_ASSERT_NONNULL(kj::runCatchingExceptions([&]() { event(*worker, waitScope); })));
    }
    expectTracedFailure(
        KJ_ASSERT_NONNULL(kj::runCatchingExceptions([&]() { worker->test().wait(waitScope); })));
    expectCustomEventFails(*worker, waitScope);
  }
  worker->prewarm("/warm"_kj).wait(waitScope);
}

KJ_TEST("failure worker rejects a customEvent with the exception its failed() throws") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  auto worker = kj::from<Rust>(new_failure_worker(tracedFailure()));
  auto exception = kj::runCatchingExceptions([&]() {
    worker
        ->customEvent(kj::heap<RecordingEvent>([](const kj::Exception&) {
      KJ_FAIL_REQUIRE("failed() threw");
    })).wait(waitScope);
  });
  KJ_EXPECT(KJ_ASSERT_NONNULL(exception).getDescription().contains("failed() threw"));
}

// Holds for both implementations of fromException(): the Rust worker when the
// RUST_FAILURE_WORKER_INTERFACE autogate is enabled, as in the @all-autogates variant, and the C++
// one otherwise. The C++ one gives its exception away at its first failure, so each event gets a
// worker of its own.
KJ_TEST("WorkerInterface::fromException() fails each event with the whole exception") {
  kj::EventLoop loop;
  kj::WaitScope waitScope(loop);

  for (auto& event: failingEvents()) {
    auto worker = WorkerInterface::fromException(tracedFailure());
    expectTracedFailure(
        KJ_ASSERT_NONNULL(kj::runCatchingExceptions([&]() { event(*worker, waitScope); })));
  }
  expectCustomEventFails(*WorkerInterface::fromException(tracedFailure()), waitScope);
  WorkerInterface::fromException(tracedFailure())->prewarm("/warm"_kj).wait(waitScope);
}

}  // namespace
}  // namespace workerd
