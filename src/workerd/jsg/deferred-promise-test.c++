// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "deferred-promise.h"
#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;

struct DeferredPromiseContext: public jsg::Object, public jsg::ContextGlobal {
  // Test basic resolve/reject flow
  void testBasicResolve(jsg::Lock& js) {
    auto pair = newDeferredPromiseAndResolver<int>();
    KJ_EXPECT(pair.promise.isPending());
    KJ_EXPECT(!pair.promise.isResolved());
    KJ_EXPECT(!pair.promise.isRejected());

    pair.resolver.resolve(js, 42);
    KJ_EXPECT(!pair.promise.isPending());
    KJ_EXPECT(pair.promise.isResolved());
    KJ_EXPECT(!pair.promise.isRejected());
  }

  void testBasicReject(jsg::Lock& js) {
    auto pair = newDeferredPromiseAndResolver<int>();
    // Use v8StrIntern directly as the rejection value, not ThrowException
    pair.resolver.reject(js, v8StrIntern(js.v8Isolate, "error"));
    KJ_EXPECT(!pair.promise.isPending());
    KJ_EXPECT(!pair.promise.isResolved());
    KJ_EXPECT(pair.promise.isRejected());
  }

  // Test .then() with sync callbacks
  void testThenSync(jsg::Lock& js) {
    int result = 0;

    auto pair = newDeferredPromiseAndResolver<int>();
    pair.promise.then(js, [&result](jsg::Lock&, int value) { result = value * 2; });

    KJ_EXPECT(result == 0);
    pair.resolver.resolve(js, 21);
    KJ_EXPECT(result == 42);
  }

  // Test .then() with value transformation
  void testThenTransform(jsg::Lock& js) {
    kj::String result;

    auto pair = newDeferredPromiseAndResolver<int>();
    auto stringPromise = pair.promise.then(
        js, [](jsg::Lock&, int value) -> kj::String { return kj::str(value * 2); });

    stringPromise.then(js, [&result](jsg::Lock&, kj::String value) { result = kj::mv(value); });

    pair.resolver.resolve(js, 21);
    KJ_EXPECT(result == "42");
  }

  // Test already-resolved promise
  void testAlreadyResolved(jsg::Lock& js) {
    int result = 0;

    auto promise = DeferredPromise<int>::resolved(42);
    KJ_EXPECT(promise.isResolved());
    KJ_EXPECT(!promise.isPending());

    promise.then(js, [&result](jsg::Lock&, int value) { result = value; });
    KJ_EXPECT(result == 42);
  }

  // Test already-rejected promise
  void testAlreadyRejected(jsg::Lock& js) {
    bool errorCalled = false;

    auto promise =
        DeferredPromise<int>::rejected(js, JSG_KJ_EXCEPTION(FAILED, Error, "test error"));
    KJ_EXPECT(promise.isRejected());

    promise.then(js, [](jsg::Lock&, int) { KJ_FAIL_REQUIRE("should not be called"); },
        [&errorCalled](jsg::Lock& js, Value error) {
      // Just verify we got here - the error value is valid
      KJ_EXPECT(!error.getHandle(js).IsEmpty());
      errorCalled = true;
    });
    KJ_EXPECT(errorCalled);
  }

  // Test .catch_()
  void testCatch(jsg::Lock& js) {
    int result = 0;

    auto pair = newDeferredPromiseAndResolver<int>();
    auto recovered = pair.promise.catch_(js, [](jsg::Lock&, Value) -> int { return 123; });

    recovered.then(js, [&result](jsg::Lock&, int value) { result = value; });

    pair.resolver.reject(js, JSG_KJ_EXCEPTION(FAILED, Error, "error"));
    KJ_EXPECT(result == 123);
  }

  // Test void promise
  void testVoidPromise(jsg::Lock& js) {
    bool resolved = false;

    auto pair = newDeferredPromiseAndResolver<void>();
    pair.promise.then(js, [&resolved](jsg::Lock&) { resolved = true; });

    KJ_EXPECT(!resolved);
    pair.resolver.resolve(js);
    KJ_EXPECT(resolved);
  }

  // Test whenResolved() does not consume the promise
  void testWhenResolved(jsg::Lock& js) {
    int resolvedCount = 0;
    int thenCount = 0;

    auto pair = newDeferredPromiseAndResolver<int>();

    // whenResolved() should not consume
    pair.promise.whenResolved(js).then(js, [&resolvedCount](jsg::Lock&) { resolvedCount++; });

    // .then() should still work after whenResolved()
    pair.promise.then(js, [&thenCount](jsg::Lock&, int value) { thenCount = value; });

    pair.resolver.resolve(js, 42);
    KJ_EXPECT(resolvedCount == 1);
    KJ_EXPECT(thenCount == 42);
  }

  // Test conversion to jsg::Promise
  void testToJsPromise(jsg::Lock& js) {
    auto pair = newDeferredPromiseAndResolver<int>();
    auto jsPromise = pair.promise.toJsPromise(js);

    int result = 0;
    jsPromise.then(js, [&result](jsg::Lock&, int value) { result = value; });

    pair.resolver.resolve(js, 42);
    js.runMicrotasks();
    KJ_EXPECT(result == 42);
  }

  // Test promise chaining - DeferredPromise returning DeferredPromise
  void testDeferredChaining(jsg::Lock& js) {
    int result = 0;

    auto outerPair = newDeferredPromiseAndResolver<int>();
    auto innerPair = newDeferredPromiseAndResolver<int>();

    // The inner DeferredPromise should be automatically chained
    outerPair.promise
        .then(js, [&innerPair](jsg::Lock&, int) -> DeferredPromise<int> {
      return kj::mv(innerPair.promise);
    }).then(js, [&result](jsg::Lock&, int value) { result = value; });

    outerPair.resolver.resolve(js, 1);
    KJ_EXPECT(result == 0);  // Still waiting on inner

    innerPair.resolver.resolve(js, 42);
    KJ_EXPECT(result == 42);
  }

  // Test promise chaining - DeferredPromise returning jsg::Promise
  void testJsgPromiseChaining(jsg::Lock& js) {
    int result = 0;

    auto pair = newDeferredPromiseAndResolver<int>();

    pair.promise
        .then(js, [](jsg::Lock& js, int value) -> jsg::Promise<int> {
      return js.resolvedPromise(value * 2);
    }).then(js, [&result](jsg::Lock&, int value) { result = value; });

    pair.resolver.resolve(js, 21);
    js.runMicrotasks();  // jsg::Promise uses microtasks
    KJ_EXPECT(result == 42);
  }

  // Test error propagation through chain
  void testErrorPropagation(jsg::Lock& js) {
    kj::String errorMessage;

    auto pair = newDeferredPromiseAndResolver<int>();
    pair.promise.then(js, [](jsg::Lock&, int value) -> int { return value * 2; })
        .then(js, [](jsg::Lock&, int value) -> int {
      return value + 10;
    }).then(js, [](jsg::Lock&, int) {
      KJ_FAIL_REQUIRE("should not reach here");
    }, [&errorMessage](jsg::Lock& js, Value error) {
      errorMessage = kj::str(error.getHandle(js));
    });

    pair.resolver.reject(js, JSG_KJ_EXCEPTION(FAILED, Error, "original error"));
    KJ_EXPECT(errorMessage.contains("original error"));
  }

  // Test tryConsumeResolved optimization
  void testTryConsumeResolved(jsg::Lock& js) {
    {
      // Pending promise should return none
      auto pair = newDeferredPromiseAndResolver<int>();
      KJ_EXPECT(pair.promise.tryConsumeResolved() == kj::none);
    }

    {
      // Resolved promise should return value
      auto promise = DeferredPromise<int>::resolved(42);
      auto value = KJ_ASSERT_NONNULL(promise.tryConsumeResolved());
      KJ_EXPECT(value == 42);
    }
  }

  // Test multiple resolvers sharing state
  void testResolverAddRef(jsg::Lock& js) {
    auto pair = newDeferredPromiseAndResolver<int>();
    auto resolver2 = pair.resolver.addRef();

    int result = 0;
    pair.promise.then(js, [&result](jsg::Lock&, int value) { result = value; });

    // Either resolver can resolve
    resolver2.resolve(js, 42);
    KJ_EXPECT(result == 42);
  }

  // Test converting jsg::Promise to DeferredPromise
  void testFromJsPromise(jsg::Lock& js) {
    int result = 0;

    // Create a jsg::Promise
    auto [jsPromise, jsResolver] = js.newPromiseAndResolver<int>();

    // Convert to DeferredPromise and set up continuation chain
    auto deferred = DeferredPromise<int>::fromJsPromise(js, kj::mv(jsPromise));

    // These continuations will run synchronously when the JS promise resolves
    deferred.then(js, [](jsg::Lock&, int value) -> int {
      return value * 2;
    }).then(js, [&result](jsg::Lock&, int value) { result = value; });

    KJ_EXPECT(result == 0);  // Not yet resolved

    // Resolve the original JS promise
    jsResolver.resolve(js, 21);
    js.runMicrotasks();  // jsg::Promise uses microtasks

    KJ_EXPECT(result == 42);  // Continuations ran synchronously after microtask
  }

  // Test fromJsPromise with rejection
  void testFromJsPromiseReject(jsg::Lock& js) {
    bool errorCaught = false;

    auto [jsPromise, jsResolver] = js.newPromiseAndResolver<int>();
    auto deferred = DeferredPromise<int>::fromJsPromise(js, kj::mv(jsPromise));

    deferred.then(js, [](jsg::Lock&, int) { KJ_FAIL_REQUIRE("should not be called"); },
        [&errorCaught](jsg::Lock&, Value) { errorCaught = true; });

    jsResolver.reject(js, JSG_KJ_EXCEPTION(FAILED, Error, "test error"));
    js.runMicrotasks();

    KJ_EXPECT(errorCaught);
  }

  // Test that deep promise chains don't cause stack overflow (trampolining)
  void testDeepChain(jsg::Lock& js) {
    constexpr size_t CHAIN_DEPTH = 10000;

    int result = 0;

    // Build a very deep chain - this would overflow the stack without trampolining
    auto pair = newDeferredPromiseAndResolver<int>();
    auto promise = kj::mv(pair.promise);

    for (size_t i = 0; i < CHAIN_DEPTH; ++i) {
      promise = kj::mv(promise).then(js, [](jsg::Lock&, int v) { return v + 1; });
    }

    promise.then(js, [&result](jsg::Lock&, int v) { result = v; });

    // Resolve - if trampolining works, this won't overflow the stack
    pair.resolver.resolve(js, 0);

    // All callbacks should have run
    KJ_EXPECT(result == CHAIN_DEPTH);
  }

  // Test that FIFO order is maintained with trampolining
  void testTrampolineOrder(jsg::Lock& js) {
    kj::Vector<int> order;

    auto pair1 = newDeferredPromiseAndResolver<void>();
    auto pair2 = newDeferredPromiseAndResolver<void>();
    auto pair3 = newDeferredPromiseAndResolver<void>();

    pair1.promise.then(js, [&order](jsg::Lock&) { order.add(1); });
    pair2.promise.then(js, [&order](jsg::Lock&) { order.add(2); });
    pair3.promise.then(js, [&order](jsg::Lock&) { order.add(3); });

    // Resolve in order 1, 2, 3
    pair1.resolver.resolve(js);
    pair2.resolver.resolve(js);
    pair3.resolver.resolve(js);

    // Should maintain FIFO order
    KJ_ASSERT(order.size() == 3);
    KJ_EXPECT(order[0] == 1);
    KJ_EXPECT(order[1] == 2);
    KJ_EXPECT(order[2] == 3);
  }

  JSG_RESOURCE_TYPE(DeferredPromiseContext) {
    JSG_METHOD(testBasicResolve);
    JSG_METHOD(testBasicReject);
    JSG_METHOD(testThenSync);
    JSG_METHOD(testThenTransform);
    JSG_METHOD(testFromJsPromise);
    JSG_METHOD(testFromJsPromiseReject);
    JSG_METHOD(testAlreadyResolved);
    JSG_METHOD(testAlreadyRejected);
    JSG_METHOD(testCatch);
    JSG_METHOD(testVoidPromise);
    JSG_METHOD(testWhenResolved);
    JSG_METHOD(testToJsPromise);
    JSG_METHOD(testDeferredChaining);
    JSG_METHOD(testJsgPromiseChaining);
    JSG_METHOD(testErrorPropagation);
    JSG_METHOD(testTryConsumeResolved);
    JSG_METHOD(testResolverAddRef);
    JSG_METHOD(testDeepChain);
    JSG_METHOD(testTrampolineOrder);
  }
};

JSG_DECLARE_ISOLATE_TYPE(DeferredPromiseIsolate, DeferredPromiseContext);

KJ_TEST("DeferredPromise basic resolve") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testBasicResolve()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise basic reject") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testBasicReject()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise then sync") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testThenSync()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise then transform") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testThenTransform()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise already resolved") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testAlreadyResolved()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise already rejected") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testAlreadyRejected()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise catch") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testCatch()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise void") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testVoidPromise()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise whenResolved") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testWhenResolved()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise to jsg::Promise") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testToJsPromise()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise deferred chaining") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testDeferredChaining()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise jsg::Promise chaining") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testJsgPromiseChaining()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise error propagation") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testErrorPropagation()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise tryConsumeResolved") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testTryConsumeResolved()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise resolver addRef") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testResolverAddRef()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise from jsg::Promise") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testFromJsPromise()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise from jsg::Promise reject") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testFromJsPromiseReject()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise deep chain (trampolining)") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testDeepChain()", "undefined", "undefined");
}

KJ_TEST("DeferredPromise trampoline order") {
  Evaluator<DeferredPromiseContext, DeferredPromiseIsolate> e(v8System);
  e.expectEval("testTrampolineOrder()", "undefined", "undefined");
}

}  // namespace
}  // namespace workerd::jsg::test
