// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "dom-exception.h"
#include "jsg-test.h"
#include "ser.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;

struct DOMExceptionContext: public Object, public ContextGlobal {
  JSG_RESOURCE_TYPE(DOMExceptionContext) {
    JSG_NESTED_TYPE(DOMException);
  }
};
JSG_DECLARE_ISOLATE_TYPE(DOMExceptionIsolate, DOMExceptionContext);

KJ_TEST("DOMException's prototype is ErrorPrototype") {
  Evaluator<DOMExceptionContext, DOMExceptionIsolate> e(v8System);
  e.expectEval(
      "Object.getPrototypeOf(DOMException.prototype) === Error.prototype", "boolean", "true");
}

KJ_TEST("DOMException has a stack property") {
  Evaluator<DOMExceptionContext, DOMExceptionIsolate> e(v8System);
  e.expectEval("function throwError() { throw new DOMException('test error') }\n"
               "try { throwError() } catch (e) { e.stack }",
      "string",
      "Error: test error\n"
      "    at throwError (<anonymous>:1:31)\n"
      "    at <anonymous>:2:7");
}

KJ_TEST("DOMException has legacy code constants") {
  Evaluator<DOMExceptionContext, DOMExceptionIsolate> e(v8System);
  // Test a subset of error codes that commonly appear in web APIs.
  e.expectEval("DOMException.INDEX_SIZE_ERR === 1", "boolean", "true");
  e.expectEval("DOMException.NOT_SUPPORTED_ERR === 9", "boolean", "true");
  e.expectEval("DOMException.SYNTAX_ERR === 12", "boolean", "true");
  e.expectEval("DOMException.INVALID_ACCESS_ERR === 15", "boolean", "true");
  e.expectEval("DOMException.TYPE_MISMATCH_ERR === 17", "boolean", "true");
  e.expectEval("DOMException.QUOTA_EXCEEDED_ERR === 22", "boolean", "true");
  e.expectEval("DOMException.DATA_CLONE_ERR === 25", "boolean", "true");
}

KJ_TEST("jsg::Lock domException") {
  Evaluator<DOMExceptionContext, DOMExceptionIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](DOMExceptionIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock,
        lock.template newContext<DOMExceptionContext>().getHandle(lock.v8Isolate),
        [&](jsg::Lock& js) {
      Ref<DOMException> exception = js.domException(kj::str("foo"), kj::str("bar"));

      // The DOMException object should have the JS handle created already.
      auto handle = JsObject(KJ_ASSERT_NONNULL(exception.tryGetHandle(js)));

      // And it will have the stack.
      KJ_ASSERT(handle.has(js, "stack"_kj));

      KJ_ASSERT(exception->getName() == "foo"_kj);
      KJ_ASSERT(exception->getMessage() == "bar"_kj);
    });
  });
}

KJ_TEST("newDOMException") {
  Evaluator<DOMExceptionContext, DOMExceptionIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](DOMExceptionIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock,
        lock.template newContext<DOMExceptionContext>().getHandle(lock.v8Isolate),
        [&](jsg::Lock& js) {
      v8::Local<v8::Object> handle;
      KJ_ASSERT(
          newDOMException(js.v8Isolate, "AbortError"_kj, js.str("aborted"_kj)).ToLocal(&handle));
      auto exception = JsObject(handle);
      auto domException =
          KJ_ASSERT_NONNULL(js.global().get(js, "DOMException"_kj).tryCast<JsObject>());
      KJ_ASSERT(exception.getPrototype(js) == domException.get(js, "prototype"_kj));
      KJ_ASSERT(exception.get(js, "name"_kj).toString(js) == "AbortError"_kj);
      KJ_ASSERT(exception.get(js, "message"_kj).toString(js) == "aborted"_kj);
      KJ_ASSERT(exception.get(js, "code"_kj).toString(js) == "20"_kj);
      KJ_ASSERT(exception.has(js, "stack"_kj));
    });
  });
}

KJ_TEST("newDOMException while terminating creates nothing and keeps the termination") {
  Evaluator<DOMExceptionContext, DOMExceptionIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](DOMExceptionIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock,
        lock.template newContext<DOMExceptionContext>().getHandle(lock.v8Isolate),
        [&](jsg::Lock& js) {
      // NOLINTNEXTLINE(custom-raw-v8-trycatch): the test only checks that the termination stays.
      v8::TryCatch tryCatch(js.v8Isolate);
      try {
        js.terminateExecutionNow();
      } catch (JsExceptionThrown&) {}
      KJ_ASSERT(js.v8Isolate->IsExecutionTerminating());
      auto message = js.str("aborted"_kj);
      KJ_ASSERT(newDOMException(js.v8Isolate, "AbortError"_kj, message).IsEmpty());
      KJ_ASSERT(js.v8Isolate->IsExecutionTerminating());
      js.v8Isolate->CancelTerminateExecution();
    });
  });
}

KJ_TEST("newDOMException while an exception is pending creates nothing and keeps it") {
  Evaluator<DOMExceptionContext, DOMExceptionIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](DOMExceptionIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock,
        lock.template newContext<DOMExceptionContext>().getHandle(lock.v8Isolate),
        [&](jsg::Lock& js) {
      // NOLINTNEXTLINE(custom-raw-v8-trycatch): the test only checks which exception is caught.
      v8::TryCatch tryCatch(js.v8Isolate);
      v8::Local<v8::Value> first = js.str("first"_kj);
      js.v8Isolate->ThrowException(first);
      auto message = js.str("aborted"_kj);
      KJ_ASSERT(newDOMException(js.v8Isolate, "AbortError"_kj, message).IsEmpty());
      KJ_ASSERT(tryCatch.HasCaught());
      KJ_ASSERT(tryCatch.Exception()->StrictEquals(first));
    });
  });
}

KJ_TEST("DOMException deserializes the legacy tag") {
  // The legacy format: the name, then an object holding the message and the stack.
  // clang-format off
  kj::byte payload[] = {
    0xff, 0x0f,                                    // V8 header, version 15
    0x5c, DOMException::SERIALIZATION_TAG,         // host object, tag
    10, 'A', 'b', 'o', 'r', 't', 'E', 'r', 'r', 'o', 'r',
    0x6f,                                          // begin object
    0x22, 7, 'm', 'e', 's', 's', 'a', 'g', 'e',
    0x22, 4, 'b', 'o', 'o', 'm',
    0x22, 5, 's', 't', 'a', 'c', 'k',
    0x22, 16, 'A', 'b', 'o', 'r', 't', 'E', 'r', 'r', 'o', 'r', ':', ' ', 'b', 'o', 'o', 'm',
    0x7b, 2,                                       // end object, two properties
  };
  // clang-format on

  Evaluator<DOMExceptionContext, DOMExceptionIsolate> e(v8System);
  e.getIsolate().runInLockScope([&](DOMExceptionIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock,
        lock.template newContext<DOMExceptionContext>().getHandle(lock.v8Isolate),
        [&](jsg::Lock& js) {
      Deserializer deser(js, payload);
      auto exception = KJ_ASSERT_NONNULL(deser.readValue(js).tryCast<JsObject>());
      KJ_EXPECT(exception.get(js, "name"_kj).toString(js) == "AbortError"_kj);
      KJ_EXPECT(exception.get(js, "message"_kj).toString(js) == "boom"_kj);
      KJ_EXPECT(exception.get(js, "code"_kj).toString(js) == "20"_kj);
      KJ_EXPECT(exception.get(js, "stack"_kj).toString(js) == "AbortError: boom"_kj);
    });
  });
}

}  // namespace
}  // namespace workerd::jsg::test
