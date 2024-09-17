// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "dom-exception.h"
#include "jsg-test.h"
#include "jsg.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;
class ContextGlobalObject: public Object, public ContextGlobal {};

struct FreezeContext: public ContextGlobalObject {
  void recursivelyFreeze(v8::Local<v8::Value> value, v8::Isolate* isolate) {
    jsg::recursivelyFreeze(isolate->GetCurrentContext(), value);
  }
  JSG_RESOURCE_TYPE(FreezeContext) {
    JSG_METHOD(recursivelyFreeze);
  }
};
JSG_DECLARE_ISOLATE_TYPE(FreezeIsolate, FreezeContext);

KJ_TEST("recursive freezing") {
  Evaluator<FreezeContext, FreezeIsolate> e(v8System);
  e.expectEval("let obj = { foo: [ { bar: 1 } ] };\n"
               "recursivelyFreeze(obj);\n"
               // We rely on non-strict mode here to silently discard our mutations.
               "obj.foo[0].bar = 2;\n"
               "obj.foo[0].baz = 3;\n"
               "obj.foo[1] = { qux: 4 };\n"
               "obj.bar = {};\n"
               "JSON.stringify(obj);\n",
      "string", "{\"foo\":[{\"bar\":1}]}");
}

// ========================================================================================

struct CloneContext: public ContextGlobalObject {
  v8::Local<v8::Value> deepClone(v8::Local<v8::Value> value, v8::Isolate* isolate) {
    return jsg::deepClone(isolate->GetCurrentContext(), value);
  }
  JSG_RESOURCE_TYPE(CloneContext) {
    JSG_METHOD(deepClone);
  }
};
JSG_DECLARE_ISOLATE_TYPE(CloneIsolate, CloneContext);

KJ_TEST("deep clone") {
  Evaluator<CloneContext, CloneIsolate> e(v8System);
  e.expectEval(
      "let obj = { foo: [ { bar: 1 } ] };\n"
      "let clone = deepClone(obj);\n"
      "clone.foo[0].bar = 2;\n"
      "if (clone === obj) throw new Error('clone === obj');\n"
      "if (clone.foo[0] === obj.foo[0]) throw new Error('clone.foo[0] === obj.foo[0]');\n"
      "if (clone.foo[0].bar === obj.foo[0].bar) throw new Error('clone.foo[0].bar === obj.foo[0].bar');\n"
      "JSON.stringify(clone);\n",
      "string", "{\"foo\":[{\"bar\":2}]}");
}

// ========================================================================================

struct TypeErrorContext: public ContextGlobalObject {
  auto returnFunctionTakingBox(double value) {
    return [value](Lock&, Ref<NumberBox> value2) mutable { return value + value2->value; };
  }

  JSG_RESOURCE_TYPE(TypeErrorContext) {
    JSG_NESTED_TYPE(NumberBox);
    JSG_METHOD(returnFunctionTakingBox);
  }
};
JSG_DECLARE_ISOLATE_TYPE(TypeErrorIsolate, TypeErrorContext, NumberBox);

KJ_TEST("throw TypeError") {
  Evaluator<TypeErrorContext, TypeErrorIsolate> e(v8System);
  e.expectEval("new NumberBox(123).addBox(321)", "throws",
      "TypeError: Failed to execute 'addBox' on 'NumberBox': parameter 1 is not of "
      "type 'NumberBox'.");
  e.expectEval("new NumberBox(123).boxed = 321", "throws",
      "TypeError: Failed to set the 'boxed' property on 'NumberBox': the provided "
      "value is not of type 'NumberBox'.");
  e.expectEval("NumberBox(123)", "throws",
      "TypeError: Failed to construct 'NumberBox': Please use the 'new' operator, "
      "this object constructor cannot be called as a function.");
  e.expectEval("returnFunctionTakingBox(123)(321)", "throws",
      "TypeError: Failed to execute function: parameter 1 is not of type 'NumberBox'.");
}

// ========================================================================================

struct ThrowContext: public ContextGlobalObject {
  auto returnFunctionThatThrows(double value) {
    return [](Lock&, double) -> double { KJ_FAIL_ASSERT("thrown from returnFunctionThatThrows"); };
  }
  void throwException() {
    KJ_FAIL_REQUIRE("thrown from throwException");
  }
  double getThrowing() {
    KJ_FAIL_REQUIRE("thrown from getThrowing");
  }
  void setThrowing(double) {
    KJ_FAIL_REQUIRE("thrown from setThrowing");
  }
  JSG_RESOURCE_TYPE(ThrowContext) {
    JSG_METHOD(returnFunctionThatThrows);
    JSG_METHOD(throwException);
    JSG_INSTANCE_PROPERTY(throwing, getThrowing, setThrowing);
  }
};
JSG_DECLARE_ISOLATE_TYPE(ThrowIsolate, ThrowContext);

KJ_TEST("throw internal error") {
  Evaluator<ThrowContext, ThrowIsolate> e(v8System);
  {
    KJ_EXPECT_LOG(ERROR, "thrown from throwException");
    e.expectEval("throwException()", "throws", "Error: internal error");
  }

  {
    KJ_EXPECT_LOG(ERROR, "thrown from getThrowing");
    e.expectEval("throwing", "throws", "Error: internal error");
  }

  {
    KJ_EXPECT_LOG(ERROR, "thrown from setThrowing");
    e.expectEval("throwing = 123", "throws", "Error: internal error");
  }

  {
    KJ_EXPECT_LOG(ERROR, "thrown from returnFunctionThatThrows");
    e.expectEval("returnFunctionThatThrows(123)(321)", "throws", "Error: internal error");
  }
}

// ========================================================================================

struct TunneledContext: public ContextGlobalObject {
  void throwTunneledTypeError() {
    JSG_FAIL_REQUIRE(TypeError, "thrown from throwTunneledTypeError");
  }
  void throwTunneledTypeErrorWithoutMessage() {
    KJ_FAIL_REQUIRE("jsg.TypeError <unseen message>");
  }
  void throwTunneledTypeErrorLateColon() {
    KJ_FAIL_REQUIRE("jsg.TypeError would be an appropriate error to throw here, but that would "
                    "cause a problem: We actually don't want this top secret message to be visible "
                    "to developers!");
  }
  void throwTunneledTypeErrorWithExpectation() {
    auto s = kj::str("Hello, world!");
    JSG_REQUIRE(s.startsWith(";"), TypeError, "thrown from throwTunneledTypeErrorWithExpectation");
  }
  void throwTunneledOperationError() {
    JSG_FAIL_REQUIRE(DOMOperationError, "thrown from throwTunneledOperationError");
  }
  void throwTunneledOperationErrorWithoutMessage() {
    KJ_FAIL_REQUIRE("jsg.DOMException(OperationError) <unseen message>");
  }
  void throwTunneledOperationErrorLateColon() {
    KJ_FAIL_REQUIRE("jsg.DOMException(OperationError) would be an appropriate error to throw here "
                    "here, but that would cause a problem: We actually don't want this top secret "
                    "message to be visible to developers!");
  }
  void throwTunneledOperationErrorWithExpectation() {
    auto s = kj::str("Hello, world!");
    JSG_REQUIRE(s.startsWith(";"), DOMOperationError,
        "thrown from "
        "throwTunneledOperationErrorWithExpectation");
  }
  void throwTunneledInternalOperationError() {
    JSG_FAIL_REQUIRE(InternalDOMOperationError, "thrown from throwTunneledInternalOperationError");
  }
  void throwRemoteCpuExceededError() {
    kj::throwFatalException(KJ_EXCEPTION(OVERLOADED,
        "remote exception: remote exception: worker_do_not_log; script exceeded time limit",
        "script exceeded time limit"));
  }
  void throwBadTunneledError() {
    KJ_FAIL_REQUIRE(" jsg.TypeError");
  }
  void throwBadTunneledErrorWithExpectation() {
    auto s = kj::str("Hello, world!");
    KJ_REQUIRE(s.startsWith(";"), " jsg.TypeError");
  }
  void throwRetunneledTypeError(v8::Isolate* isolate) {
    // Not sure what to call this ...
    v8::TryCatch tryCatch(isolate);
    try {
      jsg::throwTypeError(isolate, "Dummy error message.");
      KJ_UNREACHABLE;
    } catch (JsExceptionThrown&) {
      throwTunneledException(isolate, tryCatch.Exception());
    }
  }
  void throwTunneledMacroTypeError() {
    JSG_FAIL_REQUIRE(TypeError, "thrown ", "from ", "throwTunneledMacroTypeError");
  }
  void throwTunneledMacroTypeErrorWithExpectation() {
    auto s = kj::str("Hello, world!");
    JSG_REQUIRE(
        s.startsWith(";"), TypeError, "thrown from throwTunneledMacroTypeErrorWithExpectation");
  }
  void throwTunneledMacroOperationError() {
    JSG_FAIL_REQUIRE(DOMOperationError, "thrown ", "from throwTunneledMacroOperationError");
  }
  void throwTunneledMacroOperationErrorWithExpectation() {
    auto s = kj::str("Hello, world!");
    JSG_REQUIRE(s.startsWith(";"), DOMOperationError, "thrown from ",
        kj::str("throwTunneledMacroOperationErrorWithExpectation"));
  }
  // Test that the error types mapped to WasmCompileError are handled correctly
  void throwTunneledCompileError() {
    KJ_FAIL_REQUIRE("jsg.CompileError: thrown from throwTunneledCompileError");
  }
  void throwTunneledLinkError() {
    KJ_FAIL_REQUIRE("jsg.LinkError: thrown from throwTunneledLinkError");
  }
  void throwTunneledRuntimeError() {
    KJ_FAIL_REQUIRE("jsg.RuntimeError: thrown from throwTunneledRuntimeError");
  }
  // Test that only valid DOM exceptions are processed
  void throwTunneledDOMException() {
    KJ_FAIL_REQUIRE("jsg.DOMException(Some error): thrown from throwTunneledDOMException");
  }
  void throwTunneledInvalidDOMException() {
    KJ_FAIL_REQUIRE("jsg.DOMException: thrown from throwTunneledInvalidDOMException");
  }
  void throwTunneledGarbledDOMException() {
    KJ_FAIL_REQUIRE("jsg.DOMException(: thrown from throwTunneledGarbledDOMException");
  }

  JSG_RESOURCE_TYPE(TunneledContext) {
    JSG_NESTED_TYPE(DOMException);
    JSG_METHOD(throwTunneledTypeError);
    JSG_METHOD(throwTunneledTypeErrorWithoutMessage);
    JSG_METHOD(throwTunneledTypeErrorLateColon);
    JSG_METHOD(throwTunneledTypeErrorWithExpectation);
    JSG_METHOD(throwTunneledOperationError);
    JSG_METHOD(throwTunneledOperationErrorWithoutMessage);
    JSG_METHOD(throwTunneledOperationErrorLateColon);
    JSG_METHOD(throwTunneledOperationErrorWithExpectation);
    JSG_METHOD(throwTunneledInternalOperationError);
    JSG_METHOD(throwRemoteCpuExceededError);
    JSG_METHOD(throwBadTunneledError);
    JSG_METHOD(throwBadTunneledErrorWithExpectation);
    JSG_METHOD(throwRetunneledTypeError);
    JSG_METHOD(throwTunneledMacroTypeError);
    JSG_METHOD(throwTunneledMacroTypeErrorWithExpectation);
    JSG_METHOD(throwTunneledMacroOperationError);
    JSG_METHOD(throwTunneledMacroOperationErrorWithExpectation);
    JSG_METHOD(throwTunneledCompileError);
    JSG_METHOD(throwTunneledLinkError);
    JSG_METHOD(throwTunneledRuntimeError);
    JSG_METHOD(throwTunneledDOMException);
    JSG_METHOD(throwTunneledInvalidDOMException);
    JSG_METHOD(throwTunneledGarbledDOMException);
  }
};
JSG_DECLARE_ISOLATE_TYPE(TunneledIsolate, TunneledContext);

KJ_TEST("throw tunneled exception") {
  Evaluator<TunneledContext, TunneledIsolate> e(v8System);
  e.expectEval(
      "throwTunneledTypeError()", "throws", "TypeError: thrown from throwTunneledTypeError");
  e.expectEval("throwTunneledTypeErrorLateColon()", "throws", "TypeError");
  e.expectEval("throwTunneledTypeErrorWithExpectation()", "throws",
      "TypeError: thrown from throwTunneledTypeErrorWithExpectation");
  e.expectEval("throwTunneledOperationError()", "throws",
      "OperationError: thrown from throwTunneledOperationError");
  e.expectEval("throwTunneledOperationErrorWithoutMessage()", "throws", "OperationError");
  e.expectEval("throwTunneledOperationErrorLateColon()", "throws", "OperationError");
  e.expectEval("throwTunneledOperationErrorWithExpectation()", "throws",
      "OperationError: thrown from throwTunneledOperationErrorWithExpectation");
  {
    KJ_EXPECT_LOG(ERROR, "thrown from throwTunneledInternalOperationError");
    e.expectEval(
        "throwTunneledInternalOperationError()", "throws", "OperationError: internal error");
  }
  {
    KJ_EXPECT_LOG(ERROR, " jsg.TypeError");
    e.expectEval("throwBadTunneledError()", "throws", "Error: internal error");
  }
  {
    KJ_EXPECT_LOG(ERROR, "expected s.startsWith(\";\");  jsg.TypeError");
    e.expectEval("throwBadTunneledErrorWithExpectation()", "throws", "Error: internal error");
  }
  e.expectEval("throwTunneledMacroTypeError()", "throws",
      "TypeError: thrown from throwTunneledMacroTypeError");
  e.expectEval("throwTunneledMacroTypeErrorWithExpectation()", "throws",
      "TypeError: thrown from throwTunneledMacroTypeErrorWithExpectation");
  e.expectEval("throwTunneledMacroOperationError()", "throws",
      "OperationError: thrown from throwTunneledMacroOperationError");
  e.expectEval("throwTunneledMacroOperationErrorWithExpectation()", "throws",
      "OperationError: thrown from throwTunneledMacroOperationErrorWithExpectation");
  e.expectEval("throwTunneledCompileError()", "throws",
      "CompileError: thrown from throwTunneledCompileError");
  e.expectEval(
      "throwTunneledLinkError()", "throws", "CompileError: thrown from throwTunneledLinkError");
  e.expectEval("throwTunneledRuntimeError()", "throws",
      "CompileError: thrown from throwTunneledRuntimeError");
  e.expectEval(
      "throwTunneledDOMException()", "throws", "Some error: thrown from throwTunneledDOMException");
  {
    KJ_EXPECT_LOG(ERROR, " thrown from throwTunneledInvalidDOMException");
    e.expectEval("throwTunneledInvalidDOMException()", "throws", "Error: internal error");
  }
  {
    KJ_EXPECT_LOG(ERROR, " thrown from throwTunneledGarbledDOMException");
    e.expectEval("throwTunneledGarbledDOMException()", "throws", "Error: internal error");
  }
}

KJ_TEST("runTunnelingExceptions") {
  Evaluator<TunneledContext, TunneledIsolate> e(v8System);
  e.expectEval("throwRetunneledTypeError()", "throws", "TypeError: Dummy error message.");
}

KJ_TEST("isTunneledException") {
  TunneledContext context;
  try {
    context.throwTunneledTypeError();
    KJ_UNREACHABLE;
  } catch (kj::Exception e) {
    KJ_EXPECT(isTunneledException(e.getDescription()), e.getDescription());
  }
  try {
    context.throwTunneledTypeErrorWithoutMessage();
    KJ_UNREACHABLE;
  } catch (kj::Exception e) {
    KJ_EXPECT(isTunneledException(e.getDescription()), e.getDescription());
  }
  try {
    context.throwTunneledTypeErrorLateColon();
    KJ_UNREACHABLE;
  } catch (kj::Exception e) {
    KJ_EXPECT(isTunneledException(e.getDescription()), e.getDescription());
  }
  try {
    context.throwTunneledTypeErrorWithExpectation();
    KJ_UNREACHABLE;
  } catch (kj::Exception e) {
    KJ_EXPECT(isTunneledException(e.getDescription()), e.getDescription());
  }
  try {
    context.throwTunneledOperationError();
    KJ_UNREACHABLE;
  } catch (kj::Exception e) {
    KJ_EXPECT(isTunneledException(e.getDescription()), e.getDescription());
  }
  try {
    context.throwTunneledOperationErrorLateColon();
    KJ_UNREACHABLE;
  } catch (kj::Exception e) {
    KJ_EXPECT(isTunneledException(e.getDescription()), e.getDescription());
  }
  try {
    context.throwTunneledOperationErrorWithExpectation();
    KJ_UNREACHABLE;
  } catch (kj::Exception e) {
    KJ_EXPECT(isTunneledException(e.getDescription()), e.getDescription());
  }

  try {
    context.throwBadTunneledError();
    KJ_UNREACHABLE;
  } catch (kj::Exception e) {
    KJ_EXPECT(!isTunneledException(e.getDescription()), e.getDescription());
  }
  try {
    context.throwBadTunneledErrorWithExpectation();
    KJ_UNREACHABLE;
  } catch (kj::Exception e) {
    KJ_EXPECT(!isTunneledException(e.getDescription()), e.getDescription());
  }

  try {
    context.throwRemoteCpuExceededError();
    KJ_UNREACHABLE;
  } catch (kj::Exception e) {
    KJ_EXPECT(!isTunneledException(e.getDescription()), e.getDescription());
    KJ_EXPECT(isDoNotLogException(e.getDescription()), e.getDescription());
  }

  try {
    JSG_FAIL_REQUIRE(InternalDOMOperationError, "foo");
    KJ_UNREACHABLE;
  } catch (kj::Exception e) {
    KJ_EXPECT(!isTunneledException(e.getDescription()), e.getDescription());
  }
}

}  // namespace
}  // namespace workerd::jsg::test
