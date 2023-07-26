// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"


namespace workerd::jsg::test {
namespace {

V8System v8System;

int promiseTestResult = 0;
kj::String catchTestResult;

struct PromiseContext: public jsg::Object, public jsg::ContextGlobal {
  Promise<kj::String> makePromise(Lock& js) {
    auto [ p, r ] = js.newPromiseAndResolver<int>();
    resolver = kj::mv(r);
    return p.then([](int i) { return i * 2; })
        .then(js, [](Lock& js, int i) { return js.resolvedPromise(i + 2); })
        .then([](int i) { return kj::str(i); });
  }

  void resolvePromise(int i) {
    KJ_ASSERT_NONNULL(resolver).resolve(kj::mv(i));
  }

  void setResult(Promise<kj::String> promise) {
    // Throwing away the result of `.then()` dosen't cancel it!
    promise.then([](kj::String str) {
      promiseTestResult = str.parseAs<int>();
    }).then([]() {
      promiseTestResult += 60000;
    });
  }

  void catchIt(Lock& js, Promise<int> promise) {
    promise.catch_(js, [](Lock& js, Value value) -> int {
      JSG_FAIL_REQUIRE(Error, kj::str(value.getHandle(js.v8Isolate)));
    }).then(js, [](Lock& js, int i) {
      KJ_FAIL_REQUIRE("shouldn't get here");
      return kj::str("nope");
    }, [](Lock& js, Value value) {
      return kj::str(value.getHandle(js.v8Isolate));
    }).then([](kj::String s) {
      catchTestResult = kj::mv(s);
    });
  }

  Promise<kj::String> makeRejected(jsg::Value exception, v8::Isolate* isolate) {
    return rejectedPromise<kj::String>(isolate, kj::mv(exception));
  }

  Promise<kj::String> makeRejectedKj(Lock& js) {
    return js.rejectedPromise<kj::String>(JSG_KJ_EXCEPTION(FAILED, TypeError, "bar"));
  }

  void testConsumeResolved(Lock& js) {
    auto [ promise, resolver ] = js.newPromiseAndResolver<int>();
    KJ_EXPECT(promise.tryConsumeResolved() == nullptr);
    resolver.resolve(123);
    KJ_EXPECT(KJ_ASSERT_NONNULL(promise.tryConsumeResolved()) == 123);

    KJ_EXPECT(js.rejectedPromise<kj::String>(v8Str(js.v8Isolate, "foo"))
        .tryConsumeResolved() == nullptr);
  }

  void whenResolved(jsg::Promise<int> promise, v8::Isolate* isolate) {
    // The returned promise should resolve to undefined.

    uint resolved = 0;

    auto handle = promise.whenResolved().then([&resolved]() {
      resolved++;
    }).consumeHandle(isolate);

    promise.then([&resolved](int v) {
      KJ_ASSERT(v == 1);
      resolved++;
    });

    isolate->PerformMicrotaskCheckpoint();
    KJ_ASSERT(resolved == 2);

    {
      KJ_ASSERT(handle->State() == v8::Promise::PromiseState::kFulfilled);
      auto result = handle->Result();
      KJ_ASSERT(!result.IsEmpty());
      KJ_ASSERT(result->IsUndefined());
    }
  }

  JSG_RESOURCE_TYPE(PromiseContext) {
    JSG_READONLY_PROTOTYPE_PROPERTY(promise, makePromise);
    JSG_METHOD(resolvePromise);
    JSG_METHOD(setResult);
    JSG_METHOD(catchIt);

    JSG_METHOD(makeRejected);
    JSG_METHOD(makeRejectedKj);

    JSG_METHOD(testConsumeResolved);
    JSG_METHOD(whenResolved);
  }

  kj::Maybe<Promise<int>::Resolver> resolver;
};
JSG_DECLARE_ISOLATE_TYPE(PromiseIsolate, PromiseContext);

KJ_TEST("jsg::Promise<T>") {
  Evaluator<PromiseContext, PromiseIsolate> e(v8System);

  e.expectEval(
      "setResult(promise.then(i => i + 1 /* oops, i is a string */));\n"
      "resolvePromise(123)", "undefined", "undefined");

  KJ_EXPECT(promiseTestResult == 0);

  e.runMicrotasks();

  KJ_EXPECT(promiseTestResult == 62481);
}

KJ_TEST("jsg::Promise<T> exception catching") {
  Evaluator<PromiseContext, PromiseIsolate> e(v8System);

  {
    e.expectEval(
        "catchIt(Promise.reject('foo'))", "undefined", "undefined");

    KJ_EXPECT(catchTestResult == nullptr);

    e.runMicrotasks();

    KJ_EXPECT(catchTestResult == "Error: foo");
    catchTestResult = nullptr;
  }

  {
    e.expectEval(
        "catchIt(makeRejected(123))", "undefined", "undefined");

    KJ_EXPECT(catchTestResult == nullptr);

    e.runMicrotasks();

    KJ_EXPECT(catchTestResult == "Error: 123");
    catchTestResult = nullptr;
  }

  {
    e.expectEval(
        "catchIt(makeRejectedKj())", "undefined", "undefined");

    KJ_EXPECT(catchTestResult == nullptr);

    e.runMicrotasks();

    KJ_EXPECT(catchTestResult == "Error: TypeError: bar");
    catchTestResult = nullptr;
  }

  {
    e.expectEval("testConsumeResolved()", "undefined", "undefined");
  }
}

KJ_TEST("whenResolved") {
  Evaluator<PromiseContext, PromiseIsolate> e(v8System);

  e.expectEval("whenResolved(Promise.resolve(1))", "undefined", "undefined");
}

}  // namespace
}  // namespace workerd::jsg::test
