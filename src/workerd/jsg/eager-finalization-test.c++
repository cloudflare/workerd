// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

// newCppHeap() configures cppgc with sweeping_support = kAtomic, so a major GC finalizes the
// wrappers it collects before it returns. Isolate teardown depends on this: a shim whose finalizer
// is still pending when the isolate is destroyed still owns its Wrappable, which is then finalized
// during v8::Isolate::Dispose() without the isolate lock and after IsolateBase's
// deferred-destruction queue has stopped accepting work. (The condemned-wrapper handling in
// Wrappable covers WeakRef promotion in that window, not shutdown.)
//
// This test pins that timing down. It must not use a plain forced GC, which sweeps atomically
// regardless of how the embedder configured cppgc and so would pass either way; see
// Lock::requestGcWithDefaultSweepForTesting().
//
// RefHolder is returned to JavaScript and therefore has a wrapper and a shim. Child is never
// exposed to JavaScript and has no wrapper of its own, but tracing RefHolder propagates the
// isolate pointer to it through RefHolder::visitForGc(). That combination is what makes late
// finalization fatal rather than merely late: Child remembers the isolate, so when RefHolder
// releases its Ref<Child> it tries to defer Child's destruction, and finds the queue closed.
// Production stacks reached this through container objects such as Response and R2 GetResult
// releasing ReadableStream references from CppgcShim::~CppgcShim().
V8System v8System({"--expose-gc"_kj});

class ContextGlobalObject: public Object, public ContextGlobal {};

uint childDestructions = 0;
uint holderDestructions = 0;

class Child final: public Object {
 public:
  ~Child() noexcept(false) {
    ++childDestructions;
  }

  JSG_RESOURCE_TYPE(Child) {}
};

class RefHolder final: public Object {
 public:
  explicit RefHolder(Ref<Child> child): child(kj::mv(child)) {}

  ~RefHolder() noexcept(false) {
    ++holderDestructions;
  }

  void visitForGc(GcVisitor& visitor) {
    visitor.visit(child);
  }

  JSG_RESOURCE_TYPE(RefHolder) {}

 private:
  Ref<Child> child;
};

struct EagerFinalizationContext: public ContextGlobalObject {
  Ref<RefHolder> makeHolder(Lock& js) {
    return js.alloc<RefHolder>(js.alloc<Child>());
  }

  JSG_RESOURCE_TYPE(EagerFinalizationContext) {
    JSG_NESTED_TYPE(Child);
    JSG_NESTED_TYPE(RefHolder);
    JSG_METHOD(makeHolder);
  }
};
JSG_DECLARE_ISOLATE_TYPE(EagerFinalizationIsolate, EagerFinalizationContext, Child, RefHolder);

KJ_TEST("major GC finalizes the wrappers it collects before returning") {
  setPredictableModeForTest();
  childDestructions = 0;
  holderDestructions = 0;

  {
    EagerFinalizationIsolate isolate(v8System, kj::heap<IsolateObserver>());
    isolate.runInLockScope([&](EagerFinalizationIsolate::Lock& lock) {
      JSG_WITHIN_CONTEXT_SCOPE(
          lock, lock.newContext<EagerFinalizationContext>().getHandle(lock), [&](Lock& js) {
        js.withinHandleScope([&] {
          // The nested handle scope ensures no temporary V8 handle outlives the script and keeps
          // RefHolder's wrapper alive.
          auto source = "let holder = makeHolder(); holder = null;"_kj;
          auto script = check(v8::Script::Compile(js.v8Context(), js.str(source)));
          check(script->Run(js.v8Context()));
        });

        js.requestGcWithDefaultSweepForTesting();

        // Both objects are gone by the time the collection returns, while the isolate lock is
        // still held. If cppgc ever starts sweeping incrementally or concurrently here, these
        // assertions are what catches it.
        KJ_ASSERT(holderDestructions == 1, holderDestructions);
        KJ_ASSERT(childDestructions == 1, childDestructions);
      });
    });
  }

  // Shutdown neither re-finalizes the pair nor trips the deferred-destruction queue.
  KJ_ASSERT(holderDestructions == 1, holderDestructions);
  KJ_ASSERT(childDestructions == 1, childDestructions);
}

}  // namespace
}  // namespace workerd::jsg::test
