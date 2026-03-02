// Copyright (c) 2025 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "container.h"

#include <workerd/io/container.capnp.h>
#include <workerd/io/io-context.h>
#include <workerd/tests/test-fixture.h>

#include <kj/test.h>

namespace workerd::api {
namespace {

// Mock rpc::Container::Server that gives tests control over monitor() resolution.
// Each monitor() RPC creates a PendingMonitor that the test fulfills or rejects.
class MockContainerServer final: public rpc::Container::Server {
 public:
  struct PendingMonitor {
    kj::Own<kj::PromiseFulfiller<void>> fulfiller;
    capnp::CallContext<rpc::Container::MonitorParams, rpc::Container::MonitorResults> context;
  };

  kj::Vector<PendingMonitor> pendingMonitors;
  uint startCount = 0;
  uint monitorCount = 0;
  uint destroyCount = 0;

  kj::Promise<void> status(StatusContext) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> start(StartContext) override {
    ++startCount;
    return kj::READY_NOW;
  }
  kj::Promise<void> monitor(MonitorContext context) override {
    ++monitorCount;
    auto paf = kj::newPromiseAndFulfiller<void>();
    pendingMonitors.add(PendingMonitor{kj::mv(paf.fulfiller), kj::mv(context)});
    return kj::mv(paf.promise);
  }
  kj::Promise<void> destroy(DestroyContext) override {
    ++destroyCount;
    return kj::READY_NOW;
  }
  kj::Promise<void> signal(SignalContext) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> getTcpPort(GetTcpPortContext) override {
    KJ_UNIMPLEMENTED("not tested");
  }
  kj::Promise<void> listenTcp(ListenTcpContext) override {
    KJ_UNIMPLEMENTED("not tested");
  }
  kj::Promise<void> setInactivityTimeout(SetInactivityTimeoutContext) override {
    return kj::READY_NOW;
  }
  kj::Promise<void> setEgressHttp(SetEgressHttpContext) override {
    return kj::READY_NOW;
  }

  void fulfillMonitor(uint index, int32_t exitCode = 0) {
    KJ_ASSERT(index < pendingMonitors.size());
    pendingMonitors[index].context.getResults().setExitCode(exitCode);
    pendingMonitors[index].fulfiller->fulfill();
  }

  void rejectMonitor(uint index, kj::Exception&& error) {
    KJ_ASSERT(index < pendingMonitors.size());
    pendingMonitors[index].fulfiller->reject(kj::mv(error));
  }
};

kj::Promise<void> yieldEventLoop(uint n = 1) {
  for (uint i = 0; i < n; ++i) {
    co_await kj::evalLater([]() {});
  }
}

// ===========================================================================
// Verifies the core auto-abort safety net: when start() is called but the
// user never calls monitor(), and the container exits with an RPC error
// (crash, OOM, infrastructure failure), the background monitor automatically
// aborts the IoContext. This ensures the Durable Object is notified of
// container death even when user code neglects to call monitor().
//
// Sequence:
//   1. Container is created with running=false, then start() is called
//   2. start() issues a start RPC and calls monitorOnBackgroundIfNeeded()
//   3. monitorOnBackgroundIfNeeded() issues a monitor RPC and sets up a
//      background promise that will abort on error
//   4. The mock rejects the monitor RPC with an error
//   5. The background error handler sees monitoringExplicitly=false and
//      calls ioContext.abort() via the weak reference
//   6. context.onAbort() fires with the container error
//
// Covers: monitorOnBackgroundIfNeeded() error path, auto-abort via weak ref
// ===========================================================================
KJ_TEST("Container: start without monitor auto-aborts on error") {
  TestFixture fixture;

  fixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;
    auto& context = env.context;

    auto mockServer = kj::heap<MockContainerServer>();
    auto& mock = *mockServer;
    rpc::Container::Client client = kj::mv(mockServer);

    auto container = js.alloc<Container>(kj::mv(client), false);
    container->start(js, kj::none);
    KJ_ASSERT(container->getRunning());

    auto abortPromise = context.onAbort();
    // The monitor RPC is chained after the start RPC completes, so we need
    // enough event-loop turns for both to resolve.
    co_await yieldEventLoop(3);

    KJ_ASSERT(mock.startCount == 1);
    KJ_ASSERT(mock.monitorCount == 1);

    mock.rejectMonitor(0, KJ_EXCEPTION(FAILED, "container crashed unexpectedly"));
    co_await yieldEventLoop(3);

    co_await abortPromise.then(
        []() { KJ_FAIL_ASSERT("abort promise should have been rejected"); }, [](kj::Exception&& e) {
      KJ_EXPECT(e.getDescription().contains("container crashed unexpectedly"));
    });
  }, kj::arr("container crashed unexpectedly"_kj));
}

// ===========================================================================
// Verifies that a clean container exit (exit code 0) without an explicit
// monitor() call does NOT abort the IoContext. Only RPC errors should
// trigger auto-abort. A clean exit is normal and should not disrupt the DO.
//
// This exercises the background monitor's success handler (.then branch),
// which stores finished=true and exitCode but does NOT call abort(). The
// test passes if runInIoContext() completes without throwing — no
// errorsToIgnore is provided, so any abort would fail the test.
//
// Covers: monitorOnBackgroundIfNeeded() success path, MonitorState storage
// ===========================================================================
KJ_TEST("Container: clean exit without monitor does not abort") {
  TestFixture fixture;

  fixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;

    auto mockServer = kj::heap<MockContainerServer>();
    auto& mock = *mockServer;
    rpc::Container::Client client = kj::mv(mockServer);

    auto container = js.alloc<Container>(kj::mv(client), false);
    container->start(js, kj::none);
    KJ_ASSERT(container->getRunning());

    co_await yieldEventLoop(3);
    KJ_ASSERT(mock.monitorCount == 1);

    mock.fulfillMonitor(0, 0);
    co_await yieldEventLoop(3);
  });
}

// ===========================================================================
// Verifies that the Container constructor sets up background monitoring when
// created with running=true (the case where the DO starts up and discovers
// a pre-existing running container). The constructor calls
// monitorOnBackgroundIfNeeded() directly, so if the container then crashes,
// the IoContext is auto-aborted just as it would be for start().
//
// Key assertions:
//   - No start RPC is issued (the container was already running)
//   - A monitor RPC IS issued (from the constructor's background setup)
//   - Auto-abort fires when the monitor RPC fails
//
// Covers: Constructor running=true path, monitorOnBackgroundIfNeeded()
//         called from constructor
// ===========================================================================
KJ_TEST("Container: constructor with running=true sets up background monitoring") {
  TestFixture fixture;

  fixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;
    auto& context = env.context;

    auto mockServer = kj::heap<MockContainerServer>();
    auto& mock = *mockServer;
    rpc::Container::Client client = kj::mv(mockServer);

    auto container = js.alloc<Container>(kj::mv(client), true);
    KJ_ASSERT(container->getRunning());

    auto abortPromise = context.onAbort();
    // The monitor RPC is issued via a .then() chain (even with READY_NOW
    // prerequisite), so we need extra event-loop turns for it to arrive.
    co_await yieldEventLoop(3);

    KJ_ASSERT(mock.startCount == 0);
    KJ_ASSERT(mock.monitorCount == 1);

    mock.rejectMonitor(0, KJ_EXCEPTION(FAILED, "pre-existing container died"));
    co_await yieldEventLoop(2);

    co_await abortPromise.then(
        []() { KJ_FAIL_ASSERT("abort promise should have been rejected"); }, [](kj::Exception&& e) {
      KJ_EXPECT(e.getDescription().contains("pre-existing container died"));
    });
  }, kj::arr("pre-existing container died"_kj));
}

// ===========================================================================
// Verifies that calling monitor() before the container exits prevents the
// background auto-abort. The monitoringExplicitly flag is set to true when
// monitor() is called. The background error handler checks this flag and
// skips the abort() call, deferring error delivery to the JS promise from
// monitor() instead.
//
// Sequence:
//   1. start() → background monitor set up
//   2. monitor() → sets monitoringExplicitly=true, sends its own independent
//      monitor RPC via awaitIo (because background hasn't finished yet)
//   3. Both RPCs arrive at the mock (monitorCount == 2): index 0 is the
//      background monitor, index 1 is the explicit monitor
//   4. Both are rejected with the same error
//   5. Background error handler sees monitoringExplicitly=true → no abort
//   6. The awaitIo branch delivers the error to handleError → running=false
//
// The test passes without errorsToIgnore, proving the IoContext was NOT
// aborted. running=false proves handleError fired correctly.
//
// Covers: monitoringExplicitly flag, background error handler skip path,
//         handleError lambda through awaitIo, two independent monitor RPCs
// ===========================================================================
KJ_TEST("Container: explicit monitor prevents auto-abort on error") {
  TestFixture fixture;
  KJ_EXPECT_LOG(ERROR, "container crashed");

  fixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;
    auto& context = env.context;

    auto mockServer = kj::heap<MockContainerServer>();
    auto& mock = *mockServer;
    rpc::Container::Client client = kj::mv(mockServer);

    auto container = js.alloc<Container>(kj::mv(client), false);
    container->start(js, kj::none);
    static_cast<void>(container->monitor(js));

    co_await yieldEventLoop(3);
    // Two independent monitor RPCs: background (index 0) + explicit (index 1).
    KJ_ASSERT(mock.monitorCount == 2);

    mock.rejectMonitor(0, KJ_EXCEPTION(FAILED, "container crashed"));
    mock.rejectMonitor(1, KJ_EXCEPTION(FAILED, "container crashed"));
    co_await yieldEventLoop(5);

    co_await context.run([&](Worker::Lock& lock) { KJ_ASSERT(!container->getRunning()); });
  });
}

// ===========================================================================
// Tests the hibernation-friendly immediate-result path in monitor(). When
// the background monitor has already completed (container exited) before
// the user calls monitor(), the method returns an immediately-resolved JS
// promise via js.resolvedPromise().then() instead of calling awaitIo().
// This is critical because awaitIo() calls registerPendingEvent(), which
// blocks Durable Object hibernation.
//
// Sequence:
//   1. start() → background monitor set up, monitor RPC issued
//   2. Container exits cleanly with exit code 0
//   3. Background success handler fires: finished=true, exitCode=0
//   4. User calls monitor() after the background has already completed
//   5. monitor() sees finished=true, exception=none → immediate path
//   6. handleExitCode fires with exitCode=0 → running=false, no throw
//
// Key assertion: monitorCount stays at 1 (no new RPC issued by monitor())
//
// Covers: monitorState->finished immediate success path, handleExitCode
//         with exit code 0
// ===========================================================================
KJ_TEST("Container: monitor after clean exit returns immediate result") {
  TestFixture fixture;

  fixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;
    auto& context = env.context;

    auto mockServer = kj::heap<MockContainerServer>();
    auto& mock = *mockServer;
    rpc::Container::Client client = kj::mv(mockServer);

    auto container = js.alloc<Container>(kj::mv(client), false);
    container->start(js, kj::none);

    co_await yieldEventLoop(3);
    KJ_ASSERT(mock.monitorCount == 1);

    mock.fulfillMonitor(0, 0);
    co_await yieldEventLoop(3);

    co_await context.run([&](Worker::Lock& lock) {
      auto& js2 = jsg::Lock::from(lock.getIsolate());
      static_cast<void>(container->monitor(js2));
    });

    KJ_ASSERT(mock.monitorCount == 1);
    co_await yieldEventLoop(2);

    co_await context.run([&](Worker::Lock& lock) { KJ_ASSERT(!container->getRunning()); });
  });
}

// ===========================================================================
// Tests the hibernation-friendly immediate-result path when the background
// monitor completed with an RPC error. When monitor() is called after the
// background already stored an exception in MonitorState, it returns
// js.rejectedPromise().catch_(handleError) instead of awaitIo(). The
// handleError lambda sets running=false and re-throws the error.
//
// Because monitoringExplicitly was false when the background fired, the
// auto-abort has already happened. The test uses errorsToIgnore for the
// abort. The key verification is that monitor()'s immediate error path
// still works correctly after the abort: handleError fires, running=false.
//
// Sequence:
//   1. start() → background monitor set up
//   2. Container crashes (RPC error) — auto-abort fires
//   3. Background error handler: finished=true, exception=error, abort
//   4. User calls monitor() on the (aborted) IoContext
//   5. monitor() sees finished=true, exception=some → immediate error path
//   6. handleError fires → running=false
//
// Covers: monitorState->finished immediate error path, handleError lambda
//         via immediate rejection
// ===========================================================================
KJ_TEST("Container: monitor after error returns immediate result") {
  TestFixture fixture;

  fixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;
    auto& context = env.context;

    auto mockServer = kj::heap<MockContainerServer>();
    auto& mock = *mockServer;
    rpc::Container::Client client = kj::mv(mockServer);

    auto container = js.alloc<Container>(kj::mv(client), false);
    container->start(js, kj::none);

    co_await yieldEventLoop(3);
    KJ_ASSERT(mock.monitorCount == 1);

    mock.rejectMonitor(0, KJ_EXCEPTION(FAILED, "container exploded"));
    co_await yieldEventLoop(3);

    co_await context.run([&](Worker::Lock& lock) {
      auto& js2 = jsg::Lock::from(lock.getIsolate());
      static_cast<void>(container->monitor(js2));
    });

    KJ_ASSERT(mock.monitorCount == 1);
    co_await yieldEventLoop(2);

    co_await context.run([&](Worker::Lock& lock) { KJ_ASSERT(!container->getRunning()); });
  }, kj::arr("container exploded"_kj));
}

// ===========================================================================
// Tests the non-zero exit code path through monitor()'s handleExitCode
// lambda via the awaitIo path (container still running when monitor() is
// called). When the container exits with a non-zero code, handleExitCode
// creates a JS Error with an exitCode property and throws it, rejecting
// the monitor() promise.
//
// Sequence:
//   1. start() → background monitor set up
//   2. monitor() → monitoringExplicitly=true, sends its own independent
//      monitor RPC via awaitIo (because background hasn't finished yet)
//   3. Both RPCs arrive (monitorCount == 2): background (0) + explicit (1)
//   4. Both are fulfilled with exit code 42
//   5. Background success handler stores exitCode=42 in MonitorState
//   6. Explicit monitor's handleExitCode fires with 42 → running=false
//
// The test verifies running=false (handler fired) and that no auto-abort
// occurs (monitoringExplicitly was true).
//
// Covers: handleExitCode with exitCode != 0, two independent monitor RPCs
// ===========================================================================
KJ_TEST("Container: non-zero exit code through monitor rejects promise") {
  TestFixture fixture;

  fixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;
    auto& context = env.context;

    auto mockServer = kj::heap<MockContainerServer>();
    auto& mock = *mockServer;
    rpc::Container::Client client = kj::mv(mockServer);

    auto container = js.alloc<Container>(kj::mv(client), false);
    container->start(js, kj::none);
    static_cast<void>(container->monitor(js));

    co_await yieldEventLoop(3);
    // Two independent monitor RPCs: background (index 0) + explicit (index 1).
    KJ_ASSERT(mock.monitorCount == 2);

    mock.fulfillMonitor(0, 42);
    mock.fulfillMonitor(1, 42);
    co_await yieldEventLoop(5);

    co_await context.run([&](Worker::Lock& lock) { KJ_ASSERT(!container->getRunning()); });
  });
}

// ===========================================================================
// Verifies that a Container can complete a full lifecycle (start → monitor
// → exit) and then be restarted for a second identical cycle. This is the
// critical test for the state reset logic in start().
//
// When start() is called for the second time, it discards stale promises
// (monitorJsPromise, backgroundMonitor) and creates a
// fresh MonitorState. The old MonitorState is intentionally orphaned so
// that any in-flight background continuation from the first cycle writes
// to a dead object rather than corrupting the new run's state.
//
// Each cycle issues two independent monitor RPCs (background + explicit),
// so monitorCount increments by 2 per cycle.
//
// Assertions per cycle:
//   - startCount increments by 1 (fresh start RPC)
//   - monitorCount increments by 2 (background + explicit RPCs)
//   - running transitions true → false after exit
//   - Second cycle works identically to the first
//
// Covers: start() fresh MonitorState allocation, monitorOnBackgroundIfNeeded()
//         re-invocation, full two-cycle lifecycle
// ===========================================================================
KJ_TEST("Container: start-monitor-exit-restart-monitor lifecycle") {
  TestFixture fixture;

  fixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;
    auto& context = env.context;

    auto mockServer = kj::heap<MockContainerServer>();
    auto& mock = *mockServer;
    rpc::Container::Client client = kj::mv(mockServer);

    auto container = js.alloc<Container>(kj::mv(client), false);

    container->start(js, kj::none);
    KJ_ASSERT(container->getRunning());
    static_cast<void>(container->monitor(js));

    co_await yieldEventLoop(3);
    KJ_ASSERT(mock.startCount == 1);
    // Two independent monitor RPCs: background (index 0) + explicit (index 1).
    KJ_ASSERT(mock.monitorCount == 2);

    // Fulfill both RPCs for cycle 1.
    mock.fulfillMonitor(0, 0);
    mock.fulfillMonitor(1, 0);
    co_await yieldEventLoop(5);

    co_await context.run([&](Worker::Lock& lock) {
      auto& js2 = jsg::Lock::from(lock.getIsolate());
      KJ_ASSERT(!container->getRunning());

      container->start(js2, kj::none);
      KJ_ASSERT(container->getRunning());
      static_cast<void>(container->monitor(js2));
    });

    co_await yieldEventLoop(3);
    KJ_ASSERT(mock.startCount == 2);
    // Two more RPCs for cycle 2: background (index 2) + explicit (index 3).
    KJ_ASSERT(mock.monitorCount == 4);

    mock.fulfillMonitor(2, 0);
    mock.fulfillMonitor(3, 0);
    co_await yieldEventLoop(5);

    co_await context.run([&](Worker::Lock& lock) { KJ_ASSERT(!container->getRunning()); });
  });
}

// ===========================================================================
// Verifies that calling destroy() while the background monitor is actively
// running does not abort the IoContext and correctly cancels the background
// monitor. Per the capnp schema: "If a call to monitor() is waiting when
// destroy() is invoked, monitor() will also return (with no error)."
//
// Sequence:
//   1. start() → background monitor set up (monitor RPC in flight)
//   2. destroy() → running=false, backgroundMonitor canceled, destroy RPC
//   3. The background monitor branch is dropped (no abort)
//   4. The container is cleanly destroyed
//
// The test passes without errorsToIgnore, proving no abort occurred.
//
// Covers: destroy() canceling backgroundMonitor, no spurious auto-abort
// ===========================================================================
KJ_TEST("Container: destroy while background monitor is active does not abort") {
  TestFixture fixture;

  fixture.runInIoContext([](const TestFixture::Environment& env) -> kj::Promise<void> {
    auto& js = env.js;
    auto& context = env.context;

    auto mockServer = kj::heap<MockContainerServer>();
    auto& mock = *mockServer;
    rpc::Container::Client client = kj::mv(mockServer);

    auto container = js.alloc<Container>(kj::mv(client), false);
    container->start(js, kj::none);
    KJ_ASSERT(container->getRunning());

    co_await yieldEventLoop(3);
    KJ_ASSERT(mock.startCount == 1);
    KJ_ASSERT(mock.monitorCount == 1);

    // Destroy while the background monitor's monitor RPC is pending.
    co_await context.run([&](Worker::Lock& lock) {
      auto& js2 = jsg::Lock::from(lock.getIsolate());
      static_cast<void>(container->destroy(js2, kj::none));
    });

    co_await yieldEventLoop(3);

    KJ_ASSERT(mock.destroyCount == 1);
    KJ_ASSERT(!container->getRunning());
  });
}

}  // namespace
}  // namespace workerd::api
