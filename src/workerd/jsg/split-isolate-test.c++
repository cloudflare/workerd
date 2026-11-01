// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;
class ContextGlobalObject: public Object, public ContextGlobal {};

// A context's global object inherits the context templates of its JSG superclasses.
struct SplitContextBase: public ContextGlobalObject {
  JSG_RESOURCE_TYPE(SplitContextBase) {
    JSG_NESTED_TYPE(NumberBox);
  }
};
struct SplitContext: public SplitContextBase {
  JSG_RESOURCE_TYPE(SplitContext) {
    JSG_INHERIT(SplitContextBase);
    JSG_NESTED_TYPE(BoxBox);
  }
};
JSG_DECLARE_SPLIT_ISOLATE_TYPE(SplitIsolate, SplitContext, SplitContextBase, NumberBox, BoxBox);

// An application would usually call these from several translation units.
void SplitIsolate_TypeWrapper::registerTemplates() {
  registerTemplateShard<0, 2>();
  registerTemplateShard<1, 2>();
  registerContextTemplate<SplitContext>();
}

KJ_TEST("split isolate types build registered templates") {
  Evaluator<SplitContext, SplitIsolate> e(v8System);
  e.expectEval("new NumberBox(123).value", "number", "123");
  e.expectEval("new BoxBox(new NumberBox(123), 321).inner.value", "number", "444");
  e.expectEval("var n = new NumberBox(123);\n"
               "n.incrementBy(321);\n"
               "n.getValue()",
      "number", "444");
  e.expectEval("new NumberBox(123) instanceof NumberBox", "boolean", "true");
  e.expectEval("new NumberBox(123) instanceof BoxBox", "boolean", "false");
}

JSG_DECLARE_SPLIT_ISOLATE_TYPE(
    UnregisteredIsolate, SplitContext, SplitContextBase, NumberBox, BoxBox);

// Leaves out the second of two shards, which has SplitContext and NumberBox.
void UnregisteredIsolate_TypeWrapper::registerTemplates() {
  registerTemplateShard<0, 2>();
  registerContextTemplate<SplitContext>();
}

KJ_TEST("split isolate types fail to build unregistered templates") {
  UnregisteredIsolate isolate(v8System, kj::heap<IsolateObserver>());
  KJ_EXPECT_THROW_MESSAGE(
      "template was not registered", isolate.runInLockScope([&](UnregisteredIsolate::Lock& lock) {
    // The context's global object inherits a nested NumberBox, whose template it builds.
    lock.newContext<SplitContext>();
  }));
}

}  // namespace
}  // namespace workerd::jsg::test
