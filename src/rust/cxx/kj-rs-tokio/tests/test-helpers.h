#pragma once

#include <kj/async.h>

namespace kj_rs_tokio_test {

// A KJ timer promise from the test's current TokioAsyncIoContext (the C++ test installs the
// timer before spawning). Lets a Rust task spawned on the loop's LocalSet hold a live KJ promise
// (a TimerPromiseAdapter registered with the port's kj::TimerImpl, plus an armed
// RustPromiseAwaiter Event) across context teardown.
kj::Promise<void> kjTimerDelay(uint64_t ms);

// kj::NEVER_DONE, so a spawned Rust task can hold a live OwnPromiseNode + armed
// RustPromiseAwaiter event across context teardown.
kj::Promise<void> kjNeverPromise();

// Re-enters promise.wait() on the test's current WaitScope (installed by the C++ test). From a
// spawned task this nests block_on inside the port's block_on; the failure must surface as a
// kj::Exception (which the bridge turns into a Rust Err), never an abort.
void nestedWait();

// Fulfills the kj::PromiseFulfiller<int> the current C++ test installed (setTestFulfiller).
// Called from a spawned tokio task while the loop is parked: arms a KJ event by a means other than
// a bridged waker.
void fulfillTestFulfiller(int32_t value);

}  // namespace kj_rs_tokio_test
