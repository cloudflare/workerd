// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "fiber.h"

#include <workerd/api/html-rewriter/ffi.rs.h>
#include <workerd/io/worker.h>

namespace workerd::api::html_rewriter {

namespace {

#if KJ_HAS_COMPILER_FEATURE(address_sanitizer) || defined(__SANITIZE_ADDRESS__)
// encoding_rs decoder frames can take over 60 KiB in ASan debug builds. This leaves room for the
// rest of the rewriter's stack too.
constexpr size_t FIBER_STACK_SIZE = 256 * 1024;
#else
// The smallest stack KJ gives a fiber.
constexpr size_t FIBER_STACK_SIZE = 64 * 1024;
#endif

const kj::FiberPool& fiberPool() {
  static const kj::FiberPool pool(FIBER_STACK_SIZE);
  return pool;
}

}  // namespace

void FiberWait::rethrow() {
  if (canceled) {
    throw kj::CanceledException{};
  }
  KJ_IF_SOME(e, exception) {
    kj::throwFatalException(kj::mv(e));
  }
}

void start_fiber(::rust::Box<FiberTask> task) {
  auto& context = IoContext::current();
  // A fiber that has not started can't be destroyed, so it starts in a later turn of the event
  // loop, and then runs at once. Until then, canceling the task drops only the FiberTask. The
  // IoContext owns the task, so it outlives the fiber.
  context.addTask(kj::evalLater([&context, task = kj::mv(task)]() mutable {
    return fiberPool().startFiber(
        [&context, task = kj::mv(task)](kj::WaitScope& waitScope) mutable {
      FiberWait wait(waitScope, context);
      fiber_task_run(*task, wait);
      wait.rethrow();
    });
  }));
}

WaitOutcome run_on_main_stack(FiberWait& wait, MainStackCall& call) {
  if (wait.canceled || wait.exception != kj::none) {
    return WaitOutcome::Stopped;
  }
  try {
    KJ_IF_SOME(exception, kj::runCatchingExceptions([&]() {
      auto settled = kj::newPromiseAndCrossThreadFulfiller<void>();
      // evalLater() returns to the event loop, on the main stack, which IoContext::run() then
      // runs the call on.
      auto ran = kj::evalLater([&]() {
        return wait.context.run([&](Worker::Lock& lock) {
          auto isolate = reinterpret_cast<uintptr_t>(lock.getIsolate());
          switch (
              main_stack_call_run(isolate, call, kj::heap<Settler>(kj::mv(settled.fulfiller)))) {
            case MainStackOutcome::Dispatched:
              return;
            case MainStackOutcome::Terminated:
              // The termination fails run() in its microtask checkpoint, and otherwise the
              // dropped Settler fails the wait.
              return;
            case MainStackOutcome::Panicked:
              KJ_FAIL_ASSERT("an HTMLRewriter handler call panicked");
          }
          KJ_UNREACHABLE;
        });
      });
      ran.then([promise = kj::mv(settled.promise)]() mutable {
        return kj::mv(promise);
      }).wait(wait.waitScope);
    })) {
      wait.exception = kj::mv(exception);
      return WaitOutcome::Stopped;
    }
  } catch (kj::CanceledException) {
    // KJ is destroying the fiber's promise. lol-html has to return before the fiber can unwind,
    // so the exception is rethrown then.
    wait.canceled = true;
    return WaitOutcome::Stopped;
  }
  return WaitOutcome::Continue;
}

void take_wait_error(FiberWait& wait) {
  KJ_IF_SOME(e, wait.exception) {
    auto exception = kj::mv(e);
    wait.exception = kj::none;
    kj::throwFatalException(kj::mv(exception));
  }
}

void settle(kj::Own<Settler> settler) {
  settler->settle();
}

}  // namespace workerd::api::html_rewriter
