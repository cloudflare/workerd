// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;

struct GeneratorContext: public Object, public ContextGlobal {

  uint generatorTest(Lock& js, Generator<kj::String> generator) {

    KJ_DEFER(generator.forEach(
        js, [](auto& js, auto, auto&) { KJ_FAIL_ASSERT("Should not have been called"); }));

    uint count = 0;
    auto ret = generator.forEach(js, [&count](auto& js, auto val, auto& context) {
      if (count == 2 && !context.isReturning()) {
        return context.return_(js, kj::str("foo"));
      }

      ++count;
    });
    KJ_ASSERT(KJ_ASSERT_NONNULL(ret) == "foo");

    // Moving the generator then accessing it doesn't crash anything.
    auto gen2 = kj::mv(generator);
    gen2.forEach(
        js, [](auto& js, auto, auto&) { KJ_FAIL_ASSERT("Should not actually be called"); });

    return count;
  }

  uint generatorErrorTest(Lock& js, Generator<kj::String> generator) {
    uint count = 0;
    generator.forEach(js, [&count](auto& js, auto value, auto& context) {
      if (count == 1 && !context.isErroring()) {
        js.throwException(JSG_KJ_EXCEPTION(FAILED, Error, "boom"));
      }

      KJ_ASSERT(value == "a" || value == "c");

      ++count;
    });
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
    generator
        .forEach(js, [&count](auto& js, auto, auto& context) {
      if (count == 1 && !context.isReturning()) {
        context.return_(js, kj::str("foo"));
      } else {
        ++count;
      }
      return js.resolvedPromise();
    }).then(js, [&finished](auto& js, auto value) {
      KJ_ASSERT(KJ_ASSERT_NONNULL(value) == "foo");
      finished = true;
    });

    // Should just return a resolved promise without crashing.
    generator.forEach(js, [](auto& js, auto, auto&) -> Promise<void> {
      KJ_FAIL_ASSERT("Should not have been called");
    });

    js.runMicrotasks();

    KJ_ASSERT(finished);

    return count;
  }

  uint asyncGeneratorErrorTest(Lock& js, AsyncGenerator<kj::String> generator) {
    uint count = 0;
    generator.forEach(js, [&count](auto& js, auto val, auto& context) -> Promise<void> {
      if (count == 1 && !context.isErroring()) {
        js.throwException(JSG_KJ_EXCEPTION(FAILED, Error, "boom"));
      }

      KJ_ASSERT(val == "a" || val == "c");

      ++count;
      return js.resolvedPromise();
    });

    js.runMicrotasks();

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

    generator.return_(js, kj::str("foo")).then(js, [&calls](auto& js) { calls++; });

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
        .catch_(js, [&calls](jsg::Lock& js, jsg::Value exception) { calls++; });

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
    generator.forEach(js, [](auto&, auto, auto& context) {});
  }

  JSG_RESOURCE_TYPE(GeneratorContext) {
    JSG_METHOD(generatorTest);
    JSG_METHOD(generatorErrorTest);
    JSG_METHOD(sequenceOfSequenceTest);
    JSG_METHOD(generatorWrongType);
    JSG_METHOD(asyncGeneratorTest);
    JSG_METHOD(asyncGeneratorErrorTest);
    JSG_METHOD(manualAsyncGeneratorTest);
    JSG_METHOD(manualAsyncGeneratorTestEarlyReturn);
    JSG_METHOD(manualAsyncGeneratorTestThrow);
  }
};
JSG_DECLARE_ISOLATE_TYPE(GeneratorIsolate, GeneratorContext, GeneratorContext::Test);

KJ_TEST("Generator works") {
  Evaluator<GeneratorContext, GeneratorIsolate> e(v8System);

  e.expectEval("generatorTest([undefined,2,3])", "number", "2");

  e.expectEval(
      "function* gen() { try { yield 'a'; yield 'b'; yield 'c'; } finally { yield 'd'; } };"
      "generatorTest(gen())",
      "number", "3");

  e.expectEval("function* gen() { try { yield 'a'; yield 'b'; } catch { yield 'c' } }; "
               "generatorErrorTest(gen())",
      "number", "2");

  e.expectEval("sequenceOfSequenceTest([['a','b'],['c', undefined]])", "number", "4");

  e.expectEval("generatorWrongType(['a'])", "throws",
      "TypeError: Incorrect type: the provided value is not of type 'Test'.");
}

KJ_TEST("AsyncGenerator works") {
  Evaluator<GeneratorContext, GeneratorIsolate> e(v8System);

  e.expectEval(
      "async function* foo() { yield 'a'; yield 'b'; }; asyncGeneratorTest(foo());", "number", "1");

  e.expectEval("async function* foo() { try { yield 'a'; yield 'b'; } finally { yield 'c'; } };"
               "asyncGeneratorTest(foo());",
      "number", "2");

  e.expectEval("async function* gen() { try { yield 'a'; yield 'b'; } catch { yield 'c' } }; "
               "asyncGeneratorErrorTest(gen())",
      "number", "2");

  e.expectEval("manualAsyncGeneratorTest(async function* foo() { yield 'a'; yield 'b'; }())",
      "undefined", "undefined");
  e.expectEval("manualAsyncGeneratorTestEarlyReturn(async function* foo() "
               "{ yield 'a'; yield 'b'; }())",
      "undefined", "undefined");
  e.expectEval("manualAsyncGeneratorTestThrow(async function* foo() { yield 'a'; yield 'b'; }())",
      "undefined", "undefined");
}

}  // namespace
}  // namespace workerd::jsg::test
