// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;
class ContextGlobalObject: public Object, public ContextGlobal {};

struct CallbackContext: public ContextGlobalObject {
  kj::String callCallback(Lock& js, jsg::Function<kj::String(kj::StringPtr, double)> function) {
    return kj::str(function(js, "foo", 123), ", abc");
  }
  double callCallbackReturningBox(Lock& js, jsg::Function<Ref<NumberBox>()> function) {
    return function(js)->value;
  }

  struct Frobber {
    kj::String s;
    double n;
    jsg::Function<kj::String(double)> frob;
    Optional<jsg::Function<kj::String(double)>> optionalFrob;
    kj::Maybe<jsg::Function<kj::String(double)>> maybeFrob;
    JSG_STRUCT(s, n, frob, optionalFrob, maybeFrob);
  };

  kj::String callConstructor(Lock& js, Constructor<Frobber(kj::StringPtr, double)> constructor) {
    auto frobber = constructor(js, "foo", 123);
    return kj::str(frobber.s, frobber.n, frobber.frob(js, 321),
        KJ_ASSERT_NONNULL(frobber.optionalFrob)(js, 654),
        KJ_ASSERT_NONNULL(frobber.maybeFrob)(js, 987));
  }

  JSG_RESOURCE_TYPE(CallbackContext) {
    JSG_NESTED_TYPE(NumberBox);
    JSG_METHOD(callCallback);
    JSG_METHOD(callCallbackReturningBox);
    JSG_METHOD(callConstructor);
  }
};
JSG_DECLARE_ISOLATE_TYPE(CallbackIsolate, CallbackContext, CallbackContext::Frobber, NumberBox);

KJ_TEST("callbacks") {
  Evaluator<CallbackContext, CallbackIsolate> e(v8System);
  e.expectEval("callCallback((str, num) => {\n"
               "  return [typeof str, str, typeof num, num.toString(), 'bar'].join(', ');\n"
               "})",
      "string", "string, foo, number, 123, bar, abc");

  e.expectEval("callCallback((str, num) => {\n"
               "  throw new Error('error message')\n"
               "})",
      "throws", "Error: error message");

  e.expectEval("callCallbackReturningBox(() => {\n"
               "  return new NumberBox(123);\n"
               "})",
      "number", "123");
  e.expectEval("callCallbackReturningBox(() => {\n"
               "  return 'foo';\n"
               "})",
      "throws", "TypeError: Callback returned incorrect type; expected 'NumberBox'");

  e.expectEval("class Frobber {\n"
               "  constructor(s, n) {\n"
               "    this.s = s;\n"
               "    this.n = n;\n"
               "  }\n"
               "  frob(m) {\n"
               "    return this.s + (m + this.n);\n"
               "  }\n"
               "  optionalFrob(m) {\n"
               "    return 'opn' + this.s + (m + this.n);\n"
               "  }\n"
               "  maybeFrob(m) {\n"
               "    return 'mby' + this.s + (m + this.n);\n"
               "  }\n"
               "}\n"
               "callConstructor(Frobber)",
      "string", "foo123foo444opnfoo777mbyfoo1110");
}

// ========================================================================================

struct WrapContext: public ContextGlobalObject {
  auto returnFunction(double value) {
    return [value](Lock&, double value2) { return value + value2; };
  }
  auto returnFunctionWithInfo(double value) {
    return [value](Lock&, const v8::FunctionCallbackInfo<v8::Value>& info, double value2) {
      // Prove that we received `info` by adding in the argument count.
      return value + value2 + info.Length();
    };
  }
  auto returnFunctionMutable(double value) {
    return [value](Lock&, double value2) mutable { return value + value2; };
  }
  auto returnFunctionWithInfoMutable(double value) {
    return [value](Lock&, const v8::FunctionCallbackInfo<v8::Value>& info, double value2) mutable {
      // Prove that we received `info` by adding in the argument count.
      return value + value2 + info.Length();
    };
  }
  auto returnFunctionReturningVoid(double value) {
    return [value](Lock&, NumberBox& box) -> void { box.value = value; };
  }

  JSG_RESOURCE_TYPE(WrapContext) {
    JSG_NESTED_TYPE(NumberBox);
    JSG_METHOD(returnFunction);
    JSG_METHOD(returnFunctionWithInfo);
    JSG_METHOD(returnFunctionMutable);
    JSG_METHOD(returnFunctionWithInfoMutable);
    JSG_METHOD(returnFunctionReturningVoid);
  }
};
JSG_DECLARE_ISOLATE_TYPE(WrapIsolate, WrapContext, NumberBox);

KJ_TEST("wrap functions") {
  Evaluator<WrapContext, WrapIsolate> e(v8System);

  e.expectEval("returnFunction(123)(321)", "number", "444");
  e.expectEval("returnFunctionWithInfo(123)(321, '', undefined)", "number", "447");
  e.expectEval("returnFunctionMutable(123)(321)", "number", "444");
  e.expectEval("returnFunctionWithInfoMutable(123)(321, '', undefined)", "number", "447");

  e.expectEval("var nb = new NumberBox(321);\n"
               "var ret = returnFunctionReturningVoid(123)(nb);\n"
               "ret === undefined ? nb.value : 555",
      "number", "123");
}

// ========================================================================================

struct FunctionContext: public ContextGlobalObject {
  auto test(Lock& js, Function<bool(int)> fn) {
    return fn(js, 1);
  }

  struct Foo {
    Function<bool()> fn;
    JSG_STRUCT(fn);
  };

  auto test2(Lock& js, Foo foo) {
    return foo.fn(js);
  }

  jsg::Function<double(double)> getSquare(Lock& js) {
    jsg::Function<double(double)> result = [](Lock&, double x) { return x * x; };

    // Check we can call it directly.
    KJ_ASSERT(result(js, 11) == 121);

    return result;
  }

  struct VisitDetector {
    bool visited = false;
    void visitForGc(GcVisitor& visitor) {
      visited = true;
    }
  };

  jsg::Function<int(int)> getGcLambda() {
    return JSG_VISITABLE_LAMBDA((v1 = VisitDetector(), v2 = VisitDetector(), v3 = VisitDetector()),
        (v1, v3), (Lock&, int i) {
          KJ_ASSERT(i == 123);

          // Should return 5, since v1 and v3 are visited but v2 is not. Note that a discovery
          // visitation pass happens immediately upon constructing wrappers -- we don't need to wait
          // for an actual GC pass, which is nice for this test.
          return v1.visited + v2.visited * 2 + v3.visited * 4;
        });
  }

  jsg::Function<int(int, int)> getTwoArgs() {
    return JSG_VISITABLE_LAMBDA((), (), (Lock&, int i, int j) {
      // Also test an unparenthesized comma...
      return ++i, i * j;
    });
  }

  kj::String testTryCatch(Lock& js, jsg::Function<int()> thrower) {
    return js.tryCatch([&]() { return kj::str(thrower(js)); }, [&](Value exception) {
      auto handle = exception.getHandle(js);
      return kj::str("caught: ", handle);
    });
  }

  JSG_RESOURCE_TYPE(FunctionContext) {
    JSG_METHOD(test);
    JSG_METHOD(test2);
    JSG_METHOD(testTryCatch);

    JSG_READONLY_PROTOTYPE_PROPERTY(square, getSquare);
    JSG_READONLY_PROTOTYPE_PROPERTY(gcLambda, getGcLambda);
    JSG_READONLY_PROTOTYPE_PROPERTY(twoArgs, getTwoArgs);
  }
};
JSG_DECLARE_ISOLATE_TYPE(FunctionIsolate, FunctionContext, FunctionContext::Foo);

KJ_TEST("jsg::Function<T>") {
  Evaluator<FunctionContext, FunctionIsolate> e(v8System);

  e.expectEval("test((val) => val === 1)", "boolean", "true");

  // This variation checks that a jsg::Function pulled off a struct properly
  // preserves "this" as a reference to the object it was pulled off of.
  e.expectEval("const m = { fn() { return this === m; } }; test2(m);", "boolean", "true");

  e.expectEval("square(5)", "number", "25");

  e.expectEval("gcLambda(123)", "number", "5");

  e.expectEval("twoArgs(2, 5)", "number", "15");

  e.expectEval("testTryCatch(() => { return 123; })", "string", "123");
  e.expectEval("testTryCatch(() => { throw new Error('foo'); })", "string", "caught: Error: foo");
}

}  // namespace
}  // namespace workerd::jsg::test
