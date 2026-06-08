// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Tests for re-entrancy edge cases during draining reads.
// These exercise scenarios where synchronous JS callbacks during
// drainingRead's internal pump loop trigger state changes that
// could cause use-after-free or hangs if not properly guarded.

#include "readable.h"
#include "standard.h"

#include <workerd/jsg/jsg-test.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {
namespace {

jsg::V8System v8System({"--expose-gc"_kj});

using PendingAbort = WritableStreamController::PendingAbort;

class Foo: public jsg::Object {
 public:
  Foo(jsg::Lock& js)
      : pendingAbort(PendingAbort(js, js.newPromiseAndResolver<void>(), js.obj(), false)) {}

  void triggerTest(jsg::Lock& js) {
    // Calling gc once should force a trace. This makes the PendingAbort's
    // members weak and eligible for collection if the next trace doesn't
    // find them.
    js.requestGcForTesting();
    KJ_ASSERT(traced);
    // Moving, then calling GC again should not cause anything to be freed,
    // since the PendingAbort was moved, it's traced members are made strong
    // again. The move makes it so the PendingAbort's members are not found
    // during the next trace; but since they are strong now, they won't be
    // collected.
    KJ_IF_SOME(deq, kj::mv(pendingAbort)) {
      js.requestGcForTesting();
      // Should not UAF
      deq.complete(js);
      KJ_ASSERT(deq.promise.getState(js) == jsg::Promise<void>::State::FULFILLED);
    }
  }

  JSG_RESOURCE_TYPE(Foo) {
    JSG_METHOD(triggerTest);
  }

 private:
  kj::Maybe<PendingAbort> pendingAbort;
  bool traced = false;

  void visitForGc(jsg::GcVisitor& visitor) {
    traced = true;
    visitor.visit(pendingAbort);
  }
};

class ContextGlobalObject: public jsg::Object, public jsg::ContextGlobal {
 public:
  jsg::Ref<Foo> makeAFoo(jsg::Lock& js) {
    return js.alloc<Foo>(js);
  }
  JSG_RESOURCE_TYPE(ContextGlobalObject) {
    JSG_METHOD(makeAFoo);
  }
};

JSG_DECLARE_ISOLATE_TYPE(ContextGlobalIsolate, ContextGlobalObject, Foo);

KJ_TEST("DrainingReader: concurrent draining reads are rejected (value stream)") {
  setPredictableModeForTest();
  jsg::test::Evaluator<ContextGlobalObject, ContextGlobalIsolate, CompatibilityFlags::Reader> e(
      v8System);
  e.expectEval(R"FOO(
    const foo = makeAFoo();
    foo.triggerTest();
  )FOO",
      "undefined", "undefined");
}

}  // namespace
}  // namespace workerd::api
