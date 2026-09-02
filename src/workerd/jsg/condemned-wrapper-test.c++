// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests for the window in which a Wrappable's wrapper has been collected by a major GC but the
// ~CppgcShim that releases the Wrappable has not run yet. In that window the Wrappable is
// condemned: it is still addressable, and its WeakRef anchor still reports alive, but promoting a
// WeakRef to a strong Ref would resurrect a doomed object. See Wrappable::isCondemned().
//
// A forced GC normally sweeps atomically and runs ~CppgcShim before returning, so the window never
// opens. Lock::requestGcWithDeferredSweepForTesting() leaves the sweep pending, which is what a
// natural major GC does, and makes the window observable without relying on allocation pressure to
// be handed a GC at the right moment.

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

// A pending sweep stays pending for the rest of the test without any special GC flags, because
// cppgc only ever runs finalizers on the mutator thread: a concurrent sweep task merely collects
// unfinalized objects (DeferredFinalizationBuilder) and SweepFinalizer drains that list on the
// mutator thread. Nothing between requestGcWithDeferredSweepForTesting() and the assertions
// allocates or pumps the foreground task runner, so ~CppgcShim cannot run until
// finishDeferredSweepForTesting() asks for it.
V8System v8System({"--expose-gc"_kj});

class ContextGlobalObject: public Object, public ContextGlobal {};

// Set by DeferredSweepContext::makeBox(). A namespace-scope variable rather than a member because
// Evaluator::run() does not hand the caller a reference to the context object.
kj::Maybe<WeakRef<NumberBox>> weakBox;

struct DeferredSweepContext: public ContextGlobalObject {
  // Allocates a NumberBox and remembers a weak ref to it. Returning the Ref is what gives the
  // object a JS wrapper, which is the thing the GC later collects.
  Ref<NumberBox> makeBox(Lock& js) {
    auto box = js.alloc<NumberBox>(42);
    weakBox = box.getWeakRef(js);
    return box;
  }

  JSG_RESOURCE_TYPE(DeferredSweepContext) {
    JSG_NESTED_TYPE(NumberBox);
    JSG_METHOD(makeBox);
  }
};
JSG_DECLARE_ISOLATE_TYPE(DeferredSweepIsolate, DeferredSweepContext, NumberBox);

// Creates a wrapped NumberBox reachable only from JS, then drops the JS reference, leaving the
// wrapper collectable. The handle scope keeps the script's own handles from rooting it.
void makeCollectableBox(Lock& js) {
  js.withinHandleScope([&] {
    auto source = "let b = makeBox(); b = null;"_kj;
    auto script = check(v8::Script::Compile(js.v8Context(), js.str(source)));
    check(script->Run(js.v8Context()));
  });
  KJ_ASSERT(weakBox != kj::none);
}

KJ_TEST("deferred sweep: WeakRef refuses to promote a condemned target") {
  setPredictableModeForTest();
  Evaluator<DeferredSweepContext, DeferredSweepIsolate> e(v8System);
  e.run([](Lock& js) {
    // Reset even if an assertion fails, so a WeakRef cannot outlive the isolate or
    // leak into the next test.
    KJ_DEFER(weakBox = kj::none);
    auto& tracer = HeapTracer::getTracer(js.v8Isolate);
    auto countBefore = tracer.getCondemnedWrapperCount();

    makeCollectableBox(js);
    auto& weak = KJ_ASSERT_NONNULL(weakBox);

    js.requestGcWithDeferredSweepForTesting();

    // The Wrappable is still addressable: ~CppgcShim, which is what drops the shim's owning
    // reference and runs ~Wrappable, has not been given a chance to run.
    auto& box = KJ_ASSERT_NONNULL(weak.tryGet());
    KJ_ASSERT(box.value == 42);

    // Promotion must fail rather than resurrect it, and must record that it did so. The counter
    // is what establishes that the refusal came from the condemned check: tryAddRef() only bumps
    // it on the isCondemned() branch, so this is equivalent to asserting isCondemned() directly,
    // which the test cannot do because Object inherits Wrappable privately.
    KJ_ASSERT(weak.tryAddRef(js) == kj::none);
    KJ_ASSERT(tracer.getCondemnedWrapperCount() == countBefore + 1);

    // Having refused once, the anchor is invalidated permanently.
    KJ_ASSERT(!weak.isAlive());

    js.finishDeferredSweepForTesting();
  });
}

KJ_TEST("deferred sweep: finishing the sweep releases the Wrappable") {
  setPredictableModeForTest();
  Evaluator<DeferredSweepContext, DeferredSweepIsolate> e(v8System);
  e.run([](Lock& js) {
    // Reset even if an assertion fails, so a WeakRef cannot outlive the isolate or
    // leak into the next test.
    KJ_DEFER(weakBox = kj::none);
    makeCollectableBox(js);
    auto& weak = KJ_ASSERT_NONNULL(weakBox);

    js.requestGcWithDeferredSweepForTesting();
    KJ_ASSERT(weak.isAlive());

    // Running the deferred ~CppgcShim drops the last reference to the Wrappable, which invalidates
    // the anchor through ~Wrappable() rather than through the condemned check.
    js.finishDeferredSweepForTesting();
    KJ_ASSERT(!weak.isAlive());
  });
}

KJ_TEST("deferred sweep: isolate shutdown handles a condemned Wrappable") {
  setPredictableModeForTest();
  KJ_DEFER(weakBox = kj::none);

  {
    DeferredSweepIsolate isolate(v8System, kj::heap<IsolateObserver>());
    isolate.runInLockScope([&](DeferredSweepIsolate::Lock& lock) {
      JSG_WITHIN_CONTEXT_SCOPE(
          lock, lock.newContext<DeferredSweepContext>().getHandle(lock), [&](Lock& js) {
        makeCollectableBox(js);
        js.requestGcWithDeferredSweepForTesting();
        KJ_ASSERT(KJ_ASSERT_NONNULL(weakBox).isAlive());
      });
    });
    // The isolate is destroyed with ~CppgcShim still pending.
  }

  KJ_ASSERT(!KJ_ASSERT_NONNULL(weakBox).isAlive());
}

// Contrast with the above: this is the behaviour the deferred-sweep hook exists to change. If this
// ever starts reporting a condemned wrapper, a forced GC has stopped sweeping atomically and the
// hook is no longer buying anything.
KJ_TEST("forced GC sweeps atomically, so the condemned window never opens") {
  setPredictableModeForTest();
  Evaluator<DeferredSweepContext, DeferredSweepIsolate> e(v8System);
  e.run([](Lock& js) {
    // Reset even if an assertion fails, so a WeakRef cannot outlive the isolate or
    // leak into the next test.
    KJ_DEFER(weakBox = kj::none);
    auto& tracer = HeapTracer::getTracer(js.v8Isolate);
    auto countBefore = tracer.getCondemnedWrapperCount();

    makeCollectableBox(js);
    auto& weak = KJ_ASSERT_NONNULL(weakBox);

    js.requestGcForTesting();

    // ~CppgcShim ran inside the collection, so the anchor was invalidated by ~Wrappable() and
    // there was never a condemned Wrappable for tryAddRef() to catch.
    KJ_ASSERT(!weak.isAlive());
    KJ_ASSERT(weak.tryAddRef(js) == kj::none);
    KJ_ASSERT(tracer.getCondemnedWrapperCount() == countBefore);
  });
}

}  // namespace
}  // namespace workerd::jsg::test
