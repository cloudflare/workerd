// Copyright (c) 2026 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;
class ContextGlobalObject: public Object, public ContextGlobal {};

struct BoundFunctionContext: public ContextGlobalObject {
  // A function that returns the arguments its callback gets, with `first` and `second` bound.
  JsValue bindTwo(Lock& js, JsValue first, JsValue second) {
    v8::Local<v8::Value> boundArgs[] = {first, second};
    return JsValue(newBoundFunction(js, &collectArguments, boundArgs));
  }

  JSG_RESOURCE_TYPE(BoundFunctionContext) {
    JSG_METHOD(bindTwo);
  }

 private:
  static void collectArguments(const v8::FunctionCallbackInfo<v8::Value>& info) {
    auto isolate = info.GetIsolate();
    v8::LocalVector<v8::Value> arguments(isolate, info.Length());
    for (int i = 0; i < info.Length(); i++) {
      arguments[i] = info[i];
    }
    info.GetReturnValue().Set(v8::Array::New(isolate, arguments.data(), arguments.size()));
  }
};
JSG_DECLARE_ISOLATE_TYPE(BoundFunctionIsolate, BoundFunctionContext);

KJ_TEST("newBoundFunction() puts the bound arguments in front of the caller's") {
  Evaluator<BoundFunctionContext, BoundFunctionIsolate> e(v8System);

  e.expectEval("bindTwo('a', 'b')('c', 'd').join()", "string", "a,b,c,d");
  e.expectEval("bindTwo(1, 2)().join()", "string", "1,2");

  // It looks like a function made directly over a native callback.
  e.expectEval("const f = bindTwo(1, 2); [typeof f, JSON.stringify(f.name), f.length].join()",
      "string", "function,\"\",0");

  // The `bind` applied is the one the context was created with, whatever JavaScript has done to
  // it, or to `Function`, since.
  e.expectEval("let calls = 0;\n"
               "const bind = Function.prototype.bind;\n"
               "Function.prototype.bind = function (...args) {\n"
               "  calls++;\n"
               "  return Reflect.apply(bind, this, args);\n"
               "};\n"
               "const replaced = bindTwo('a', 'b');\n"
               "delete Function.prototype.bind;\n"
               "const deleted = bindTwo('c', 'd');\n"
               "globalThis.Function = undefined;\n"
               "const noFunction = bindTwo('e', 'f');\n"
               "[calls, replaced(1).join(), deleted(2).join(), noFunction(3).join()].join(' ')",
      "string", "0 a,b,1 c,d,2 e,f,3");
}

}  // namespace
}  // namespace workerd::jsg::test
