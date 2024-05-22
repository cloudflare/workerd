#include "promise-tracker.h"
#include "jsg-test.h"

namespace workerd::jsg::test {

V8System v8System;

namespace {

struct PromiseContext: public jsg::Object, public jsg::ContextGlobal {
  JSG_RESOURCE_TYPE(PromiseContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(PromiseIsolate, PromiseContext);

KJ_TEST("Promise Tracker") {
  Evaluator<PromiseContext, PromiseIsolate> e(v8System);
  e.getIsolate().enableUnsettledPromiseTracker();

  e.getIsolate().runInLockScope([](PromiseIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock,
      lock.template newContext<PromiseContext>().getHandle(lock.v8Isolate),
      [](jsg::Lock& js) {

      auto& isolateBase = IsolateBase::from(js.v8Isolate);
      auto& tracker = KJ_ASSERT_NONNULL(isolateBase.getUnsettledPromiseTracker());

      // Create an unresolved promise.
      auto p1 = js.newPromiseAndResolver<void>();

      // Create a resolved promise.
      auto p2 = js.resolvedPromise();

      // Create a rejected promise.
      auto p3 = js.rejectedPromise<void>(js.str("foo"_kj));

      KJ_ASSERT(tracker.size() == 1);
      KJ_ASSERT(tracker.report().size() > 0);

      // Now let's resolve the outstanding promise...
      p1.resolver.resolve(js);

      KJ_ASSERT(tracker.size() == 0);
      KJ_ASSERT(tracker.report().size() == 0);

      KJ_ASSERT(true);
    });
  });
}

}  // namespace
}  // namespace workerd::jsg::test
