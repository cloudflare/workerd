// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

// This test covers a shutdown failure involving a wrapped parent that owns an unwrapped child.
// RefHolder is returned to JavaScript and therefore has a JS wrapper and a cppgc shim. Child is
// never exposed to JavaScript, so it has no wrapper of its own, but tracing RefHolder propagates
// the isolate pointer to it through RefHolder::visitForGc().
//
// A major GC can condemn RefHolder's wrapper while leaving its cppgc finalizer pending. Isolate
// teardown must finalize RefHolder while the isolate lock is held so that releasing its Ref<Child>
// can destroy Child immediately. If RefHolder instead remains owned by its condemned shim until
// v8::Isolate::Dispose(), its finalizer runs after the deferred-destruction queue has transitioned
// to DROPPED. Child still remembers the isolate and sees that the isolate lock is not held, so it
// tries to enqueue its destruction and fails the queueState == ACTIVE requirement.
//
// Production stacks reached the same path through container objects such as Response and R2
// GetResult releasing ReadableStream references from CppgcShim::~CppgcShim().
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

struct ShutdownContext: public ContextGlobalObject {
  Ref<RefHolder> makeHolder(Lock& js) {
    return js.alloc<RefHolder>(js.alloc<Child>());
  }

  JSG_RESOURCE_TYPE(ShutdownContext) {
    JSG_NESTED_TYPE(Child);
    JSG_NESTED_TYPE(RefHolder);
    JSG_METHOD(makeHolder);
  }
};
JSG_DECLARE_ISOLATE_TYPE(ShutdownIsolate, ShutdownContext, Child, RefHolder);

KJ_TEST("isolate shutdown finalizes condemned wrappers containing unwrapped children") {
  setPredictableModeForTest();
  childDestructions = 0;
  holderDestructions = 0;

  {
    ShutdownIsolate isolate(v8System, kj::heap<IsolateObserver>());
    isolate.runInLockScope([&](ShutdownIsolate::Lock& lock) {
      JSG_WITHIN_CONTEXT_SCOPE(
          lock, lock.newContext<ShutdownContext>().getHandle(lock), [&](Lock& js) {
        js.withinHandleScope([&] {
          // Drop the only JS reference while the nested handle scope ensures no temporary V8
          // handle keeps RefHolder's wrapper alive.
          auto source = "let holder = makeHolder(); holder = null;"_kj;
          auto script = check(v8::Script::Compile(js.v8Context(), js.str(source)));
          check(script->Run(js.v8Context()));
        });

        // Forced test GCs normally sweep atomically. Leave sweeping pending to reproduce the state
        // created by a natural major GC immediately before isolate shutdown.
        js.requestGcWithDeferredSweepForTesting();
        // Prove that neither cppgc finalization nor child destruction happened during the GC. This
        // keeps the test focused on shutdown rather than the ordinary atomic-sweep path.
        KJ_ASSERT(holderDestructions == 0, holderDestructions);
        KJ_ASSERT(childDestructions == 0, childDestructions);
      });
    });

    // The isolate is destroyed with RefHolder's cppgc finalizer still pending. Isolate teardown
    // must finalize the holder and release its unwrapped child safely.
  }

  KJ_ASSERT(holderDestructions == 1, holderDestructions);
  KJ_ASSERT(childDestructions == 1, childDestructions);
}

}  // namespace
}  // namespace workerd::jsg::test
