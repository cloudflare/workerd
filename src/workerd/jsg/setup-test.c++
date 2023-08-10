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
  e.expectEval("eval('123')",
      "throws", "EvalError: Code generation from strings disallowed for this context");
  e.expectEval("new Function('a', 'b', 'return a + b;')(123, 321)",
      "throws", "EvalError: Code generation from strings disallowed for this context");

  {
    V8StackScope stackScope;
    EvalIsolate::Lock(e.getIsolate(), stackScope).setAllowEval(true);
  }

  e.expectEval("eval('123')", "number", "123");
  e.expectEval("new Function('a', 'b', 'return a + b;')(123, 321)", "number", "444");

  {
    V8StackScope stackScope;
    EvalIsolate::Lock(e.getIsolate(), stackScope).setAllowEval(false);
  }

  e.expectEval("eval('123')",
      "throws", "EvalError: Code generation from strings disallowed for this context");
  e.expectEval("new Function('a', 'b', 'return a + b;')(123, 321)",
      "throws", "EvalError: Code generation from strings disallowed for this context");

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
JSG_DECLARE_ISOLATE_TYPE(ConfigIsolate, ConfigContext, ConfigContext::Nested,
    ConfigContext::OtherNested);

KJ_TEST("configuration values reach nested type declarations") {
  {
    ConfigIsolate isolate(v8System, 123, kj::heap<IsolateObserver>());
    V8StackScope stackScope;
    ConfigIsolate::Lock lock(isolate, stackScope);
    v8::HandleScope handleScope(lock.v8Isolate);
    lock.newContext<ConfigContext>().getHandle(lock);
  }
  {
    KJ_EXPECT_LOG(ERROR, "failed: expected configuration == 123");

    ConfigIsolate isolate(v8System, 456, kj::heap<IsolateObserver>());
    V8StackScope stackScope;
    ConfigIsolate::Lock lock(isolate, stackScope);
    v8::HandleScope handleScope(lock.v8Isolate);
    lock.newContext<ConfigContext>().getHandle(lock);
  }
}

}  // namespace
}  // namespace workerd::jsg::test
