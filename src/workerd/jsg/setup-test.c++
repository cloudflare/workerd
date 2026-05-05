// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;

struct EvalContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(EvalContext) {}
};
JSG_DECLARE_ISOLATE_TYPE(EvalIsolate, EvalContext);

KJ_TEST("eval() is blocked") {
  Evaluator<EvalContext, EvalIsolate> e(v8System);
  e.expectEval("eval('123')", "throws",
      "EvalError: Code generation from strings disallowed for this context");
  e.expectEval("new Function('a', 'b', 'return a + b;')(123, 321)", "throws",
      "EvalError: Code generation from strings disallowed for this context");

  // eval() with no args or a non-string arg is allowed even when eval is blocked
  // (V8 returns the value as-is per spec since there is no string to compile).
  e.expectEval("eval()", "undefined", "undefined");
  e.expectEval("eval(undefined)", "undefined", "undefined");

  // new Function() with no arguments is allowed even when eval is blocked (the
  // synthesized source matches the known empty-body, no-parameter pattern).
  e.expectEval("typeof new Function()", "string", "function");

  // new Function() with params and an undefined body is blocked (the body becomes
  // the string "undefined" via ToString, producing a non-empty source).
  e.expectEval("new Function('a', 'b', undefined)", "throws",
      "EvalError: Code generation from strings disallowed for this context");

  // Extending Function with super() and no arguments is also allowed.
  e.expectEval("class Foo extends Function { constructor() { super(); } }; typeof new Foo()",
      "string", "function");

  e.getIsolate().runInLockScope([&](EvalIsolate::Lock& lock) { lock.setAllowEval(true); });

  e.expectEval("eval('123')", "number", "123");
  e.expectEval("new Function('a', 'b', 'return a + b;')(123, 321)", "number", "444");

  e.getIsolate().runInLockScope([&](EvalIsolate::Lock& lock) { lock.setAllowEval(false); });

  e.expectEval("eval('123')", "throws",
      "EvalError: Code generation from strings disallowed for this context");
  e.expectEval("new Function('a', 'b', 'return a + b;')(123, 321)", "throws",
      "EvalError: Code generation from strings disallowed for this context");

  // Note: It would be nice to test as well that WebAssembly is blocked, but that requires
  //   setting up an event loop since the WebAssembly calls are all async. We'll test this
  //   elsewhere.
}

// ========================================================================================

struct ConfigContext: public Object, public ContextGlobal {
  struct Nested: public Object {
    JSG_RESOURCE_TYPE(Nested, int configuration) {
      KJ_EXPECT(configuration == 123, configuration);
    }
  };
  struct OtherNested: public Object {
    JSG_RESOURCE_TYPE(OtherNested) {}
  };

  JSG_RESOURCE_TYPE(ConfigContext) {
    JSG_NESTED_TYPE(Nested);
    JSG_NESTED_TYPE(OtherNested);
  }
};
JSG_DECLARE_ISOLATE_TYPE(
    ConfigIsolate, ConfigContext, ConfigContext::Nested, ConfigContext::OtherNested);

KJ_TEST("configuration values reach nested type declarations") {
  {
    ConfigIsolate isolate(
        v8System, v8::IsolateGroup::GetDefault(), 123, kj::heap<IsolateObserver>());
    isolate.runInLockScope([&](ConfigIsolate::Lock& lock) {
      jsg::Lock& js = lock;
      js.withinHandleScope([&] { lock.newContext<ConfigContext>().getHandle(lock); });
    });
  }
  {
    KJ_EXPECT_LOG(ERROR, "failed: expected configuration == 123");
    ConfigIsolate isolate(
        v8System, v8::IsolateGroup::GetDefault(), 456, kj::heap<IsolateObserver>());
    isolate.runInLockScope([&](ConfigIsolate::Lock& lock) {
      jsg::Lock& js = lock;
      js.withinHandleScope([&] { lock.newContext<ConfigContext>().getHandle(lock); });
    });
  }
}

}  // namespace
}  // namespace workerd::jsg::test
