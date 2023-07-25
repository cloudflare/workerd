// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <kj/test.h>

#include <workerd/io/promise-wrapper.h>
#include <workerd/jsg/jsg.h>
#include <workerd/jsg/setup.h>
#include <workerd/jsg/jsg-test.h>

namespace workerd::jsg::test {  // workerd
namespace {

jsg::V8System v8System;

struct CaptureThrowContext: public jsg::Object, public ContextGlobal {
  kj::Promise<int> test1() {
    JSG_FAIL_REQUIRE(TypeError, "boom");
  }

  kj::Promise<void> test2() {
    JSG_FAIL_REQUIRE(TypeError, "boom");
  }

  int test3() {
    JSG_FAIL_REQUIRE(TypeError, "boom");
  }

  kj::Promise<void> test4(const v8::FunctionCallbackInfo<v8::Value>& args) {
    JSG_FAIL_REQUIRE(TypeError, "boom");
  }

  int test5(const v8::FunctionCallbackInfo<v8::Value>& args) {
    JSG_FAIL_REQUIRE(TypeError, "boom");
  }

  v8::Local<v8::Promise> test6() {
    JSG_FAIL_REQUIRE(TypeError, "boom");
  }

  v8::Local<v8::Promise> test7(const v8::FunctionCallbackInfo<v8::Value>& args) {
    JSG_FAIL_REQUIRE(TypeError, "boom");
  }

  template <typename T>
  v8::Local<v8::Value> testT(jsg::Lock& js, const jsg::TypeHandler<Function<T()>>& handler) {
    return handler.wrap(js, [](Lock&) -> T {
      JSG_FAIL_REQUIRE(TypeError, "boom");
    });
  }

  static kj::Promise<void> staticTest1() {
    JSG_FAIL_REQUIRE(TypeError, "boom");
  }

  static kj::Promise<void> staticTest2(const v8::FunctionCallbackInfo<v8::Value>&) {
    JSG_FAIL_REQUIRE(TypeError, "boom");
  }

  static kj::Promise<void> staticTest3(v8::Isolate* isolate) {
    // Tests that JsExceptionThrown is handled properly.
    jsg::throwTypeError(isolate, "boom"_kj);
  }

  kj::Promise<void> getTest() {
    JSG_FAIL_REQUIRE(TypeError, "boom");
  }

  JSG_RESOURCE_TYPE(CaptureThrowContext) {
    JSG_METHOD(test1);
    JSG_METHOD(test2);
    JSG_METHOD(test3);
    JSG_METHOD(test4);
    JSG_METHOD(test5);
    JSG_METHOD(test6);
    JSG_METHOD(test7);
    JSG_READONLY_PROTOTYPE_PROPERTY(test8, template testT<kj::Promise<void>>);
    JSG_STATIC_METHOD(staticTest1);
    JSG_STATIC_METHOD(staticTest2);
    JSG_STATIC_METHOD(staticTest3);
    JSG_READONLY_PROTOTYPE_PROPERTY(test, getTest);
  }
};
JSG_DECLARE_ISOLATE_TYPE(
    CaptureThrowIsolate,
    CaptureThrowContext,
    jsg::TypeWrapperExtension<workerd::PromiseWrapper>);

KJ_TEST("Async functions capture sync errors with flag") {
  Evaluator<CaptureThrowContext, CaptureThrowIsolate> e(v8System);
  e.setCaptureThrowsAsRejections(true);
  e.expectEval("test1()", "object", "[object Promise]");
  e.expectEval("test2()", "object", "[object Promise]");
  e.expectEval("test3()", "throws", "TypeError: boom");
  e.expectEval("test4()", "object", "[object Promise]");
  e.expectEval("test5()", "throws", "TypeError: boom");
  e.expectEval("test6()", "object", "[object Promise]");
  e.expectEval("test7()", "object", "[object Promise]");
  e.expectEval("test8()", "object", "[object Promise]");
  e.expectEval("CaptureThrowContext.staticTest1()", "object", "[object Promise]");
  e.expectEval("CaptureThrowContext.staticTest2()", "object", "[object Promise]");
  e.expectEval("CaptureThrowContext.staticTest3()", "object", "[object Promise]");
  e.expectEval("test", "object", "[object Promise]");
}

KJ_TEST("Async functions do not capture sync errors without flag") {
  Evaluator<CaptureThrowContext, CaptureThrowIsolate> e(v8System);
  e.setCaptureThrowsAsRejections(false);
  e.expectEval("test1()", "throws", "TypeError: boom");
  e.expectEval("test2()", "throws", "TypeError: boom");
  e.expectEval("test3()", "throws", "TypeError: boom");
  e.expectEval("test4()", "throws", "TypeError: boom");
  e.expectEval("test5()", "throws", "TypeError: boom");
  e.expectEval("test6()", "throws", "TypeError: boom");
  e.expectEval("test7()", "throws", "TypeError: boom");
  e.expectEval("test8()", "throws", "TypeError: boom");
  e.expectEval("CaptureThrowContext.staticTest1()", "throws", "TypeError: boom");
  e.expectEval("CaptureThrowContext.staticTest2()", "throws", "TypeError: boom");
  e.expectEval("CaptureThrowContext.staticTest3()", "throws", "TypeError: boom");
  e.expectEval("test", "throws", "TypeError: boom");
}

}  // namespace
}  // namespace workerd::jsg::test
