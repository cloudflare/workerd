#pragma once

#include "kj-rs-tokio/tokio-event-port.h"

#include <kj/async.h>

// The C++ helpers of both suites: the KJ-driven KJ_TESTs (tokio-event-port-test.c++) and the
// tokio-driven Rust tests (runtime_tests.rs). Each test installs the loop objects its helpers
// need (the timer, the WaitScope, a fulfiller) before it uses them.
namespace kj_rs_tokio_test {

// The calling thread's current timer / WaitScope for the helpers below (one per thread, as
// loops are); nullptr clears.
void setTestTimer(kj::Timer *timer);
void setTestWaitScope(kj::WaitScope *ws);
// Both at once, from a tokio-driven context (installTestContext), or neither.
void installTestContext(kj_rs_tokio::TokioAsyncIoContext &context);
void clearTestContext();

// A KJ timer promise from the test's current timer. Lets a Rust task spawned on the loop's
// LocalSet hold a live KJ promise (a TimerPromiseAdapter registered with the port's
// kj::TimerImpl, plus an armed RustPromiseAwaiter Event) across context teardown.
kj::Promise<void> kjTimerDelay(uint64_t ms);

// kj::NEVER_DONE, so a spawned Rust task can hold a live OwnPromiseNode + armed
// RustPromiseAwaiter event across context teardown.
kj::Promise<void> kjNeverPromise();

// Re-enters promise.wait() on the test's current WaitScope, on a short timer from the test's
// current timer so that wait() has to sleep. KJ-driven, from a spawned task this nests block_on
// inside the port's block_on; tokio-driven, the port refuses. Either failure must surface as a
// kj::Exception (which the bridge turns into a Rust Err), never an abort.
void nestedWait();

// A promise whose kj::PromiseFulfiller<int> the helpers keep (setTestFulfiller /
// fulfillTestFulfiller). A spawned tokio task fulfilling it arms a KJ event by a means other than
// a bridged waker.
void setTestFulfiller(kj::Maybe<kj::Own<kj::PromiseFulfiller<int>>> fulfiller);
kj::Promise<int32_t> testFulfillerPromise();
void fulfillTestFulfiller(int32_t value);

// Cross-thread paths into this loop, each from a plain kj::Thread started by the call: the
// thread runs `value * 2` on this loop through kj::Executor::executeSync() and reports the result
// through a cross-thread fulfiller; or fulfills the cross-thread fulfiller directly, after
// `delayMs`. Both need the loop to drain its Executor while parked.
kj::Promise<int32_t> executeSyncFromThread(int32_t value);
kj::Promise<int32_t> crossThreadFulfillFromThread(uint64_t delayMs, int32_t value);

// Two loops calling into each other: each publishes its Executor in a slot (0 or 1) and executes
// on the peer's. executeAsyncOn() blocks until the slot has been published.
void publishExecutor(uint8_t slot);
void clearExecutor(uint8_t slot);
kj::Promise<int32_t> executeAsyncOn(uint8_t slot, int32_t value);

// kj::yieldUntilWouldSleep().
kj::Promise<void> kjYieldUntilWouldSleep();

// A KJ coroutine that co_awaits a bridged Rust future (the test crate's tokio_sleep_on_runtime).
kj::Promise<void> kjAwaitsRustSleep(uint64_t ms);

}  // namespace kj_rs_tokio_test
