// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "dom-exception.h"
#include "jsg-test.h"
#include "jsg.h"

#include <workerd/jsg/exception-metadata.capnp.h>

#include <capnp/message.h>
#include <capnp/serialize.h>
#include <kj/mutex.h>

namespace workerd::jsg::test {
namespace {

V8System v8System;
class ContextGlobalObject: public Object, public ContextGlobal {};

struct FreezeContext: public ContextGlobalObject {
  void recursivelyFreeze(jsg::Lock& js, v8::Local<v8::Value> value) {
    jsg::recursivelyFreeze(js.v8Isolate->GetCurrentContext(), value);
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
  v8::Local<v8::Value> deepClone(jsg::Lock& js, v8::Local<v8::Value> value) {
    return jsg::deepClone(js.v8Isolate->GetCurrentContext(), value);
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
  setPredictableModeForTest();

  Evaluator<ThrowContext, ThrowIsolate> e(v8System);
  {
    KJ_EXPECT_LOG(ERROR, "thrown from throwException");
    e.expectEval("throwException()", "throws",
        "Error: internal error; reference = 0123456789abcdefghijklmn");
  }
  {
    // We also expect the logged internal error to contain the error id.
    KJ_EXPECT_LOG(ERROR, "wdErrId = 0123456789abcdefghijklmn");
    e.expectEval("throwException()", "throws",
        "Error: internal error; reference = 0123456789abcdefghijklmn");
  }

  {
    KJ_EXPECT_LOG(ERROR, "thrown from getThrowing");
    e.expectEval(
        "throwing", "throws", "Error: internal error; reference = 0123456789abcdefghijklmn");
  }

  {
    KJ_EXPECT_LOG(ERROR, "thrown from setThrowing");
    e.expectEval(
        "throwing = 123", "throws", "Error: internal error; reference = 0123456789abcdefghijklmn");
  }

  {
    KJ_EXPECT_LOG(ERROR, "thrown from returnFunctionThatThrows");
    e.expectEval("returnFunctionThatThrows(123)(321)", "throws",
        "Error: internal error; reference = 0123456789abcdefghijklmn");
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
    kj::throwException(KJ_EXCEPTION(OVERLOADED,
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
  void throwRetunneledTypeError(jsg::Lock& js) {
    // Not sure what to call this ...
    // NOLINTNEXTLINE(custom-raw-v8-trycatch): test: only tunnels the TypeError it threw.
    v8::TryCatch tryCatch(js.v8Isolate);
    try {
      jsg::throwTypeError(js.v8Isolate, "Dummy error message.");
      KJ_UNREACHABLE;
    } catch (JsExceptionThrown&) {
      throwTunneledException(js.v8Isolate, tryCatch.Exception());
    }
  }
  void throwHugeExternalString(jsg::Lock& js) {
    kj::ArrayPtr<const char> fakeBuf(reinterpret_cast<const char*>(1), v8::String::kMaxLength + 1);
    jsg::newExternalOneByteString(js, fakeBuf);
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
    JSG_METHOD(throwHugeExternalString);
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
  setPredictableModeForTest();

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
    e.expectEval("throwTunneledInternalOperationError()", "throws",
        "OperationError: internal error; reference = 0123456789abcdefghijklmn");
  }
  {
    // We also expect the logged internal error to contain the error id.
    KJ_EXPECT_LOG(ERROR, "wdErrId = 0123456789abcdefghijklmn");
    e.expectEval("throwTunneledInternalOperationError()", "throws",
        "OperationError: internal error; reference = 0123456789abcdefghijklmn");
  }
  {
    KJ_EXPECT_LOG(ERROR, " jsg.TypeError");
    e.expectEval("throwBadTunneledError()", "throws",
        "Error: internal error; reference = 0123456789abcdefghijklmn");
  }
  {
    KJ_EXPECT_LOG(ERROR, "expected s.startsWith(\";\");  jsg.TypeError");
    e.expectEval("throwBadTunneledErrorWithExpectation()", "throws",
        "Error: internal error; reference = 0123456789abcdefghijklmn");
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
      "throwTunneledLinkError()", "throws", "LinkError: thrown from throwTunneledLinkError");
  e.expectEval("throwTunneledRuntimeError()", "throws",
      "RuntimeError: thrown from throwTunneledRuntimeError");
  e.expectEval(
      "throwTunneledDOMException()", "throws", "Some error: thrown from throwTunneledDOMException");
  {
    KJ_EXPECT_LOG(ERROR, " thrown from throwTunneledInvalidDOMException");
    e.expectEval("throwTunneledInvalidDOMException()", "throws",
        "Error: internal error; reference = 0123456789abcdefghijklmn");
  }
  {
    KJ_EXPECT_LOG(ERROR, " thrown from throwTunneledGarbledDOMException");
    e.expectEval("throwTunneledGarbledDOMException()", "throws",
        "Error: internal error; reference = 0123456789abcdefghijklmn");
  }
}

KJ_TEST("runTunnelingExceptions") {
  Evaluator<TunneledContext, TunneledIsolate> e(v8System);
  e.expectEval("throwRetunneledTypeError()", "throws", "TypeError: Dummy error message.");
  e.expectEval("throwHugeExternalString()", "throws", "Error: String allocation failed");
}

KJ_TEST("isTunneledException") {
  auto context = kj::rc<TunneledContext>();
  try {
    context->throwTunneledTypeError();
    KJ_UNREACHABLE;
  } catch (kj::Exception& e) {
    KJ_EXPECT(isTunneledException(e.getDescription()), e.getDescription());
  }
  try {
    context->throwTunneledTypeErrorWithoutMessage();
    KJ_UNREACHABLE;
  } catch (kj::Exception& e) {
    KJ_EXPECT(isTunneledException(e.getDescription()), e.getDescription());
  }
  try {
    context->throwTunneledTypeErrorLateColon();
    KJ_UNREACHABLE;
  } catch (kj::Exception& e) {
    KJ_EXPECT(isTunneledException(e.getDescription()), e.getDescription());
  }
  try {
    context->throwTunneledTypeErrorWithExpectation();
    KJ_UNREACHABLE;
  } catch (kj::Exception& e) {
    KJ_EXPECT(isTunneledException(e.getDescription()), e.getDescription());
  }
  try {
    context->throwTunneledOperationError();
    KJ_UNREACHABLE;
  } catch (kj::Exception& e) {
    KJ_EXPECT(isTunneledException(e.getDescription()), e.getDescription());
  }
  try {
    context->throwTunneledOperationErrorLateColon();
    KJ_UNREACHABLE;
  } catch (kj::Exception& e) {
    KJ_EXPECT(isTunneledException(e.getDescription()), e.getDescription());
  }
  try {
    context->throwTunneledOperationErrorWithExpectation();
    KJ_UNREACHABLE;
  } catch (kj::Exception& e) {
    KJ_EXPECT(isTunneledException(e.getDescription()), e.getDescription());
  }

  try {
    context->throwBadTunneledError();
    KJ_UNREACHABLE;
  } catch (kj::Exception& e) {
    KJ_EXPECT(!isTunneledException(e.getDescription()), e.getDescription());
  }
  try {
    context->throwBadTunneledErrorWithExpectation();
    KJ_UNREACHABLE;
  } catch (kj::Exception& e) {
    KJ_EXPECT(!isTunneledException(e.getDescription()), e.getDescription());
  }

  try {
    context->throwRemoteCpuExceededError();
    KJ_UNREACHABLE;
  } catch (kj::Exception& e) {
    KJ_EXPECT(!isTunneledException(e.getDescription()), e.getDescription());
    KJ_EXPECT(isDoNotLogException(e.getDescription()), e.getDescription());
  }

  try {
    JSG_FAIL_REQUIRE(InternalDOMOperationError, "foo");
    KJ_UNREACHABLE;
  } catch (kj::Exception& e) {
    KJ_EXPECT(!isTunneledException(e.getDescription()), e.getDescription());
  }
}

// ========================================================================================

struct RetryableContext: public ContextGlobalObject {
  void throwException(kj::String type, kj::String description, bool transient) {
    auto exceptionType = type == "overloaded" ? kj::Exception::Type::OVERLOADED
        : type == "disconnected"              ? kj::Exception::Type::DISCONNECTED
                                              : kj::Exception::Type::FAILED;
    kj::Exception exception(exceptionType, __FILE__, __LINE__, kj::mv(description));
    if (transient) markTransient(exception);
    kj::throwException(kj::mv(exception));
  }
  JSG_RESOURCE_TYPE(RetryableContext) {
    JSG_METHOD(throwException);
  }
};
JSG_DECLARE_ISOLATE_TYPE(RetryableIsolate, RetryableContext);

KJ_TEST("retryable reflects DISCONNECTED and the transient marker") {
  Evaluator<RetryableContext, RetryableIsolate> e(v8System);
  auto flags = [&](kj::StringPtr args) {
    return kj::str("try { throwException(", args,
        "); } catch (e) { `${e.retryable === true} ${e.overloaded === true}` }");
  };
  e.expectEval(flags("'failed', 'jsg.Error: unmarked', false"), "string", "false false");
  e.expectEval(flags("'disconnected', 'jsg.Error: disconnected', false"), "string", "true false");
  e.expectEval(flags("'failed', 'jsg.Error: marked', true"), "string", "true false");
  e.expectEval(flags("'overloaded', 'jsg.Error: marked', true"), "string", "true true");
  {
    KJ_EXPECT_LOG(ERROR, "marked internal failure");
    e.expectEval(flags("'failed', 'marked internal failure', true"), "string", "true false");
  }
}

// ========================================================================================
// describeAsJsError()

constexpr kj::StringPtr INTERNAL_ERROR_MESSAGE =
    "internal error; reference = 0123456789abcdefghijklmn"_kj;

kj::Exception failure(kj::StringPtr description) {
  return kj::Exception(kj::Exception::Type::FAILED, __FILE__, __LINE__, kj::str(description));
}

void expectDescription(const JsErrorDescription& description,
    kj::StringPtr name,
    kj::StringPtr message,
    kj::Maybe<kj::StringPtr> stackPrefix = kj::none) {
  KJ_EXPECT(description.name == name, description.name);
  KJ_EXPECT(description.message == message, description.message);
  KJ_IF_SOME(prefix, stackPrefix) {
    auto& stack = KJ_ASSERT_NONNULL(description.stack, "expected a stack");
    KJ_EXPECT(stack.startsWith(prefix), stack);
  } else {
    KJ_EXPECT(description.stack == kj::none, description.stack);
  }
}

KJ_TEST("describeAsJsError: errors tunneled by description") {
  auto expectTunneled = [](kj::StringPtr description, kj::StringPtr name, kj::StringPtr message) {
    KJ_CONTEXT(description);
    auto result = describeAsJsError(failure(description), kj::none);
    expectDescription(result, name, message);
    KJ_EXPECT(result.internalException == kj::none);
  };

  expectTunneled("jsg.Error: plain", "Error", "plain");
  expectTunneled("jsg.TypeError: wrong type", "TypeError", "wrong type");
  expectTunneled("jsg.RangeError:   spaced", "RangeError", "spaced");
  expectTunneled("jsg.CompileError: bad wasm", "CompileError", "bad wasm");
  expectTunneled("jsg.SuppressedError: suppressed", "SuppressedError", "suppressed");
  // The text after the type is the message only if a colon introduces it.
  expectTunneled("jsg.TypeError", "TypeError", "");
  expectTunneled("jsg.TypeError <unseen message>", "TypeError", "");
  // The first type that the description starts with wins.
  expectTunneled("jsg.ErrorLike: message", "Error", "");

  expectTunneled("jsg.DOMException(OperationError): failed", "OperationError", "failed");
  expectTunneled("jsg.DOMException(Some error): failed", "Some error", "failed");
  expectTunneled("jsg.DOMException(OperationError) <unseen message>", "OperationError", "");
  // An unclosed "DOMException(" is skipped, and what follows names the type.
  expectTunneled("jsg.DOMException(TypeError: unclosed", "TypeError", "unclosed");

  // Prefixes that say where the error has been do not change what it is.
  expectTunneled("remote.jsg.TypeError: from afar", "TypeError", "from afar");
  expectTunneled("remote.remote.jsg.Error: from further", "Error", "from further");
  expectTunneled("remote exception: remote.jsg.Error: over RPC", "Error", "over RPC");
  expectTunneled("broken.outputGateBroken; jsg.Error: gate broke", "Error", "gate broke");
  expectTunneled("remote.broken.inputGateBroken; jsg.DOMException(AbortError): aborted",
      "AbortError", "aborted");
  expectTunneled("expected s.startsWith(\";\"); jsg.TypeError: required", "TypeError", "required");

  // A tunneled error is described as tunneled whatever the exception's type, and is not logged.
  auto result =
      describeAsJsError(KJ_EXCEPTION(DISCONNECTED, "jsg.Error: tunneled disconnect"), kj::none);
  expectDescription(result, "Error", "tunneled disconnect");
  KJ_EXPECT(result.internalException == kj::none);
}

KJ_TEST("describeAsJsError: internal exceptions") {
  setPredictableModeForTest();

  auto expectId = [](const JsErrorDescription& description) {
    auto& detail = KJ_ASSERT_NONNULL(description.internalException);
    KJ_EXPECT(detail.isInternal);
    auto& id = KJ_ASSERT_NONNULL(detail.internalErrorId);
    KJ_EXPECT(kj::str(id) == "0123456789abcdefghijklmn");
  };

  {
    // The description is logged, with the id that the message refers to, and hidden.
    KJ_EXPECT_LOG(ERROR,
        "something broke; sentryErrorContext = jsgInternalError; "
        "wdErrId = 0123456789abcdefghijklmn");
    auto result = describeAsJsError(failure("something broke"), kj::none);
    expectDescription(result, "Error", INTERNAL_ERROR_MESSAGE);
    expectId(result);
    auto& detail = KJ_ASSERT_NONNULL(result.internalException);
    KJ_EXPECT(!detail.isFromRemote);
    KJ_EXPECT(!detail.isDurableObjectReset);
  }

  {
    KJ_EXPECT_LOG(ERROR, "storage failed");
    auto result =
        describeAsJsError(failure("remote.broken.outputGateBroken; storage failed"), kj::none);
    expectDescription(result, "Error", INTERNAL_ERROR_MESSAGE);
    expectId(result);
    auto& detail = KJ_ASSERT_NONNULL(result.internalException);
    KJ_EXPECT(detail.isFromRemote);
    KJ_EXPECT(detail.isDurableObjectReset);
  }

  {
    // An internal error with a type keeps the type and hides only the message.
    KJ_EXPECT_LOG(ERROR, "jsg-internal.TypeError: secret");
    auto result = describeAsJsError(failure("jsg-internal.TypeError: secret"), kj::none);
    expectDescription(result, "TypeError", INTERNAL_ERROR_MESSAGE);
    expectId(result);
  }

  {
    KJ_EXPECT_LOG(ERROR, "jsg-internal.DOMException(OperationError): secret");
    auto result =
        describeAsJsError(failure("jsg-internal.DOMException(OperationError): secret"), kj::none);
    expectDescription(result, "OperationError", INTERNAL_ERROR_MESSAGE);
    expectId(result);
  }

  {
    // Not a tunneled error: "jsg." has to come first.
    KJ_EXPECT_LOG(ERROR, " jsg.TypeError");
    auto result = describeAsJsError(failure(" jsg.TypeError"), kj::none);
    expectDescription(result, "Error", INTERNAL_ERROR_MESSAGE);
    expectId(result);
  }

  {
    // Neither a DOMException nor a type follows "jsg.".
    KJ_EXPECT_LOG(ERROR, "jsg.DOMException: no name");
    auto result = describeAsJsError(failure("jsg.DOMException: no name"), kj::none);
    expectDescription(result, "Error", INTERNAL_ERROR_MESSAGE);
    expectId(result);
  }

  {
    // A disconnection is not worth an id, and is logged only at INFO level, which a failing
    // KJ_EXPECT_LOG(ERROR) would show.
    auto result = describeAsJsError(KJ_EXCEPTION(DISCONNECTED, "peer went away"), kj::none);
    expectDescription(result, "Error", "Network connection lost.");
    auto& detail = KJ_ASSERT_NONNULL(result.internalException);
    KJ_EXPECT(detail.isInternal);
    KJ_EXPECT(detail.internalErrorId == kj::none);
  }

  {
    // Nor is an exception marked as not worth logging, though its message still has an id.
    auto result = describeAsJsError(
        KJ_EXCEPTION(OVERLOADED, "remote exception: worker_do_not_log; script exceeded time limit"),
        kj::none);
    expectDescription(result, "Error", INTERNAL_ERROR_MESSAGE);
    auto& detail = KJ_ASSERT_NONNULL(result.internalException);
    KJ_EXPECT(detail.isInternal);
    KJ_EXPECT(detail.internalErrorId == kj::none);
  }
}

// Records what it is told about internal exceptions.
struct InternalExceptionRecorder final: public InternalExceptionObserver {
  void reportInternalException(const kj::Exception& exception, Detail detail) const override {
    reports.lockExclusive()->add(Report{kj::str(exception.getDescription()), detail});
  }

  struct Report {
    kj::String description;
    Detail detail;
  };
  kj::MutexGuarded<kj::Vector<Report>> reports;
};

struct DescribeContext: public ContextGlobalObject {
  JSG_RESOURCE_TYPE(DescribeContext) {
    JSG_NESTED_TYPE(DOMException);
  }
};
JSG_DECLARE_ISOLATE_TYPE(DescribeIsolate, DescribeContext);
JSG_DECLARE_ISOLATE_TYPE(EnhancedDescribeIsolate, DescribeContext);

// Runs `code`, which has to throw, and returns what it threw as a kj::Exception.
template <typename IsolateType>
kj::Exception tunnelThrown(kj::StringPtr code) {
  Evaluator<DescribeContext, IsolateType> e(v8System);
  kj::Maybe<kj::Exception> result;
  e.getIsolate().runInLockScope([&](IsolateType::Lock& lock) {
    if constexpr (kj::isSameType<IsolateType, EnhancedDescribeIsolate>()) {
      lock.setUsingEnhancedErrorSerialization();
    }
    JSG_WITHIN_CONTEXT_SCOPE(lock,
        lock.template newContext<DescribeContext>().getHandle(lock.v8Isolate), [&](jsg::Lock& js) {
      js.tryCatch([&]() {
        auto script = check(v8::Script::Compile(js.v8Context(), jsg::v8Str(js.v8Isolate, code)));
        check(script->Run(js.v8Context()));
        KJ_FAIL_ASSERT("the code did not throw", code);
      }, [&](Value exception) { result = js.exceptionToKj(kj::mv(exception)); });
    });
  });
  return KJ_ASSERT_NONNULL(kj::mv(result));
}

KJ_TEST("describeAsJsError: internal exceptions are reported to the observer") {
  setPredictableModeForTest();
  InternalExceptionRecorder observer;

  {
    KJ_EXPECT_LOG(ERROR, "storage failed");
    auto result =
        describeAsJsError(failure("remote.broken.outputGateBroken; storage failed"), observer);
    auto reports = observer.reports.lockExclusive();
    KJ_ASSERT(reports->size() == 1);
    auto& report = (*reports)[0];
    KJ_EXPECT(report.description == "remote.broken.outputGateBroken; storage failed");
    KJ_EXPECT(report.detail.isInternal);
    KJ_EXPECT(report.detail.isFromRemote);
    KJ_EXPECT(report.detail.isDurableObjectReset);
    KJ_EXPECT(
        kj::str(KJ_ASSERT_NONNULL(report.detail.internalErrorId)) == "0123456789abcdefghijklmn");
    // The description has what the observer was told.
    auto& detail = KJ_ASSERT_NONNULL(result.internalException);
    KJ_EXPECT(detail.isFromRemote);
    KJ_EXPECT(detail.internalErrorId != kj::none);
  }

  {
    // An exception that is not logged under an id is reported without one.
    describeAsJsError(KJ_EXCEPTION(DISCONNECTED, "peer went away"), observer);
    auto reports = observer.reports.lockExclusive();
    KJ_ASSERT(reports->size() == 2);
    KJ_EXPECT((*reports)[1].detail.isInternal);
    KJ_EXPECT((*reports)[1].detail.internalErrorId == kj::none);
  }

  // An error that JavaScript sees in full is not reported.
  describeAsJsError(failure("jsg.TypeError: wrong type"), observer);
  describeAsJsError(tunnelThrown<DescribeIsolate>("throw new Error('boom');"), observer);
  KJ_EXPECT(observer.reports.lockExclusive()->size() == 2);
}

constexpr kj::StringPtr THROW_CUSTOM_ERROR =
    "class CustomError extends Error {\n"
    "  constructor(message) {\n"
    "    super(message);\n"
    "    this.name = 'CustomError';\n"
    "  }\n"
    "}\n"
    "function thrower() { throw new CustomError('boom'); }\n"
    "thrower();"_kj;

template <typename IsolateType>
void expectTunneledJsErrors() {
  auto expectThrown = [](kj::StringPtr code, kj::StringPtr name, kj::StringPtr message,
                          kj::Maybe<kj::StringPtr> stackPrefix = kj::none) {
    KJ_CONTEXT(code);
    auto result = describeAsJsError(tunnelThrown<IsolateType>(code), kj::none);
    expectDescription(result, name, message, stackPrefix);
    KJ_EXPECT(result.internalException == kj::none);
  };

  // An error object is described by its own properties, stack included.
  expectThrown("function thrower() { throw new Error('boom'); }\nthrower();", "Error", "boom",
      "Error: boom\n    at thrower ("_kj);
  expectThrown("function thrower() { throw new TypeError('wrong'); }\nthrower();", "TypeError",
      "wrong", "TypeError: wrong\n    at thrower ("_kj);
  expectThrown("function thrower() { throw new Error('two\\nlines'); }\nthrower();", "Error",
      "two\nlines", "Error: two\nlines\n    at thrower ("_kj);
  expectThrown("function thrower() { throw new Error(); }\nthrower();", "Error", "",
      "Error\n    at thrower ("_kj);
  expectThrown(
      "const e = new RangeError('no stack'); delete e.stack; e.stack = undefined; throw e;",
      "RangeError", "no stack");

  // So is any other object with a message.
  expectThrown("throw { name: 'Custom', message: 'from an object' };", "Custom", "from an object");
  expectThrown("throw { message: 'no name', stack: 'made up' };", "Error", "no name", "made up"_kj);

  // A DOMException is rebuilt from its name and message, which leaves no stack.
  expectThrown("throw new DOMException('aborted', 'AbortError');", "AbortError", "aborted");

  // A value that is not an object becomes the message of an Error.
  expectThrown("throw 'just a string';", "Error", "just a string");
  expectThrown("throw 42;", "Error", "42");

  // An object without a string for a message is described by how it was stringified when it was
  // tunneled.
  expectThrown("throw { foo: 1 };", "Error", "#<Object>");
  expectThrown("throw { name: 'Custom', foo: 1 };", "Custom", "#<Object>");
}

KJ_TEST("describeAsJsError: tunneled JavaScript errors") {
  expectTunneledJsErrors<DescribeIsolate>();

  // V8's error serialization does not keep a name that is not a standard one, so neither does the
  // error that exceptionToJs() returns.
  auto result = describeAsJsError(tunnelThrown<DescribeIsolate>(THROW_CUSTOM_ERROR), kj::none);
  expectDescription(result, "Error", "boom", "CustomError: boom\n    at thrower ("_kj);

  // That only goes for errors. Another object keeps all of its properties.
  result = describeAsJsError(
      tunnelThrown<DescribeIsolate>(
          "throw { name: 'CustomError', message: 'boom', stack: 'CustomError: boom\\n    at x' };"),
      kj::none);
  expectDescription(result, "CustomError", "boom", "CustomError: boom\n    at x"_kj);
}

KJ_TEST("describeAsJsError: tunneled JavaScript errors, enhanced error serialization") {
  expectTunneledJsErrors<EnhancedDescribeIsolate>();

  auto result =
      describeAsJsError(tunnelThrown<EnhancedDescribeIsolate>(THROW_CUSTOM_ERROR), kj::none);
  expectDescription(result, "CustomError", "boom", "CustomError: boom\n    at thrower ("_kj);
}

KJ_TEST("describeAsJsError: tunneled JavaScript error properties are cut off at 4KB") {
  {
    // The message is 4094 bytes of ASCII followed by three-byte characters, the first of which
    // straddles the limit.
    auto result = describeAsJsError(
        tunnelThrown<DescribeIsolate>("throw new Error('x'.repeat(4094) + '\\u20ac'.repeat(10));"),
        kj::none);
    KJ_EXPECT(result.message.size() == 4094, result.message.size());
    KJ_EXPECT(result.message.endsWith("xxx"));
    // The 4KB kept of the stack end within the message that the stack starts with. A piece of
    // the message is not a stack.
    KJ_EXPECT(result.stack == kj::none, result.stack);
  }

  {
    // The same goes for an error whose name is not the one its type has, as the stack starts
    // with the name it was thrown with.
    auto result = describeAsJsError(
        tunnelThrown<DescribeIsolate>(
            "const e = new Error('y'.repeat(5000)); e.name = 'CustomError'; throw e;"),
        kj::none);
    expectDescription(result, "Error", kj::str(kj::repeat('y', 4096)));
  }

  {
    // A stack that is cut off after its first frames is kept.
    auto result = describeAsJsError(
        tunnelThrown<DescribeIsolate>("function thrower(depth) {\n"
                                      "  if (depth > 0) return thrower(depth - 1);\n"
                                      "  throw new Error('z'.repeat(3900));\n"
                                      "}\n"
                                      "thrower(8);"),
        kj::none);
    KJ_EXPECT(result.message.size() == 3900, result.message.size());
    auto& stack = KJ_ASSERT_NONNULL(result.stack);
    KJ_EXPECT(stack.size() == 4096, stack.size());
    KJ_EXPECT(stack.startsWith(kj::str("Error: ", result.message, "\n    at thrower (")), stack);
  }
}

void setJsExceptionMetadata(
    kj::Exception& exception, kj::Function<void(JsExceptionMetadata::Builder)> init) {
  capnp::MallocMessageBuilder message;
  init(message.initRoot<JsExceptionMetadata>());
  auto words = capnp::messageToFlatArray(message);
  exception.setDetail(JS_EXCEPTION_METADATA_DETAIL_ID, kj::heapArray(words.asBytes()));
}

KJ_TEST("describeAsJsError: metadata that does not describe the error") {
  auto stack = "TypeError: old\n    at somewhere (worker.js:1:1)"_kj;

  {
    // Metadata written before it had a message: the description provides the message.
    auto exception = failure("jsg.TypeError: old");
    exception.setDetail(TUNNELED_EXCEPTION_DETAIL_ID, kj::heapArray<kj::byte>({0}));
    setJsExceptionMetadata(exception, [&](JsExceptionMetadata::Builder metadata) {
      metadata.setErrorType("TypeError");
      metadata.setStackTrace(stack);
    });
    expectDescription(describeAsJsError(exception, kj::none), "TypeError", "old", stack);
  }

  {
    // Without a serialized copy of the error, exceptionToJs() makes a new error from the
    // description, which has no stack.
    auto exception = failure("jsg.TypeError: not serializable");
    setJsExceptionMetadata(exception, [&](JsExceptionMetadata::Builder metadata) {
      metadata.setErrorType("CustomError");
      metadata.setMessage("something else");
      metadata.setStackTrace(stack);
    });
    expectDescription(describeAsJsError(exception, kj::none), "TypeError", "not serializable");
  }

  {
    // Metadata that is not a valid message is ignored.
    auto exception = failure("jsg.TypeError: garbled");
    exception.setDetail(TUNNELED_EXCEPTION_DETAIL_ID, kj::heapArray<kj::byte>({0}));
    exception.setDetail(JS_EXCEPTION_METADATA_DETAIL_ID, kj::heapArray<kj::byte>({1, 2, 3}));
    expectDescription(describeAsJsError(exception, kj::none), "TypeError", "garbled");

    auto garbage = kj::heapArray<kj::byte>(64);
    garbage.asPtr().fill(0xff);
    exception.setDetail(JS_EXCEPTION_METADATA_DETAIL_ID, kj::mv(garbage));
    expectDescription(describeAsJsError(exception, kj::none), "TypeError", "garbled");
  }
}

KJ_TEST("exceptionToJs() does not log an exception that was already described again") {
  setPredictableModeForTest();

  auto exception = KJ_EXCEPTION(OVERLOADED, "remote.broken.outputGateBroken; storage failed");
  markTransient(exception);
  addDurableObjectId(exception, "some-durable-object");

  auto description = [&]() {
    KJ_EXPECT_LOG(ERROR, "storage failed");
    return describeAsJsError(exception, kj::none);
  }();
  // An id that no other conversion comes up with.
  auto& id = KJ_ASSERT_NONNULL(description.internalErrorId);
  id.asPtr().fill('z');
  KJ_ASSERT_NONNULL(description.internalException).internalErrorId = id;
  description.message = kj::str("internal error; reference = ", id);

  Evaluator<DescribeContext, DescribeIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](DescribeIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock,
        lock.template newContext<DescribeContext>().getHandle(lock.v8Isolate), [&](jsg::Lock& js) {
      // An ERROR log here would fail the test.
      auto error =
          KJ_ASSERT_NONNULL(JsValue(exceptionToJs(js.v8Isolate, exception.clone(),
                                        {.trusted = true, .alreadyDescribed = description}))
                                .tryCast<JsObject>());

      // The error refers to the id that the exception was logged under, and is otherwise the one
      // that any conversion makes.
      KJ_EXPECT(kj::str(error.get(js, "message")) == description.message);
      KJ_EXPECT(error.get(js, "remote").isTrue());
      KJ_EXPECT(error.get(js, "retryable").isTrue());
      KJ_EXPECT(error.get(js, "overloaded").isTrue());
      KJ_EXPECT(error.get(js, "durableObjectReset").isTrue());
      KJ_EXPECT(kj::str(error.get(js, "durableObjectId")) == "some-durable-object");

      // An internal error with a type has that type.
      auto typed = KJ_EXCEPTION(FAILED, "jsg-internal.DOMException(OperationError): secret");
      auto typedDescription = [&]() {
        KJ_EXPECT_LOG(ERROR, "jsg-internal.DOMException(OperationError): secret");
        return describeAsJsError(typed, kj::none);
      }();
      auto typedError = KJ_ASSERT_NONNULL(JsValue(
          exceptionToJs(js.v8Isolate, kj::mv(typed), {.alreadyDescribed = typedDescription}))
                                              .tryCast<JsObject>());
      KJ_EXPECT(kj::str(typedError.get(js, "name")) == "OperationError");
      KJ_EXPECT(kj::str(typedError.get(js, "message")) == typedDescription.message);

      // A description does not change the conversion of an error that is not internal.
      auto tunneled = KJ_EXCEPTION(FAILED, "jsg.TypeError: not internal");
      auto tunneledDescription = describeAsJsError(tunneled, kj::none);
      auto tunneledError = KJ_ASSERT_NONNULL(JsValue(
          exceptionToJs(js.v8Isolate, kj::mv(tunneled), {.alreadyDescribed = tunneledDescription}))
                                                 .tryCast<JsObject>());
      KJ_EXPECT(kj::str(tunneledError.get(js, "message")) == "not internal");
    });
  });
}

// ========================================================================================
// Regression test for asBytes() with resizable ArrayBuffers.
// resize(0) decommits pages even while the BackingStore shared_ptr is held, so asBytes()
// must deep-copy. Ref: AUTOVULN-CLOUDFLARE-WORKERD-73

struct AsBytesResizableContext: public ContextGlobalObject {
  // sumBytes(data, extra): sum all bytes in data, ignore extra.
  // The extra parameter (kj::Maybe<uint32_t>) exists so that a hostile valueOf() on
  // the second argument can resize the ArrayBuffer during argument unwrapping.
  double sumBytes(kj::Array<kj::byte> data, kj::Maybe<uint32_t> extra) {
    double sum = 0;
    for (auto b: data) sum += b;
    return sum;
  }
  JSG_RESOURCE_TYPE(AsBytesResizableContext) {
    JSG_METHOD(sumBytes);
  }
};
JSG_DECLARE_ISOLATE_TYPE(AsBytesResizableIsolate, AsBytesResizableContext);

KJ_TEST("asBytes copies data from resizable ArrayBuffer") {
  Evaluator<AsBytesResizableContext, AsBytesResizableIsolate> e(v8System);

  // Baseline: normal (non-resizable) buffer works.
  e.expectEval("var buf = new ArrayBuffer(4);\n"
               "var view = new Uint8Array(buf);\n"
               "view[0] = 1; view[1] = 2; view[2] = 3; view[3] = 4;\n"
               "sumBytes(view, 0);\n",
      "number", "10");

  // Resizable buffer, no hostile resize -- should work normally.
  e.expectEval("var rab = new ArrayBuffer(4, { maxByteLength: 4 });\n"
               "var u8 = new Uint8Array(rab);\n"
               "u8[0] = 10; u8[1] = 20; u8[2] = 30; u8[3] = 40;\n"
               "sumBytes(u8, 0);\n",
      "number", "100");

  // Resizable buffer with hostile valueOf that resizes to 0 during argument unwrap.
  // The evaluation order of arguments is indeterminate, so we can't predict whether
  // the data is captured before or after the resize. Either way, it must not crash.
  // We pass a Uint8Array (exercises the ArrayBufferView overload).
  e.expectEval("var rab2 = new ArrayBuffer(4, { maxByteLength: 4 });\n"
               "var u8b = new Uint8Array(rab2);\n"
               "u8b[0] = 10; u8b[1] = 20; u8b[2] = 30; u8b[3] = 40;\n"
               "typeof sumBytes(u8b, { valueOf() { rab2.resize(0); return 0; } });\n",
      "string", "number");

  // Same, but pass the raw ArrayBuffer (exercises the ArrayBuffer overload).
  e.expectEval("var rab3 = new ArrayBuffer(3, { maxByteLength: 3 });\n"
               "var v3 = new Uint8Array(rab3);\n"
               "v3[0] = 5; v3[1] = 15; v3[2] = 25;\n"
               "typeof sumBytes(rab3, { valueOf() { rab3.resize(0); return 0; } });\n",
      "string", "number");
}

}  // namespace
}  // namespace workerd::jsg::test
