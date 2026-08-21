// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Filter some stuff from the JSG_RESOURCE_TYPE blocks so that we can actually compile this
// test without pulling in the world.
#define WORKERD_API_BASICS_TEST 1

#include "actor-state.h"
#include "actor.h"
#include "basics.h"
#include "util.h"

#include <workerd/io/promise-wrapper.h>
#include <workerd/jsg/jsg-test.h>
#include <workerd/jsg/jsg.h>

namespace workerd::api {
namespace {

jsg::V8System v8System({"--expose-gc"_kj});

struct BasicsContext: public jsg::Object, public jsg::ContextGlobal {

  bool testAbortAlgorithmsRun(jsg::Lock& js) {
    auto signal = js.alloc<api::AbortSignal>();

    kj::Vector<int> order;
    auto reg1 = signal->addAbortAlgorithm(js, [&order](jsg::Lock&) { order.add(1); });
    auto reg2 = signal->addAbortAlgorithm(js, [&order](jsg::Lock&) { order.add(2); });
    auto reg3 = signal->addAbortAlgorithm(js, [&order](jsg::Lock&) { order.add(3); });

    // Dropping a registration unregisters its algorithm.
    reg2 = kj::Own<void>();

    // A synthetic dispatch of an 'abort' event does not run abort algorithms; only a real
    // abort does.
    signal->dispatchEventImpl(js, js.alloc<api::Event>(kj::str("abort")));
    KJ_ASSERT(order.empty());

    signal->triggerAbort(js, kj::none);
    KJ_ASSERT(order.size() == 2);
    KJ_ASSERT(order[0] == 1);
    KJ_ASSERT(order[1] == 3);
    KJ_ASSERT(signal->getAborted(js));

    // Algorithms are emptied by the abort; a second trigger is a no-op.
    signal->triggerAbort(js, kj::none);
    KJ_ASSERT(order.size() == 2);
    return true;
  }

  bool testAbortAlgorithmHandleAfterSignalGone(jsg::Lock& js) {
    // A registration handle may safely outlive its signal: dropping it afterward is a no-op.
    kj::Own<void> reg;
    {
      auto signal = js.alloc<api::AbortSignal>();
      reg = signal->addAbortAlgorithm(js, [](jsg::Lock&) {});
    }
    js.v8Isolate->RequestGarbageCollectionForTesting(v8::Isolate::kFullGarbageCollection);
    reg = kj::Own<void>();
    return true;
  }

  bool testAbortAlgorithmAddedWhileAborted(jsg::Lock& js) {
    // Callers are expected to check getAborted() first; an algorithm registered against an
    // already-aborted signal never runs (a real abort happens at most once).
    auto signal = js.alloc<api::AbortSignal>();
    signal->triggerAbort(js, kj::none);

    bool called = false;
    auto reg = signal->addAbortAlgorithm(js, [&called](jsg::Lock&) { called = true; });
    signal->triggerAbort(js, kj::none);
    KJ_ASSERT(!called);
    return true;
  }

  JSG_RESOURCE_TYPE(BasicsContext) {
    JSG_METHOD(testAbortAlgorithmsRun);
    JSG_METHOD(testAbortAlgorithmHandleAfterSignalGone);
    JSG_METHOD(testAbortAlgorithmAddedWhileAborted);
  }
};
JSG_DECLARE_ISOLATE_TYPE(BasicsIsolate,
    BasicsContext,
    EW_BASICS_ISOLATE_TYPES,
    jsg::TypeWrapperExtension<PromiseWrapper>);

KJ_TEST("AbortSignal abort algorithms run in order, once, and only for real aborts") {
  jsg::test::Evaluator<BasicsContext, BasicsIsolate, CompatibilityFlags::Reader> e(v8System);
  e.expectEval("testAbortAlgorithmsRun()", "boolean", "true");
}

KJ_TEST("AbortSignal abort algorithm handles are safe after the signal is gone") {
  jsg::test::Evaluator<BasicsContext, BasicsIsolate, CompatibilityFlags::Reader> e(v8System);
  e.expectEval("testAbortAlgorithmHandleAfterSignalGone()", "boolean", "true");
}

KJ_TEST("AbortSignal abort algorithms registered after abort never run") {
  jsg::test::Evaluator<BasicsContext, BasicsIsolate, CompatibilityFlags::Reader> e(v8System);
  e.expectEval("testAbortAlgorithmAddedWhileAborted()", "boolean", "true");
}

}  // namespace
}  // namespace workerd::api
