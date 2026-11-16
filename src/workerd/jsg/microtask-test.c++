// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

#include <v8-microtask-queue.h>

namespace workerd::jsg::test {
namespace {

V8System v8System;
class ContextGlobalObject: public Object, public ContextGlobal {};

struct MicrotaskContext: public ContextGlobalObject {
  // What the microtasks did, since a microtask has no caller to return to. Static rather than
  // members, because a microtask is not tied to the context object it was enqueued through.
  static inline kj::Vector<kj::String> ran;
  static inline uint destroyed = 0;
  static inline kj::Vector<kj::String> reported;

  // Counts the tasks destroyed.
  struct Canary {
    ~Canary() noexcept(false) {
      ++destroyed;
    }
  };

  enum class Failure { NONE, JSG_ERROR, INTERNAL_ERROR, TERMINATION };

  // The data of a callback microtask.
  struct Task {
    kj::Maybe<Function<void()>> function;
    Failure failure = Failure::NONE;
    kj::Own<Canary> canary = kj::heap<Canary>();

    void visitForGc(GcVisitor& visitor) {
      visitor.visit(function);
    }
  };

  void enqueue(Lock& js, Function<void()> function) {
    enqueueTask(js, Task{.function = kj::mv(function)});
  }
  void enqueueThrowing(Lock& js) {
    enqueueTask(js, Task{.failure = Failure::JSG_ERROR});
  }
  void enqueueFailing(Lock& js) {
    enqueueTask(js, Task{.failure = Failure::INTERNAL_ERROR});
  }
  void enqueueTerminating(Lock& js) {
    enqueueTask(js, Task{.failure = Failure::TERMINATION});
  }

  JSG_RESOURCE_TYPE(MicrotaskContext) {
    JSG_METHOD(enqueue);
    JSG_METHOD(enqueueThrowing);
    JSG_METHOD(enqueueFailing);
    JSG_METHOD(enqueueTerminating);
  }

 private:
  static void enqueueTask(Lock& js, Task task) {
    js.v8Context()->GetMicrotaskQueue()->EnqueueMicrotask(
        js.v8Isolate, &runTask, wrapOpaque(js.v8Context(), kj::mv(task)));
  }

  static void runTask(v8::Local<v8::Data> data) {
    auto& js = Lock::current();
    liftKjMicrotask(js.v8Isolate, data, [&]() {
      // Taken out of its wrapper, so that it is destroyed when the call returns.
      auto task = unwrapOpaque<Task>(js.v8Isolate, data.As<v8::Value>());
      // Names the context the task runs in.
      ran.add(js.toString(js.v8Get(js.v8Context()->Global(), "name"_kj)));
      switch (task.failure) {
        case Failure::NONE:
          break;
        case Failure::JSG_ERROR:
          JSG_FAIL_REQUIRE(TypeError, "thrown by the task");
        case Failure::INTERNAL_ERROR:
          KJ_FAIL_ASSERT("the task failed");
        case Failure::TERMINATION:
          js.terminateExecutionNow();
      }
      KJ_IF_SOME(function, task.function) {
        function(js);
      }
    });
  }
};
JSG_DECLARE_ISOLATE_TYPE(MicrotaskIsolate, MicrotaskContext);

KJ_TEST("liftKjMicrotask() runs the body of a callback microtask") {
  Evaluator<MicrotaskContext, MicrotaskIsolate> e(v8System);
  auto& ran = MicrotaskContext::ran;
  auto& destroyed = MicrotaskContext::destroyed;
  auto& reported = MicrotaskContext::reported;

  e.getIsolate().runInLockScope([&](MicrotaskIsolate::Lock& lock) {
    lock.setErrorReporterCallback(
        [](Lock&, kj::String description, const JsValue&, const JsMessage&) {
      MicrotaskContext::reported.add(kj::mv(description));
    });

    auto first = lock.newContext<MicrotaskContext>();
    auto second = lock.newContext<MicrotaskContext>();
    auto evalIn = [&](auto& context, kj::StringPtr code) {
      JSG_WITHIN_CONTEXT_SCOPE(lock, context.getHandle(lock.v8Isolate), [&](Lock& js) {
        auto script = check(v8::Script::Compile(js.v8Context(), v8Str(js.v8Isolate, code)));
        check(script->Run(js.v8Context()));
      });
    };
    evalIn(first, "globalThis.name = 'first';"_kj);
    evalIn(second, "globalThis.name = 'second';"_kj);

    // The queue is drained here with no context entered, as it is after an inspector command. A
    // task runs in the context it was enqueued in all the same, and is destroyed when it returns
    // rather than when the GC collects its wrapper.
    evalIn(first, "enqueue(() => { globalThis.called = true; });"_kj);
    evalIn(second, "enqueue(() => {});"_kj);
    KJ_EXPECT(ran.size() == 0);
    lock.runMicrotasks();
    KJ_EXPECT(kj::strArray(ran, ",") == "first,second");
    KJ_EXPECT(destroyed == 2);
    KJ_EXPECT(reported.size() == 0);
    evalIn(first, "if (!globalThis.called) throw new Error('the function was not called');"_kj);

    // An exception has no caller to reach, so it is reported, and the tasks after it still run.
    evalIn(first, "enqueue(() => { throw new Error('thrown by the function'); });"_kj);
    evalIn(first, "enqueueThrowing();"_kj);
    evalIn(second, "enqueue(() => {});"_kj);
    lock.runMicrotasks();
    KJ_EXPECT(kj::strArray(ran, ",") == "first,second,first,first,second");
    KJ_EXPECT(kj::strArray(reported, ",") ==
        "Error: thrown by the function,TypeError: thrown by the task");
    {
      KJ_EXPECT_LOG(ERROR, "the task failed");
      evalIn(second, "enqueueFailing();"_kj);
      lock.runMicrotasks();
    }
    KJ_EXPECT(reported.size() == 3);
    KJ_EXPECT(reported.back().startsWith("Error: internal error"), reported.back());
    KJ_EXPECT(destroyed == 6);

    // A termination is not reported: it reaches whoever drained the queue, and V8 drops the tasks
    // left in the queue.
    evalIn(first, "enqueueTerminating(); enqueue(() => {});"_kj);
    {
      // NOLINTNEXTLINE(custom-raw-v8-trycatch): test: only observes the termination.
      v8::TryCatch tryCatch(lock.v8Isolate);
      lock.runMicrotasks();
      KJ_EXPECT(tryCatch.HasTerminated());
      tryCatch.Reset();
    }
    lock.v8Isolate->CancelTerminateExecution();
    KJ_EXPECT(ran.size() == 7);
    KJ_EXPECT(ran.back() == "first");
    KJ_EXPECT(reported.size() == 3);
  });
}

}  // namespace
}  // namespace workerd::jsg::test
