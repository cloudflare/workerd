// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#pragma once

#include <workerd/io/io-context.h>

#include <rust/cxx.h>

#include <kj/async.h>

#include <cstdint>

// The fibers that ffi.rs runs lol-html on, so that a handler can wait for JavaScript's promise
// while lol-html, which calls handlers synchronously, waits for the handler.
//
// lol-html runs on a fiber of a pool, with the stack size C++ HTMLRewriter uses. Each handler runs
// JavaScript on the thread's main stack, in IoContext::run(), since V8 checks the stack limit of
// the main stack, and the fiber waits until the handler is done. Only lol-html and Rust code that
// does not enter V8 run on the fiber.
//
// No exception unwinds through Rust frames: a KJ exception, including the kj::CanceledException
// with which KJ cancels a fiber that is waiting, is caught where it is thrown and kept in the
// FiberWait. Rust takes a failure with take_wait_error(), and C++ rethrows a cancellation once
// Rust has returned.
namespace workerd::api::html_rewriter {

// Rust types, defined in ffi.rs.
struct FiberTask;
struct MainStackCall;
enum class MainStackOutcome : uint8_t;
enum class WaitOutcome : uint8_t;

// What a fiber that runs a FiberTask can wait for, and why it stopped waiting.
class FiberWait {
 public:
  FiberWait(kj::WaitScope& waitScope, IoContext& context): waitScope(waitScope), context(context) {}
  KJ_DISALLOW_COPY_AND_MOVE(FiberWait);

  // Rethrows the exception that stopped a wait, if one did and Rust has not taken it.
  void rethrow();

 private:
  kj::WaitScope& waitScope;
  IoContext& context;
  kj::Maybe<kj::Exception> exception;
  bool canceled = false;

  friend WaitOutcome run_on_main_stack(FiberWait& wait, MainStackCall& call);
  friend void take_wait_error(FiberWait& wait);
};

// The fulfiller of what a fiber waits for: Rust fulfills it when the handler is done, and
// dropping it unfulfilled ends the wait with an error. The reactions to a handler's promise, which
// hold it, may run on any thread that holds the isolate lock, so the fulfiller is a cross-thread
// one, which any thread may fulfill or destroy.
class Settler {
 public:
  explicit Settler(kj::Own<kj::CrossThreadPromiseFulfiller<void>> fulfiller)
      : fulfiller(kj::mv(fulfiller)) {}

  void settle() const {
    fulfiller->fulfill();
  }

 private:
  kj::Own<kj::CrossThreadPromiseFulfiller<void>> fulfiller;
};

// Runs `task` on a fiber, as a task of the current request, once the event loop next runs, so
// that a request canceled before then starts no fiber. Canceling the request cancels the fiber.
// The task tells Rust when it is done.
void start_fiber(::rust::Box<FiberTask> task);

// From the fiber: runs `call` on the main stack, in the IoContext of the fiber's task, and waits
// until it settles its Settler. Returns WaitOutcome::Stopped without running `call` once a wait
// has failed or been canceled, and when this one is.
WaitOutcome run_on_main_stack(FiberWait& wait, MainStackCall& call);

// Throws the KJ exception that stopped a wait, other than a cancellation, if one did, so that Rust
// gets it as an error; the FiberWait then no longer has it.
void take_wait_error(FiberWait& wait);

// Settler::settle(), for Rust.
void settle(kj::Own<Settler> settler);

}  // namespace workerd::api::html_rewriter
