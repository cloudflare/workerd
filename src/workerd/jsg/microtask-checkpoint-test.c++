// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;

struct CheckpointContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(CheckpointContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(CheckpointIsolate, CheckpointContext);

// Evaluates `code`, which must evaluate to a function.
V8Ref<v8::Function> compileFunction(Lock& js, kj::StringPtr code) {
  auto script = check(v8::Script::Compile(js.v8Context(), v8Str(js.v8Isolate, code)));
  return js.v8Ref(check(script->Run(js.v8Context())).As<v8::Function>());
}

void call(Lock& js, V8Ref<v8::Function>& function) {
  check(function.getHandle(js)->Call(js.v8Context(), js.v8Undefined(), 0, nullptr));
}

void enqueue(Lock& js, V8Ref<v8::Function>& function) {
  js.v8Context()->GetMicrotaskQueue()->EnqueueMicrotask(js.v8Isolate, function.getHandle(js));
}

// Reports the promises of the isolate rejected with no handler to an UnhandledRejectionHandler,
// as a Worker does.
class RejectionReporter {
 public:
  RejectionReporter(Lock& js, UnhandledRejectionHandler& handler): js(js) {
    current = handler;
    reported = 0;
    handler.setUseMicrotasksCompletedCallback(true);
    js.v8Isolate->SetPromiseRejectCallback(&report);
  }
  ~RejectionReporter() noexcept(false) {
    js.v8Isolate->SetPromiseRejectCallback(nullptr);
    current = kj::none;
  }
  KJ_DISALLOW_COPY_AND_MOVE(RejectionReporter);

  // How many rejections were reported.
  static inline uint reported = 0;

 private:
  Lock& js;
  static inline kj::Maybe<UnhandledRejectionHandler&> current;

  static void report(v8::PromiseRejectMessage message) {
    auto& js = Lock::from(v8::Isolate::GetCurrent());
    ++reported;
    try {
      KJ_ASSERT_NONNULL(current).report(
          js, message.GetEvent(), js.v8Ref(message.GetPromise()), js.v8Ref(message.GetValue()));
    } catch (JsExceptionThrown&) {
      // V8 expects us to just return.
    }
  }
};

KJ_TEST("an unhandled rejection handler that terminates execution ends a chain of rejections") {
  // Each round of the chain takes an extra microtask checkpoint: the handler queues a microtask
  // that rejects the next promise. At the last round it then terminates execution, as a listener
  // that exceeds the CPU limit or aborts its Durable Object does.
  static constexpr uint ROUNDS = 10;
  Evaluator<CheckpointContext, CheckpointIsolate> e(v8System);
  e.run([](Lock& js) {
    auto reject = compileFunction(js, "(function() { Promise.reject(new Error('rejected')); })");
    uint rounds = 0;
    UnhandledRejectionHandler handler(
        [&](Lock& js, v8::PromiseRejectEvent, V8Ref<v8::Promise>, Value) {
      // A chain that outlives the termination stops, for the test to fail rather than hang.
      if (++rounds > ROUNDS) {
        return;
      }
      enqueue(js, reject);
      if (rounds == ROUNDS) {
        js.terminateExecutionNow();
      }
    });
    RejectionReporter reporter(js, handler);

    // NOLINTNEXTLINE(custom-raw-v8-trycatch): the test only checks for the termination.
    v8::TryCatch tryCatch(js.v8Isolate);
    call(js, reject);
    js.runMicrotasks();
    KJ_EXPECT(tryCatch.HasTerminated());
    KJ_EXPECT(rounds == ROUNDS, rounds);
    // The microtask that the last round queued has not run. Nothing was logged either: an
    // unexpected error log fails the test.
    KJ_EXPECT(reporter.reported == ROUNDS, reporter.reported);
    js.v8Isolate->CancelTerminateExecution();

    // That microtask is still queued, as V8 drops the queue only when a microtask terminates
    // execution. The caller cancels it.
    js.terminateNextExecution();
    js.runMicrotasks();
    KJ_EXPECT(tryCatch.HasTerminated());
    js.v8Isolate->CancelTerminateExecution();
    js.runMicrotasks();
    KJ_EXPECT(reporter.reported == ROUNDS, reporter.reported);
  });
}

KJ_TEST("a microtask that terminates execution drops the pending unhandled rejections") {
  Evaluator<CheckpointContext, CheckpointIsolate> e(v8System);
  e.run([](Lock& js) {
    auto reject = compileFunction(js, "(function() { Promise.reject(new Error('rejected')); })");
    auto spin = compileFunction(js, "(function() { for (;;) {} })");
    uint handled = 0;
    UnhandledRejectionHandler handler(
        [&](Lock&, v8::PromiseRejectEvent, V8Ref<v8::Promise>, Value) { ++handled; });
    RejectionReporter reporter(js, handler);

    // NOLINTNEXTLINE(custom-raw-v8-trycatch): the test only checks for the termination.
    v8::TryCatch tryCatch(js.v8Isolate);
    call(js, reject);
    enqueue(js, spin);
    js.terminateNextExecution();
    js.runMicrotasks();
    KJ_EXPECT(tryCatch.HasTerminated());
    KJ_EXPECT(handled == 0, handled);
    js.v8Isolate->CancelTerminateExecution();

    // Only the new rejection is handled.
    call(js, reject);
    js.runMicrotasks();
    KJ_EXPECT(handled == 1, handled);
  });
}

}  // namespace
}  // namespace workerd::jsg::test
