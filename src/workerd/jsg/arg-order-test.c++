// Copyright (c) 2017-2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Integration tests verifying that JSG-generated V8 callbacks unwrap their
// arguments in deterministic left-to-right order, regardless of the host
// compiler's chosen argument-evaluation order.
//
// Each test passes 3+ arguments to a JSG-exposed entry point, where each
// argument is a JS object with a `toString` that records its label into a
// shared array.  The C++ entry point coerces each argument to `kj::String`
// (triggering `.toString()`), which appends to the array.  We then assert
// the array's contents match the declaration order.
//
// Before this fix, on toolchains that evaluated function-call arguments
// right-to-left (e.g. MSVC), the order would reverse.  After the fix, all
// toolchains produce `a,b,c` regardless of evaluation choice.

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;
class ContextGlobalObject: public Object, public ContextGlobal {};

// Shared JS snippet that creates a function `record(label)` returning an
// object whose `toString` pushes `label` onto a global `order` array, then
// returns `label` so the C++ side receives a normal string.  Each test
// concatenates this prelude with its actual invocation.
constexpr kj::StringPtr kRecordPrelude =
    "let order = [];"
    "let record = (label) => ({ toString() { order.push(label); return label; } });"_kj;

// =====================================================================================
// MethodCallback (plain) — instance method that takes 3 stringifiable args.

struct PlainMethodContext: public ContextGlobalObject {
  kj::String orderTest(kj::String a, kj::String b, kj::String c) {
    return kj::str(a, ",", b, ",", c);
  }
  JSG_RESOURCE_TYPE(PlainMethodContext) {
    JSG_METHOD(orderTest);
  }
};
JSG_DECLARE_ISOLATE_TYPE(PlainMethodIsolate, PlainMethodContext);

KJ_TEST("Argument evaluation order: instance method (plain)") {
  Evaluator<PlainMethodContext, PlainMethodIsolate> e(v8System);
  e.expectEval(kj::str(kRecordPrelude,
                   "orderTest(record('a'), record('b'), record('c'));"
                   "order.join(',')"),
      "string", "a,b,c");
}

// =====================================================================================
// MethodCallback (Lock& first) — same shape, with Lock& as first parameter.

struct LockFirstMethodContext: public ContextGlobalObject {
  kj::String orderTest(Lock& js, kj::String a, kj::String b, kj::String c) {
    return kj::str(a, ",", b, ",", c);
  }
  JSG_RESOURCE_TYPE(LockFirstMethodContext) {
    JSG_METHOD(orderTest);
  }
};
JSG_DECLARE_ISOLATE_TYPE(LockFirstMethodIsolate, LockFirstMethodContext);

KJ_TEST("Argument evaluation order: instance method (Lock& first)") {
  Evaluator<LockFirstMethodContext, LockFirstMethodIsolate> e(v8System);
  e.expectEval(kj::str(kRecordPrelude,
                   "orderTest(record('a'), record('b'), record('c'));"
                   "order.join(',')"),
      "string", "a,b,c");
}

// =====================================================================================
// MethodCallback (FunctionCallbackInfo& first) — info-receiving method.

struct InfoFirstMethodContext: public ContextGlobalObject {
  kj::String orderTest(
      const v8::FunctionCallbackInfo<v8::Value>& info, kj::String a, kj::String b, kj::String c) {
    return kj::str(a, ",", b, ",", c);
  }
  JSG_RESOURCE_TYPE(InfoFirstMethodContext) {
    JSG_METHOD(orderTest);
  }
};
JSG_DECLARE_ISOLATE_TYPE(InfoFirstMethodIsolate, InfoFirstMethodContext);

KJ_TEST("Argument evaluation order: instance method (FunctionCallbackInfo& first)") {
  Evaluator<InfoFirstMethodContext, InfoFirstMethodIsolate> e(v8System);
  e.expectEval(kj::str(kRecordPrelude,
                   "orderTest(record('a'), record('b'), record('c'));"
                   "order.join(',')"),
      "string", "a,b,c");
}

// =====================================================================================
// ConstructorCallback (plain / Lock& first / FunctionCallbackInfo& first).
//
// A constructor's job is to produce a Ref<Self>; the body just records and
// discards its args.  The JS test then re-uses `order` to assert that the
// constructor arguments were evaluated left-to-right before the C++ body
// ran.

struct PlainConstructible: public Object {
  static Ref<PlainConstructible> constructor(kj::String a, kj::String b, kj::String c) {
    return jsg::alloc<PlainConstructible>();
  }
  JSG_RESOURCE_TYPE(PlainConstructible) {}
};

struct PlainCtorContext: public ContextGlobalObject {
  JSG_RESOURCE_TYPE(PlainCtorContext) {
    JSG_NESTED_TYPE(PlainConstructible);
  }
};
JSG_DECLARE_ISOLATE_TYPE(PlainCtorIsolate, PlainCtorContext, PlainConstructible);

KJ_TEST("Argument evaluation order: constructor (plain)") {
  Evaluator<PlainCtorContext, PlainCtorIsolate> e(v8System);
  e.expectEval(kj::str(kRecordPrelude,
                   "new PlainConstructible(record('a'), record('b'), record('c'));"
                   "order.join(',')"),
      "string", "a,b,c");
}

struct LockFirstConstructible: public Object {
  static Ref<LockFirstConstructible> constructor(
      Lock& js, kj::String a, kj::String b, kj::String c) {
    return jsg::alloc<LockFirstConstructible>();
  }
  JSG_RESOURCE_TYPE(LockFirstConstructible) {}
};

struct LockFirstCtorContext: public ContextGlobalObject {
  JSG_RESOURCE_TYPE(LockFirstCtorContext) {
    JSG_NESTED_TYPE(LockFirstConstructible);
  }
};
JSG_DECLARE_ISOLATE_TYPE(LockFirstCtorIsolate, LockFirstCtorContext, LockFirstConstructible);

KJ_TEST("Argument evaluation order: constructor (Lock& first)") {
  Evaluator<LockFirstCtorContext, LockFirstCtorIsolate> e(v8System);
  e.expectEval(kj::str(kRecordPrelude,
                   "new LockFirstConstructible(record('a'), record('b'), record('c'));"
                   "order.join(',')"),
      "string", "a,b,c");
}

struct InfoFirstConstructible: public Object {
  static Ref<InfoFirstConstructible> constructor(
      const v8::FunctionCallbackInfo<v8::Value>& info, kj::String a, kj::String b, kj::String c) {
    return jsg::alloc<InfoFirstConstructible>();
  }
  JSG_RESOURCE_TYPE(InfoFirstConstructible) {}
};

struct InfoFirstCtorContext: public ContextGlobalObject {
  JSG_RESOURCE_TYPE(InfoFirstCtorContext) {
    JSG_NESTED_TYPE(InfoFirstConstructible);
  }
};
JSG_DECLARE_ISOLATE_TYPE(InfoFirstCtorIsolate, InfoFirstCtorContext, InfoFirstConstructible);

KJ_TEST("Argument evaluation order: constructor (FunctionCallbackInfo& first)") {
  Evaluator<InfoFirstCtorContext, InfoFirstCtorIsolate> e(v8System);
  e.expectEval(kj::str(kRecordPrelude,
                   "new InfoFirstConstructible(record('a'), record('b'), record('c'));"
                   "order.join(',')"),
      "string", "a,b,c");
}

// =====================================================================================
// StaticMethodCallback (plain / Lock& first / FunctionCallbackInfo& first).

struct PlainStaticHost: public Object {
  static kj::String orderTest(kj::String a, kj::String b, kj::String c) {
    return kj::str(a, ",", b, ",", c);
  }
  JSG_RESOURCE_TYPE(PlainStaticHost) {
    JSG_STATIC_METHOD(orderTest);
  }
};

struct PlainStaticContext: public ContextGlobalObject {
  JSG_RESOURCE_TYPE(PlainStaticContext) {
    JSG_NESTED_TYPE(PlainStaticHost);
  }
};
JSG_DECLARE_ISOLATE_TYPE(PlainStaticIsolate, PlainStaticContext, PlainStaticHost);

KJ_TEST("Argument evaluation order: static method (plain)") {
  Evaluator<PlainStaticContext, PlainStaticIsolate> e(v8System);
  e.expectEval(kj::str(kRecordPrelude,
                   "PlainStaticHost.orderTest(record('a'), record('b'), record('c'));"
                   "order.join(',')"),
      "string", "a,b,c");
}

struct LockFirstStaticHost: public Object {
  static kj::String orderTest(Lock& js, kj::String a, kj::String b, kj::String c) {
    return kj::str(a, ",", b, ",", c);
  }
  JSG_RESOURCE_TYPE(LockFirstStaticHost) {
    JSG_STATIC_METHOD(orderTest);
  }
};

struct LockFirstStaticContext: public ContextGlobalObject {
  JSG_RESOURCE_TYPE(LockFirstStaticContext) {
    JSG_NESTED_TYPE(LockFirstStaticHost);
  }
};
JSG_DECLARE_ISOLATE_TYPE(LockFirstStaticIsolate, LockFirstStaticContext, LockFirstStaticHost);

KJ_TEST("Argument evaluation order: static method (Lock& first)") {
  Evaluator<LockFirstStaticContext, LockFirstStaticIsolate> e(v8System);
  e.expectEval(kj::str(kRecordPrelude,
                   "LockFirstStaticHost.orderTest(record('a'), record('b'), record('c'));"
                   "order.join(',')"),
      "string", "a,b,c");
}

struct InfoFirstStaticHost: public Object {
  static kj::String orderTest(
      const v8::FunctionCallbackInfo<v8::Value>& info, kj::String a, kj::String b, kj::String c) {
    return kj::str(a, ",", b, ",", c);
  }
  JSG_RESOURCE_TYPE(InfoFirstStaticHost) {
    JSG_STATIC_METHOD(orderTest);
  }
};

struct InfoFirstStaticContext: public ContextGlobalObject {
  JSG_RESOURCE_TYPE(InfoFirstStaticContext) {
    JSG_NESTED_TYPE(InfoFirstStaticHost);
  }
};
JSG_DECLARE_ISOLATE_TYPE(InfoFirstStaticIsolate, InfoFirstStaticContext, InfoFirstStaticHost);

KJ_TEST("Argument evaluation order: static method (FunctionCallbackInfo& first)") {
  Evaluator<InfoFirstStaticContext, InfoFirstStaticIsolate> e(v8System);
  e.expectEval(kj::str(kRecordPrelude,
                   "InfoFirstStaticHost.orderTest(record('a'), record('b'), record('c'));"
                   "order.join(',')"),
      "string", "a,b,c");
}

// =====================================================================================
// FunctorCallback — jsg::Function<R(A,B,C)> invoked from JS.
//
// JSG exposes Function<...> via methods that return one — the JS side then
// calls the returned function.  This exercises FunctorCallback's argument
// unwrap path.

struct FunctorContext: public ContextGlobalObject {
  Function<kj::String(kj::String, kj::String, kj::String)> makeFn() {
    return [](Lock& js, kj::String a, kj::String b, kj::String c) -> kj::String {
      return kj::str(a, ",", b, ",", c);
    };
  }
  JSG_RESOURCE_TYPE(FunctorContext) {
    JSG_METHOD(makeFn);
  }
};
JSG_DECLARE_ISOLATE_TYPE(FunctorIsolate, FunctorContext);

KJ_TEST("Argument evaluation order: jsg::Function (plain)") {
  Evaluator<FunctorContext, FunctorIsolate> e(v8System);
  e.expectEval(kj::str(kRecordPrelude,
                   "makeFn()(record('a'), record('b'), record('c'));"
                   "order.join(',')"),
      "string", "a,b,c");
}

// Specialisation for callable signatures that take
// `const v8::FunctionCallbackInfo<v8::Value>&` as the parameter after `Lock&`.
// In production this shape is used by `jsg::Function<void(const
// v8::FunctionCallbackInfo<v8::Value>&)>` (see e.g. `jsg::Lock`'s test-only
// `simpleFunction` callback in setup.h / jsg.h).

struct InfoFirstFunctorContext: public ContextGlobalObject {
  Function<kj::String(
      const v8::FunctionCallbackInfo<v8::Value>&, kj::String, kj::String, kj::String)>
  makeFn() {
    return [](Lock& js, const v8::FunctionCallbackInfo<v8::Value>& info, kj::String a, kj::String b,
               kj::String c) -> kj::String { return kj::str(a, ",", b, ",", c); };
  }
  JSG_RESOURCE_TYPE(InfoFirstFunctorContext) {
    JSG_METHOD(makeFn);
  }
};
JSG_DECLARE_ISOLATE_TYPE(InfoFirstFunctorIsolate, InfoFirstFunctorContext);

KJ_TEST("Argument evaluation order: jsg::Function (FunctionCallbackInfo& first)") {
  Evaluator<InfoFirstFunctorContext, InfoFirstFunctorIsolate> e(v8System);
  e.expectEval(kj::str(kRecordPrelude,
                   "makeFn()(record('a'), record('b'), record('c'));"
                   "order.join(',')"),
      "string", "a,b,c");
}

}  // namespace
}  // namespace workerd::jsg::test
