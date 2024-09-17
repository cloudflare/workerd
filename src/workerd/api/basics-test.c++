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

jsg::V8System v8System;

struct BasicsContext: public jsg::Object, public jsg::ContextGlobal {

  bool test(jsg::Lock& js) {

    auto target = jsg::alloc<api::EventTarget>();

    int called = 0;
    bool onceCalled = false;

    // Should be invoked multiple times.
    auto handler = target->newNativeHandler(js, kj::str("foo"),
        [&called](jsg::Lock& js, jsg::Ref<api::Event> event) { called++; }, false);

    // Should only be invoked once.
    auto handlerOnce = target->newNativeHandler(
        js, kj::str("foo"), [&](jsg::Lock& js, jsg::Ref<api::Event> event) {
      KJ_ASSERT(!onceCalled);
      onceCalled = true;
      // Recursively dispatching the event here should not cause this handler to
      // be invoked again.
      target->dispatchEventImpl(js, jsg::alloc<api::Event>(kj::str("foo")));
    }, true);

    KJ_ASSERT(target->dispatchEventImpl(js, jsg::alloc<api::Event>(kj::str("foo"))));
    KJ_ASSERT(target->dispatchEventImpl(js, jsg::alloc<api::Event>(kj::str("foo"))));
    KJ_ASSERT(onceCalled);
    return called == 3;
  }

  JSG_RESOURCE_TYPE(BasicsContext) {
    JSG_METHOD(test);
  }
};
JSG_DECLARE_ISOLATE_TYPE(BasicsIsolate,
    BasicsContext,
    EW_BASICS_ISOLATE_TYPES,
    jsg::TypeWrapperExtension<PromiseWrapper>);

KJ_TEST("EventTarget native listeners work") {
  jsg::test::Evaluator<BasicsContext, BasicsIsolate, CompatibilityFlags::Reader> e(v8System);
  e.expectEval("test()", "boolean", "true");
}

}  // namespace
}  // namespace workerd::api
