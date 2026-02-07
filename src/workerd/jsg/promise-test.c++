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
  Promise<kj::String> makePromise(jsg::Lock& js) {
    auto [p, r] = js.newPromiseAndResolver<int>();
    resolver = kj::mv(r);
    return p.then(js, [](jsg::Lock&, int i) { return i * 2; })
        .then(js, [](jsg::Lock& js, int i) {
      return js.resolvedPromise(i + 2);
    }).then(js, [](jsg::Lock& js, int i) { return kj::str(i); });
  }

  void resolvePromise(Lock& js, int i) {
    KJ_ASSERT_NONNULL(resolver).resolve(js, kj::mv(i));
  }

  void setResult(jsg::Lock& js, Promise<kj::String> promise) {
    // Throwing away the result of `.then()` doesn't cancel it!
    promise.then(js, [](jsg::Lock&, kj::String str) {
      promiseTestResult = str.parseAs<int>();
    }).then(js, [](jsg::Lock&) { promiseTestResult += 60000; });
  }

  void catchIt(jsg::Lock& js, Promise<int> promise) {
    promise
        .catch_(js,
            [](jsg::Lock& js, Value value) -> int {
      JSG_FAIL_REQUIRE(Error, kj::str(value.getHandle(js)));
    })
        .then(js, [](jsg::Lock& js, int i) {
      KJ_FAIL_REQUIRE("shouldn't get here");
      return kj::str("nope");
    }, [](jsg::Lock& js, Value value) {
      return kj::str(value.getHandle(js));
    }).then(js, [](jsg::Lock&, kj::String s) { catchTestResult = kj::mv(s); });
  }

  Promise<kj::String> makeRejected(jsg::Lock& js, jsg::Value exception) {
    return js.rejectedPromise<kj::String>(kj::mv(exception));
  }

  Promise<kj::String> makeRejectedKj(jsg::Lock& js) {
    return js.rejectedPromise<kj::String>(JSG_KJ_EXCEPTION(FAILED, TypeError, "bar"));
  }

  void testConsumeResolved(jsg::Lock& js) {
    auto [promise, resolver] = js.newPromiseAndResolver<int>();
    KJ_EXPECT(promise.tryConsumeResolved(js) == kj::none);
    resolver.resolve(js, 123);
    KJ_EXPECT(KJ_ASSERT_NONNULL(promise.tryConsumeResolved(js)) == 123);

    KJ_EXPECT(
        js.rejectedPromise<kj::String>(v8StrIntern(js.v8Isolate, "foo")).tryConsumeResolved(js) ==
        kj::none);
  }

  void whenResolved(jsg::Lock& js, jsg::Promise<int> promise) {
    // The returned promise should resolve to undefined.

    uint resolved = 0;

    auto handle = promise.whenResolved(js).then(js, [&resolved](jsg::Lock&) {
      resolved++;
    }).consumeHandle(js);

    promise.then(js, [&resolved](jsg::Lock&, int v) {
      KJ_ASSERT(v == 1);
      resolved++;
    });

    js.runMicrotasks();
    KJ_ASSERT(resolved == 2);

    {
      KJ_ASSERT(handle->State() == v8::Promise::PromiseState::kFulfilled);
      auto result = handle->Result();
      KJ_ASSERT(!result.IsEmpty());
      KJ_ASSERT(result->IsUndefined());
    }
  }

  int thenable(jsg::Lock& js, jsg::Promise<int> promise) {
    int result = 0;
    promise.then(js, [&result](jsg::Lock& js, int val) { result = val; });
    js.runMicrotasks();
    return result;
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

    JSG_METHOD(thenable);
  }

  kj::Maybe<Promise<int>::Resolver> resolver;
};
JSG_DECLARE_ISOLATE_TYPE(PromiseIsolate, PromiseContext);

KJ_TEST("jsg::Promise<T>") {
  Evaluator<PromiseContext, PromiseIsolate> e(v8System);

  e.expectEval("setResult(promise.then(i => i + 1 /* oops, i is a string */));\n"
               "resolvePromise(123)",
      "undefined", "undefined");

  KJ_EXPECT(promiseTestResult == 0);

  e.runMicrotasks();

  KJ_EXPECT(promiseTestResult == 62481);
}

KJ_TEST("jsg::Promise<T> exception catching") {
  Evaluator<PromiseContext, PromiseIsolate> e(v8System);

  {
    e.expectEval("catchIt(Promise.reject('foo'))", "undefined", "undefined");

    KJ_EXPECT(catchTestResult == nullptr);

    e.runMicrotasks();

    KJ_EXPECT(catchTestResult == "Error: foo");
    catchTestResult = nullptr;
  }

  {
    e.expectEval("catchIt(makeRejected(123))", "undefined", "undefined");

    KJ_EXPECT(catchTestResult == nullptr);

    e.runMicrotasks();

    KJ_EXPECT(catchTestResult == "Error: 123");
    catchTestResult = nullptr;
  }

  {
    e.expectEval("catchIt(makeRejectedKj())", "undefined", "undefined");

    KJ_EXPECT(catchTestResult == nullptr);

    e.runMicrotasks();

    KJ_EXPECT(catchTestResult == "Error: TypeError: bar");
    catchTestResult = nullptr;
  }

  { e.expectEval("testConsumeResolved()", "undefined", "undefined"); }
}

KJ_TEST("whenResolved") {
  Evaluator<PromiseContext, PromiseIsolate> e(v8System);

  e.expectEval("whenResolved(Promise.resolve(1))", "undefined", "undefined");
}

KJ_TEST("thenable") {
  static const auto config = JsgConfig{
    .unwrapCustomThenables = true,
  };

  struct ThenableConfig {
    operator const JsgConfig&() const {
      return config;
    }
  };

  Evaluator<PromiseContext, PromiseIsolate, ThenableConfig> e(v8System);

  e.expectEval("thenable({ then(res) { res(123) } })", "number", "123");
}

// =======================================================================================
// LazyPromise Tests

struct LazyPromiseContext: public jsg::Object, public jsg::ContextGlobal {
  // Store LazyPromise instances and their resolvers as members
  kj::Maybe<LazyPromiseResolverPair<int>> intPair;
  kj::Maybe<LazyPromiseResolverPair<void>> voidPair;

  void createIntPromise(jsg::Lock& js) {
    intPair = LazyPromiseResolverPair<int>();
  }

  void resolveIntPromise(jsg::Lock& js, int value) {
    KJ_ASSERT_NONNULL(intPair).resolver.resolve(js, kj::mv(value));
  }

  void rejectIntPromise(jsg::Lock& js, jsg::Value reason) {
    KJ_ASSERT_NONNULL(intPair).resolver.reject(js, kj::mv(reason));
  }

  MemoizedIdentity<Promise<int>>& getIntPromise(jsg::Lock& js) {
    return KJ_ASSERT_NONNULL(intPair).promise.getPromise(js);
  }

  void createVoidPromise(jsg::Lock& js) {
    voidPair = LazyPromiseResolverPair<void>();
  }

  void resolveVoidPromise(jsg::Lock& js) {
    KJ_ASSERT_NONNULL(voidPair).resolver.resolve(js);
  }

  void rejectVoidPromise(jsg::Lock& js, jsg::Value reason) {
    KJ_ASSERT_NONNULL(voidPair).resolver.reject(js, kj::mv(reason));
  }

  MemoizedIdentity<Promise<void>>& getVoidPromise(jsg::Lock& js) {
    return KJ_ASSERT_NONNULL(voidPair).promise.getPromise(js);
  }

  bool multipleGetPromiseReturnSame(jsg::Lock& js) {
    LazyPromiseResolverPair<int> pair;

    // Get the promise twice
    auto& memoized1 = pair.promise.getPromise(js);
    auto& memoized2 = pair.promise.getPromise(js);

    // They should be the same object
    return &memoized1 == &memoized2;
  }

  void testDoubleResolve(jsg::Lock& js) {
    // Double resolve is a no-op (matches jsg::Promise behavior)
    LazyPromiseResolverPair<int> pair;
    pair.resolver.resolve(js, 42);
    pair.resolver.resolve(js, 100);  // Should be ignored
  }

  void testResolveAfterReject(jsg::Lock& js) {
    // Resolve after reject is a no-op (matches jsg::Promise behavior)
    LazyPromiseResolverPair<int> pair;
    pair.resolver.reject(js, Value(js.v8Isolate, v8StrIntern(js.v8Isolate, "error")));
    pair.resolver.resolve(js, 100);  // Should be ignored
  }

  void testRejectAfterResolve(jsg::Lock& js) {
    // Reject after resolve is a no-op (matches jsg::Promise behavior)
    LazyPromiseResolverPair<int> pair;
    pair.resolver.resolve(js, 42);
    pair.resolver.reject(
        js, Value(js.v8Isolate, v8StrIntern(js.v8Isolate, "error")));  // Should be ignored
  }

  JSG_RESOURCE_TYPE(LazyPromiseContext) {
    JSG_METHOD(createIntPromise);
    JSG_METHOD(resolveIntPromise);
    JSG_METHOD(rejectIntPromise);
    JSG_METHOD(getIntPromise);
    JSG_METHOD(createVoidPromise);
    JSG_METHOD(resolveVoidPromise);
    JSG_METHOD(rejectVoidPromise);
    JSG_METHOD(getVoidPromise);
    JSG_METHOD(multipleGetPromiseReturnSame);
    JSG_METHOD(testDoubleResolve);
    JSG_METHOD(testResolveAfterReject);
    JSG_METHOD(testRejectAfterResolve);
  }

  void visitForGc(GcVisitor& visitor) {
    KJ_IF_SOME(pair, intPair) {
      visitor.visit(pair);
    }
    KJ_IF_SOME(pair, voidPair) {
      visitor.visit(pair);
    }
  }
};
JSG_DECLARE_ISOLATE_TYPE(LazyPromiseIsolate, LazyPromiseContext);

KJ_TEST("LazyPromise<int> resolve before getPromise") {
  Evaluator<LazyPromiseContext, LazyPromiseIsolate> e(v8System);
  e.expectEval("createIntPromise();"
               "resolveIntPromise(42);"
               "getIntPromise().then(v => v)",
      "object", "[object Promise]");
}

KJ_TEST("LazyPromise<int> resolve after getPromise") {
  Evaluator<LazyPromiseContext, LazyPromiseIsolate> e(v8System);
  e.expectEval("createIntPromise();"
               "let p = getIntPromise();"
               "resolveIntPromise(123);"
               "p.then(v => v)",
      "object", "[object Promise]");
}

KJ_TEST("LazyPromise<int> reject before getPromise") {
  Evaluator<LazyPromiseContext, LazyPromiseIsolate> e(v8System);
  e.expectEval("createIntPromise();"
               "rejectIntPromise('test error');"
               "getIntPromise().catch(e => 'caught: ' + e)",
      "object", "[object Promise]");
}

KJ_TEST("LazyPromise<int> reject after getPromise") {
  Evaluator<LazyPromiseContext, LazyPromiseIsolate> e(v8System);
  e.expectEval("createIntPromise();"
               "let p = getIntPromise();"
               "rejectIntPromise('test error');"
               "p.catch(e => 'caught: ' + e)",
      "object", "[object Promise]");
}

KJ_TEST("LazyPromise<int> multiple getPromise calls return same object") {
  Evaluator<LazyPromiseContext, LazyPromiseIsolate> e(v8System);
  e.expectEval("multipleGetPromiseReturnSame()", "boolean", "true");
}

KJ_TEST("LazyPromise<void> resolve before getPromise") {
  Evaluator<LazyPromiseContext, LazyPromiseIsolate> e(v8System);
  e.expectEval("createVoidPromise();"
               "resolveVoidPromise();"
               "getVoidPromise().then(() => 'resolved')",
      "object", "[object Promise]");
}

KJ_TEST("LazyPromise<void> resolve after getPromise") {
  Evaluator<LazyPromiseContext, LazyPromiseIsolate> e(v8System);
  e.expectEval("createVoidPromise();"
               "let p = getVoidPromise();"
               "resolveVoidPromise();"
               "p.then(() => 'resolved')",
      "object", "[object Promise]");
}

KJ_TEST("LazyPromise double resolve is no-op") {
  Evaluator<LazyPromiseContext, LazyPromiseIsolate> e(v8System);
  e.expectEval("testDoubleResolve()", "undefined", "undefined");
}

KJ_TEST("LazyPromise resolve after reject is no-op") {
  Evaluator<LazyPromiseContext, LazyPromiseIsolate> e(v8System);
  e.expectEval("testResolveAfterReject()", "undefined", "undefined");
}

KJ_TEST("LazyPromise reject after resolve is no-op") {
  Evaluator<LazyPromiseContext, LazyPromiseIsolate> e(v8System);
  e.expectEval("testRejectAfterResolve()", "undefined", "undefined");
}

}  // namespace
}  // namespace workerd::jsg::test
