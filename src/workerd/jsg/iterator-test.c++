// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;

struct GeneratorContext: public Object, public ContextGlobal {

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

  uint asyncGeneratorTest(Lock& js, AsyncGenerator<kj::String> generator) {
    uint count = 0;
    bool finished = false;

    // Get first item
    generator.next(js)
        .then(js, [&count, &generator](auto& js, auto value) {
      KJ_ASSERT(KJ_ASSERT_NONNULL(value) == "a");
      ++count;

      // After getting first item, call return_() to terminate early
      return generator.return_(js, kj::str("foo")).then(js, [&count](auto& js, auto value) {
        // return_() should give us back "foo" and mark as done
        KJ_ASSERT(KJ_ASSERT_NONNULL(value) == "foo");
        ++count;
        return js.resolvedPromise();
      });
    }).then(js, [&finished](auto& js) {
      finished = true;
      return js.resolvedPromise();
    });

    js.runMicrotasks();

    KJ_ASSERT(finished);
    KJ_ASSERT(count == 2);

    return count;
  }

  uint asyncGeneratorErrorTest(Lock& js, AsyncGenerator<kj::String> generator) {
    uint count = 0;
    bool finished = false;

    // First call to next() should succeed and return "a"
    generator.next(js)
        .then(js, [&count, &generator](auto& js, auto value) {
      KJ_ASSERT(KJ_ASSERT_NONNULL(value) == "a");
      ++count;

      // Second call - throw an error, which should trigger the generator's
      // throw handler (the catch block), which yields "c"
      return generator.throw_(js, js.template v8Ref<v8::Value>(js.str("boom"_kj)))
          .then(js, [&count](auto& js, auto value) {
        KJ_ASSERT(KJ_ASSERT_NONNULL(value) == "c");
        ++count;
        return js.resolvedPromise();
      });
    }).then(js, [&finished](auto& js) {
      finished = true;
      return js.resolvedPromise();
    });

    js.runMicrotasks();

    KJ_ASSERT(finished);
    KJ_ASSERT(count == 2);

    return count;
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
    JSG_METHOD(generatorTest);
    JSG_METHOD(generatorErrorTest);
    JSG_METHOD(sequenceOfSequenceTest);
    JSG_METHOD(generatorWrongType);
    JSG_METHOD(generatorReturnNotCallable);
    JSG_METHOD(asyncGeneratorTest);
    JSG_METHOD(asyncGeneratorErrorTest);
    JSG_METHOD(manualAsyncGeneratorTest);
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

KJ_TEST("AsyncGenerator works") {
  Evaluator<GeneratorContext, GeneratorIsolate> e(v8System);

  e.expectEval(
      "async function* foo() { yield 'a'; yield 'b'; }; asyncGeneratorTest(foo());", "number", "2");

  e.expectEval("async function* gen() { try { yield 'a'; yield 'b'; } catch { yield 'c' } }; "
               "asyncGeneratorErrorTest(gen())",
      "number", "2");

  e.expectEval("manualAsyncGeneratorTest(async function* foo() { yield 'a'; yield 'b'; }())",
      "undefined", "undefined");

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

// ======================================================================================
// Test that JSG iterators respect TerminateExecution().
//
// When V8 builtins like Array.from() or spread syntax iterate a C++ iterator, the loop
// runs entirely in C++ without JS back-edge interrupt checks. Without the termination
// check in IteratorBase::nextImpl, the iterator would keep getting called indefinitely
// after TerminateExecution(). See https://crbug.com/v8/14681 for the same class of bug
// in Intl.Segmenter.

struct CountingIterable: public Object {
  int terminateAfter;

  explicit CountingIterable(int terminateAfter): terminateAfter(terminateAfter) {}

  struct IterState {
    int current = 0;
    int terminateAfter;
  };

  // After producing `terminateAfter` items, requests termination but keeps returning
  // values. Without the termination check in nextImpl, the iterator would keep getting called.
  static kj::Maybe<int> nextItem(Lock& js, IterState& state) {
    if (state.current >= state.terminateAfter) {
      js.requestTermination();
    }
    return state.current++;
  }

  JSG_ITERATOR(ValuesIterator, values, int, IterState, nextItem);

  JSG_RESOURCE_TYPE(CountingIterable) {
    JSG_ITERABLE(values);
  }
};

Ref<CountingIterable::ValuesIterator> CountingIterable::values(Lock&) {
  return alloc<ValuesIterator>(IterState{.current = 0, .terminateAfter = terminateAfter});
}

struct TerminationIteratorContext: public Object, public ContextGlobal {
  Ref<CountingIterable> makeCountingIterable(Lock& js, int terminateAfter) {
    return js.alloc<CountingIterable>(terminateAfter);
  }

  JSG_RESOURCE_TYPE(TerminationIteratorContext) {
    JSG_METHOD(makeCountingIterable);
    JSG_NESTED_TYPE(CountingIterable);
  }
};

JSG_DECLARE_ISOLATE_TYPE(TerminationIteratorIsolate,
    TerminationIteratorContext,
    CountingIterable,
    CountingIterable::ValuesIterator,
    CountingIterable::ValuesIterator::Next);

KJ_TEST("Iterator respects TerminateExecution via Array.from()") {
  Evaluator<TerminationIteratorContext, TerminationIteratorIsolate> e(v8System);

  e.getIsolate().runInLockScope([&](TerminationIteratorIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock,
        lock.newContext<TerminationIteratorContext>().getHandle(lock.v8Isolate),
        [&](jsg::Lock& js) {
      // Array.from() runs the iteration in a V8 Torque builtin without JS back-edge
      // interrupt checks. The iterator calls TerminateExecution() after 5 items but
      // keeps returning values. The check in nextImpl should stop the iteration.
      v8::Local<v8::String> source =
          jsg::v8Str(js.v8Isolate, "Array.from(makeCountingIterable(5))");
      v8::Local<v8::Script> script = v8::Script::Compile(js.v8Context(), source).ToLocalChecked();

      v8::TryCatch catcher(js.v8Isolate);
      v8::Local<v8::Value> result;
      bool completed = script->Run(js.v8Context()).ToLocal(&result);

      // The script should not complete normally -- the termination check in
      // nextImpl should throw. Without the fix, this would loop indefinitely.
      KJ_EXPECT(!completed);
      KJ_EXPECT(catcher.HasCaught());
    });
  });
}

}  // namespace
}  // namespace workerd::jsg::test
