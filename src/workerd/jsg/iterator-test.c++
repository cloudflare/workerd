// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System({"--expose-gc"_kj});

struct GeneratorContext: public Object, public ContextGlobal {
  WeakJsRef<JsObject> trackedAsyncGenerator = nullptr;

  void trackAsyncGenerator(Lock& js, JsObject generator) {
    trackedAsyncGenerator = generator.getWeakRef(js);
  }

  void startPendingAsyncGenerator(Lock& js, JsObject object, AsyncGenerator<kj::String> generator) {
    trackedAsyncGenerator = object.getWeakRef(js);
    generator.next(js);
  }

  bool trackedAsyncGeneratorIsAlive() {
    return trackedAsyncGenerator.isAlive();
  }

  kj::Array<kj::String> generatorTest(Lock& js, Generator<kj::String> generator) {
    kj::Vector<kj::String> items;
    while (true) {
      KJ_IF_SOME(item, generator.next(js)) {
        items.add(kj::mv(item));
      } else {
        break;
      }
    }
    return items.releaseAsArray();
  }

  uint generatorErrorTest(Lock& js, Generator<kj::String> generator) {
    uint count = 0;

    // First call to next() should succeed and return "a"
    KJ_IF_SOME(val, generator.next(js)) {
      KJ_ASSERT(val == "a");
      ++count;
    }

    // Second call - we'll throw an error, which should trigger the generator's
    // throw handler (the catch block), which yields "c"
    KJ_IF_SOME(val, generator.throw_(js, js.v8Ref<v8::Value>(js.str("boom"_kj)))) {
      KJ_ASSERT(val == "c");
      ++count;
    }

    return count;
  }

  uint sequenceOfSequenceTest(Lock& js, Sequence<Sequence<kj::String>> sequence) {
    uint count = 0;
    for (auto& mem: sequence) {
      count += mem.size();
    }
    return count;
  }

  uint sequenceOfBufferSourceTest(Lock& js, Sequence<JsBufferSource> sequence) {
    uint size = 0;
    for (auto& buffer: sequence) {
      size += buffer.size();
    }
    return size;
  }

  uint asyncGeneratorTest(Lock& js, AsyncGenerator<kj::String> generator) {
    // Value-initialized to 0.
    kj::Pin<uint> count;
    bool finished = false;

    // Get first item
    generator.next(js)
        .then(js, [&count, &generator](auto& js, auto value) {
      KJ_ASSERT(KJ_ASSERT_NONNULL(value) == "a");
      ++*count;

      // After getting first item, call return_() to terminate early
      return generator.return_(js, kj::str("foo"))
          .then(js, [weakCount = count.addWeak()](auto& js, auto value) {
        // return_() should give us back "foo" and mark as done
        KJ_ASSERT(KJ_ASSERT_NONNULL(value) == "foo");
        ++KJ_REQUIRE_NONNULL(weakCount.tryGet(), "test returned before return_() resolved");
        return js.resolvedPromise();
      });
    }).then(js, [&finished](auto& js) {
      finished = true;
      return js.resolvedPromise();
    });

    js.runMicrotasks();

    KJ_ASSERT(finished);
    KJ_ASSERT(*count == 2);

    return *count;
  }

  uint asyncGeneratorErrorTest(Lock& js, AsyncGenerator<kj::String> generator) {
    // Value-initialized to 0.
    kj::Pin<uint> count;
    bool finished = false;

    // First call to next() should succeed and return "a"
    generator.next(js)
        .then(js, [&count, &generator](auto& js, auto value) {
      KJ_ASSERT(KJ_ASSERT_NONNULL(value) == "a");
      ++*count;

      // Second call - throw an error, which should trigger the generator's
      // throw handler (the catch block), which yields "c"
      return generator.throw_(js, js.template v8Ref<v8::Value>(js.str("boom"_kj)))
          .then(js, [weakCount = count.addWeak()](auto& js, auto value) {
        KJ_ASSERT(KJ_ASSERT_NONNULL(value) == "c");
        ++KJ_REQUIRE_NONNULL(weakCount.tryGet(), "test returned before throw_() resolved");
        return js.resolvedPromise();
      });
    }).then(js, [&finished](auto& js) {
      finished = true;
      return js.resolvedPromise();
    });

    js.runMicrotasks();

    KJ_ASSERT(finished);
    KJ_ASSERT(*count == 2);

    return *count;
  }

  void manualAsyncGeneratorTest(Lock& js, AsyncGenerator<kj::String> generator) {
    uint calls = 0;
    generator.next(js).then(js, [&calls](auto& js, auto value) {
      calls++;
      KJ_ASSERT(KJ_ASSERT_NONNULL(value) == "a");
      return js.resolvedPromise();
    });

    generator.next(js).then(js, [&calls](auto& js, auto value) {
      calls++;
      KJ_ASSERT(KJ_ASSERT_NONNULL(value) == "b");
      return js.resolvedPromise();
    });

    generator.next(js).then(js, [&calls](auto& js, auto value) {
      calls++;
      KJ_ASSERT(value == kj::none);
    });

    js.runMicrotasks();
    KJ_ASSERT(calls == 3);
  }

  void manualAsyncGeneratorOutOfOrderTest(
      Lock& js, AsyncGenerator<kj::String> generator, Function<void()> settle) {
    uint settled = 0;
    generator.next(js).then(js, [&settled](auto& js, auto value) {
      KJ_ASSERT(KJ_ASSERT_NONNULL(value) == "a");
      ++settled;
    });
    generator.next(js).then(js, [&settled](auto& js, auto value) {
      KJ_ASSERT(value == kj::none);
      ++settled;
    });

    settle(js);
    js.runMicrotasks();
    KJ_ASSERT(settled == 2);

    bool completed = false;
    generator.next(js).then(js, [&completed](auto& js, auto value) {
      KJ_ASSERT(value == kj::none);
      completed = true;
    });
    js.runMicrotasks();
    KJ_ASSERT(completed);
  }

  void manualAsyncGeneratorTestEarlyReturn(Lock& js, AsyncGenerator<kj::String> generator) {
    uint calls = 0;
    generator.next(js).then(js, [&calls](auto& js, auto value) {
      calls++;
      KJ_ASSERT(KJ_ASSERT_NONNULL(value) == "a");
      return js.resolvedPromise();
    });

    generator.return_(js, kj::str("foo")).then(js, [&calls](auto& js, auto value) {
      calls++;
      KJ_ASSERT(KJ_ASSERT_NONNULL(value) == "foo");
      return js.resolvedPromise();
    });

    generator.next(js).then(js, [&calls](auto& js, auto value) {
      calls++;
      KJ_ASSERT(value == kj::none);
    });

    js.runMicrotasks();
    KJ_ASSERT(calls == 3);
  }

  void manualAsyncGeneratorTestThrow(Lock& js, AsyncGenerator<kj::String> generator) {
    uint calls = 0;
    generator.next(js).then(js, [&calls](auto& js, auto value) {
      calls++;
      KJ_ASSERT(KJ_ASSERT_NONNULL(value) == "a");
      return js.resolvedPromise();
    });

    // The default implementation of throw on the Async generator will result in a
    // rejected promise being returned by generator.throw_(...)
    generator.throw_(js, js.v8Ref<v8::Value>(js.str("boom"_kj)))
        .catch_(js, [&calls](jsg::Lock& js, jsg::Value exception) {
      calls++;
      return kj::Maybe<kj::String>(kj::none);
    });

    generator.next(js).then(js, [&calls](auto& js, auto value) {
      calls++;
      KJ_ASSERT(value == kj::none);
    });

    js.runMicrotasks();
    KJ_ASSERT(calls == 3);
  }

  struct Test {
    int foo;
    JSG_STRUCT(foo);
  };

  void generatorWrongType(Lock& js, Generator<Test> generator) {
    // This should throw a type error when trying to unwrap the value
    generator.next(js);
  }

  void generatorReturnNotCallable(Lock& js, Generator<kj::String> generator) {
    // Per the GetMethod spec, if the 'return' property exists on the iterator
    // but is not callable, calling return_() should throw a TypeError.
    generator.return_(js);
  }

  void asyncGeneratorReturnNotCallable(Lock& js, AsyncGenerator<kj::String> generator) {
    // Per the GetMethod spec, if the 'return' property exists on the async iterator
    // but is not callable, calling return_() should produce a rejected promise with
    // a TypeError.
    bool gotRejection = false;
    generator.return_(js).catch_(js, [&gotRejection](jsg::Lock& js, jsg::Value exception) {
      gotRejection = true;
      return kj::Maybe<kj::String>(kj::none);
    });
    js.runMicrotasks();
    KJ_ASSERT(gotRejection);
  }

  JSG_RESOURCE_TYPE(GeneratorContext) {
    JSG_METHOD(trackAsyncGenerator);
    JSG_METHOD(startPendingAsyncGenerator);
    JSG_METHOD(trackedAsyncGeneratorIsAlive);
    JSG_METHOD(generatorTest);
    JSG_METHOD(generatorErrorTest);
    JSG_METHOD(sequenceOfSequenceTest);
    JSG_METHOD(sequenceOfBufferSourceTest);
    JSG_METHOD(generatorWrongType);
    JSG_METHOD(generatorReturnNotCallable);
    JSG_METHOD(asyncGeneratorTest);
    JSG_METHOD(asyncGeneratorErrorTest);
    JSG_METHOD(manualAsyncGeneratorTest);
    JSG_METHOD(manualAsyncGeneratorOutOfOrderTest);
    JSG_METHOD(manualAsyncGeneratorTestEarlyReturn);
    JSG_METHOD(manualAsyncGeneratorTestThrow);
    JSG_METHOD(asyncGeneratorReturnNotCallable);
  }
};
JSG_DECLARE_ISOLATE_TYPE(GeneratorIsolate, GeneratorContext, GeneratorContext::Test);

KJ_TEST("Generator works") {
  Evaluator<GeneratorContext, GeneratorIsolate> e(v8System);

  e.expectEval("generatorTest([undefined,2,3])", "object", "undefined,2,3");

  e.expectEval(
      "function* gen() { try { yield 'a'; yield 'b'; yield 'c'; } finally { yield 'd'; } };"
      "generatorTest(gen())",
      "object", "a,b,c,d");

  e.expectEval("function* gen() { try { yield 'a'; yield 'b'; } catch { yield 'c' } }; "
               "generatorErrorTest(gen())",
      "number", "2");

  e.expectEval("sequenceOfSequenceTest([['a','b'],['c', undefined]])", "number", "4");

  e.expectEval("generatorWrongType(['a'])", "throws",
      "TypeError: Incorrect type: the provided value is not of type 'Test'.");

  // Per the GetMethod spec, if the 'return' property exists but is not callable,
  // calling return_() should throw a TypeError.
  e.expectEval("var iter = { [Symbol.iterator]() { return this; }, "
               "next() { return { value: 'a', done: false }; }, "
               "return: 42 }; "
               "generatorReturnNotCallable(iter)",
      "throws", "TypeError: Property 'return' is not a function");
}

KJ_TEST("Sequence follows WebIDL iterable conversion") {
  Evaluator<GeneratorContext, GeneratorIsolate> e(v8System);

  // Elements that hold a v8::Local stay valid after the conversion returns.
  e.expectEval("function* gen() { yield new Uint8Array(3); yield new ArrayBuffer(2); }; "
               "sequenceOfBufferSourceTest(gen())",
      "number", "5");
  e.expectEval("sequenceOfBufferSourceTest([new Uint8Array(4), new DataView(new ArrayBuffer(1))])",
      "number", "5");

  e.expectEval("sequenceOfSequenceTest({ *[Symbol.iterator]() { yield new String('ab'); } })",
      "number", "2");
  e.expectEval("sequenceOfSequenceTest(['ab'])", "throws",
      "TypeError: Incorrect type for array element 0: the provided value is not of type "
      "'Sequence'.");

  // A failed element conversion propagates without calling the iterator's throw() or return().
  e.expectEval("let closed = false;"
               "const it = { [Symbol.iterator]() { return this; },"
               "  next() { return { value: 1, done: false }; },"
               "  throw() { closed = true; }, return() { closed = true; } };"
               "try { sequenceOfSequenceTest(it); } catch {}; closed",
      "boolean", "false");

  e.expectEval("sequenceOfSequenceTest({ [Symbol.iterator]() { return 1; } })", "throws",
      "TypeError: Result of the Symbol.iterator method is not an object");
  e.expectEval("sequenceOfSequenceTest({ [Symbol.iterator]() { return { next: 1 }; } })", "throws",
      "TypeError: Iterator's next method is not a function");
  e.expectEval(
      "sequenceOfSequenceTest({ [Symbol.iterator]() { return { next() { return 1; } }; } })",
      "throws", "TypeError: Iterator result is not an object");
}

KJ_TEST("AsyncGenerator works") {
  Evaluator<GeneratorContext, GeneratorIsolate> e(v8System);

  e.expectEval(
      "async function* foo() { yield 'a'; yield 'b'; }; asyncGeneratorTest(foo());", "number", "2");

  e.expectEval("async function* gen() { try { yield 'a'; yield 'b'; } catch { yield 'c' } }; "
               "asyncGeneratorErrorTest(gen())",
      "number", "2");

  e.expectEval("manualAsyncGeneratorTest(async function* foo() { yield 'a'; yield 'b'; }())",
      "undefined", "undefined");

  e.expectEval(R"(
    let calls = 0;
    let resolveFirst;
    let resolveSecond;
    const iterator = {
      [Symbol.asyncIterator]() { return this; },
      next() {
        switch (++calls) {
          case 1: return new Promise((resolve) => { resolveFirst = resolve; });
          case 2: return new Promise((resolve) => { resolveSecond = resolve; });
          default: return Promise.resolve({ value: 'unexpected', done: false });
        }
      },
    };
    manualAsyncGeneratorOutOfOrderTest(iterator, () => {
      resolveSecond({ done: true });
      resolveFirst({ value: 'a', done: false });
    });
    calls;
  )",
      "number", "2");

  e.expectEval("manualAsyncGeneratorTestEarlyReturn(async function* foo() "
               "{ yield 'a'; yield 'b'; }())",
      "undefined", "undefined");

  // e.expectEval("manualAsyncGeneratorTestThrow(async function* foo() { yield 'a'; yield 'b'; }())",
  //     "undefined", "undefined");

  // Per the GetMethod spec, if the 'return' property exists but is not callable,
  // calling return_() should produce a rejected promise with a TypeError.
  e.expectEval("var iter = { [Symbol.asyncIterator]() { return this; }, "
               "next() { return Promise.resolve({ value: 'a', done: false }); }, "
               "return: 42 }; "
               "asyncGeneratorReturnNotCallable(iter)",
      "undefined", "undefined");
}

KJ_TEST("pending AsyncGenerator does not retain an unreachable iterator") {
  Evaluator<GeneratorContext, GeneratorIsolate> e(v8System);

  e.expectEval(R"(
    let controlCollected;
    (() => {
      let iterator = (async function*() {
        await new Promise(() => {});
      })();
      trackAsyncGenerator(iterator);
      iterator.next();
      iterator = null;
    })();
    gc();
    gc();
    controlCollected = !trackedAsyncGeneratorIsAlive();

    (() => {
      let iterator = (async function*() {
        await new Promise(() => {});
      })();
      startPendingAsyncGenerator(iterator, iterator);
      iterator = null;
    })();
    gc();
    gc();

    [controlCollected, !trackedAsyncGeneratorIsAlive()];
  )",
      "object", "true,true");
}

}  // namespace
}  // namespace workerd::jsg::test
