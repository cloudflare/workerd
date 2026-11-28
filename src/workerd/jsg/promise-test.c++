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

    // Value-initialized to 0.
    kj::Pin<uint> resolved;

    auto handle = promise.whenResolved(js)
                      .then(js, [weakResolved = resolved.addWeak()](jsg::Lock&) {
      KJ_REQUIRE_NONNULL(weakResolved.tryGet(), "test returned before promise resolved")++;
    }).consumeHandle(js);

    promise.then(js, [&resolved](jsg::Lock&, int v) {
      KJ_ASSERT(v == 1);
      (*resolved)++;
    });

    js.runMicrotasks();
    KJ_ASSERT(*resolved == 2);

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

  void prototypePolution(jsg::Lock& js) {
    js.resolvedPromise(js.num(1));
  }

  // Resolves promises with C++ values, which V8 sees as opaque wrappers: through a resolver, as
  // an already resolved promise, and as the result of a C++ continuation.
  void resolveWithCppValues(jsg::Lock& js) {
    auto [promise, resolver] = js.newPromiseAndResolver<kj::String>();
    resolver.resolve(js, kj::str("resolver"));
    promise.then(js, [](jsg::Lock&, kj::String value) { return kj::mv(value); });
    js.resolvedPromise(kj::str("resolved"));
    js.runMicrotasks();
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
    JSG_METHOD(prototypePolution);
    JSG_METHOD(resolveWithCppValues);
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

KJ_TEST("prototype polution") {
  Evaluator<PromiseContext, PromiseIsolate> e(v8System);

  e.expectEval(R"A(
    let m = false;
    Object.defineProperty(Object.prototype, 'then', {
      configurable: true,
      get() {
        m = true;
        return undefined;
      },
    });
    prototypePolution();
    m;
  )A",
      "boolean", "false");
}

KJ_TEST("a then on Object.prototype isn't called for a promise resolved with a C++ value") {
  Evaluator<PromiseContext, PromiseIsolate> e(v8System);

  // Resolving a promise with an object looks up its `then`, so the opaque wrapper that carries a
  // C++ value must not inherit Object.prototype's.
  e.expectEval(R"A(
    let called = false;
    Object.defineProperty(Object.prototype, 'then', {
      configurable: true,
      get() {
        called = true;
        return undefined;
      },
    });
    resolveWithCppValues();
    called;
  )A",
      "boolean", "false");
}

// Makes a PromiseContextScope's tag, counting how often it is asked to.
struct DeferredPromiseTag {
  uint makeCount = 0;
  kj::Maybe<JsRef<JsObject>> tag;

  static v8::Local<v8::Object> make(v8::Isolate* isolate, void* data) {
    auto& self = *static_cast<DeferredPromiseTag*>(data);
    auto& js = Lock::from(isolate);
    ++self.makeCount;
    auto tag = js.obj();
    self.tag = JsRef(js, tag);
    return tag;
  }
};

uint crossContextCount = 0;
kj::Maybe<JsRef<JsObject>> crossContextTag;

KJ_TEST("a deferred promise context tag is made by the scope's first promise") {
  Evaluator<PromiseContext, PromiseIsolate> e(v8System);
  e.run([](auto& lock) {
    Lock& js = lock;
    auto eval = [&](kj::StringPtr code) {
      auto script = check(v8::Script::Compile(js.v8Context(), v8Str(js.v8Isolate, code)));
      check(script->Run(js.v8Context()));
    };

    js.v8Isolate->SetPromiseCrossContextCallback(
        [](v8::Local<v8::Context> context, v8::Local<v8::Promise> promise,
            v8::Local<v8::Object> tag) -> v8::MaybeLocal<v8::Promise> {
      auto& js = Lock::current();
      ++crossContextCount;
      crossContextTag = JsRef(js, JsObject(tag));
      return promise;
    });
    KJ_DEFER(js.v8Isolate->SetPromiseCrossContextCallback(nullptr));

    DeferredPromiseTag first;
    {
      v8::Isolate::PromiseContextScope scope(js.v8Isolate, &DeferredPromiseTag::make, &first);
      eval("globalThis.x = 1 + 1");
      KJ_EXPECT(first.makeCount == 0);

      // JavaScript promises get their tag from the builtins that make them.
      eval("globalThis.p = new Promise(() => {}); Promise.resolve(1); (async () => {})()");
      KJ_EXPECT(first.makeCount == 1);
      eval("p.then(() => {})");
      KJ_EXPECT(crossContextCount == 0);
    }

    DeferredPromiseTag second;
    {
      // A promise from another scope is from another context, both before this scope has a tag
      // and after it has made one.
      v8::Isolate::PromiseContextScope scope(js.v8Isolate, &DeferredPromiseTag::make, &second);
      eval("p.then(() => {})");
      KJ_EXPECT(crossContextCount == 1);
      JsValue seenTag = KJ_ASSERT_NONNULL(crossContextTag).getHandle(js);
      KJ_EXPECT(seenTag.strictEquals(KJ_ASSERT_NONNULL(first.tag).getHandle(js)));
      eval("p.then(() => {})");
      KJ_EXPECT(crossContextCount == 2);
    }

    DeferredPromiseTag third;
    {
      // Promises made through the API get their tag from Factory::NewJSPromiseWithoutHook.
      v8::Isolate::PromiseContextScope scope(js.v8Isolate, &DeferredPromiseTag::make, &third);
      check(v8::Promise::Resolver::New(js.v8Context()));
      check(v8::Promise::Resolver::New(js.v8Context()));
      KJ_EXPECT(third.makeCount == 1);
    }

    DeferredPromiseTag fourth;
    {
      // A subclass's promise is allocated by the Promise constructor rather than by
      // InnerNewJSPromise.
      v8::Isolate::PromiseContextScope scope(js.v8Isolate, &DeferredPromiseTag::make, &fourth);
      eval("globalThis.sub = new (class extends Promise {})(() => {})");
      KJ_EXPECT(fourth.makeCount == 1);
      eval("sub.then(() => {})");
      KJ_EXPECT(crossContextCount == 2);
    }

    {
      // A scope that starts with the first scope's tag treats its promises as its own.
      v8::Isolate::PromiseContextScope scope(
          js.v8Isolate, KJ_ASSERT_NONNULL(first.tag).getHandle(js));
      eval("p.then(() => {})");
      KJ_EXPECT(crossContextCount == 2);
    }
    crossContextTag = kj::none;
  });
}

}  // namespace
}  // namespace workerd::jsg::test
