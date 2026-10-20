// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

#include <v8-microtask-queue.h>

namespace workerd::jsg::test {
namespace {

V8System v8System({"--expose-gc"_kj});
class ContextGlobalObject: public Object, public ContextGlobal {};

struct CallbackContext: public ContextGlobalObject {
  kj::String callCallback(Lock& js, jsg::Function<kj::String(kj::StringPtr, double)> function) {
    return kj::str(function(js, "foo", 123), ", abc");
  }
  double callCallbackReturningBox(Lock& js, jsg::Function<Ref<NumberBox>()> function) {
    return function(js)->value;
  }

  struct Frobber {
    kj::String s;
    double n;
    jsg::Function<kj::String(double)> frob;
    Optional<jsg::Function<kj::String(double)>> optionalFrob;
    kj::Maybe<jsg::Function<kj::String(double)>> maybeFrob;
    JSG_STRUCT(s, n, frob, optionalFrob, maybeFrob);
  };

  kj::String callConstructor(Lock& js, Constructor<Frobber(kj::StringPtr, double)> constructor) {
    auto frobber = constructor(js, "foo", 123);
    return kj::str(frobber.s, frobber.n, frobber.frob(js, 321),
        KJ_ASSERT_NONNULL(frobber.optionalFrob)(js, 654),
        KJ_ASSERT_NONNULL(frobber.maybeFrob)(js, 987));
  }

  JSG_RESOURCE_TYPE(CallbackContext) {
    JSG_NESTED_TYPE(NumberBox);
    JSG_METHOD(callCallback);
    JSG_METHOD(callCallbackReturningBox);
    JSG_METHOD(callConstructor);
  }
};
JSG_DECLARE_ISOLATE_TYPE(CallbackIsolate, CallbackContext, CallbackContext::Frobber, NumberBox);

KJ_TEST("callbacks") {
  Evaluator<CallbackContext, CallbackIsolate> e(v8System);
  e.expectEval("callCallback((str, num) => {\n"
               "  return [typeof str, str, typeof num, num.toString(), 'bar'].join(', ');\n"
               "})",
      "string", "string, foo, number, 123, bar, abc");

  e.expectEval("callCallback((str, num) => {\n"
               "  throw new Error('error message')\n"
               "})",
      "throws", "Error: error message");

  e.expectEval("callCallbackReturningBox(() => {\n"
               "  return new NumberBox(123);\n"
               "})",
      "number", "123");
  e.expectEval("callCallbackReturningBox(() => {\n"
               "  return 'foo';\n"
               "})",
      "throws", "TypeError: Callback returned incorrect type; expected 'NumberBox'");

  e.expectEval("class Frobber {\n"
               "  constructor(s, n) {\n"
               "    this.s = s;\n"
               "    this.n = n;\n"
               "  }\n"
               "  frob(m) {\n"
               "    return this.s + (m + this.n);\n"
               "  }\n"
               "  optionalFrob(m) {\n"
               "    return 'opn' + this.s + (m + this.n);\n"
               "  }\n"
               "  maybeFrob(m) {\n"
               "    return 'mby' + this.s + (m + this.n);\n"
               "  }\n"
               "}\n"
               "callConstructor(Frobber)",
      "string", "foo123foo444opnfoo777mbyfoo1110");
}

// ========================================================================================

struct WrapContext: public ContextGlobalObject {
  auto returnFunction(double value) {
    return [value](Lock&, double value2) { return value + value2; };
  }
  auto returnFunctionWithInfo(double value) {
    return [value](Lock&, const v8::FunctionCallbackInfo<v8::Value>& info, double value2) {
      // Prove that we received `info` by adding in the argument count.
      return value + value2 + info.Length();
    };
  }
  auto returnFunctionMutable(double value) {
    return [value](Lock&, double value2) mutable { return value + value2; };
  }
  auto returnFunctionWithInfoMutable(double value) {
    return [value](Lock&, const v8::FunctionCallbackInfo<v8::Value>& info, double value2) mutable {
      // Prove that we received `info` by adding in the argument count.
      return value + value2 + info.Length();
    };
  }
  auto returnFunctionReturningVoid(double value) {
    return [value](Lock&, NumberBox& box) -> void { box.value = value; };
  }

  JSG_RESOURCE_TYPE(WrapContext) {
    JSG_NESTED_TYPE(NumberBox);
    JSG_METHOD(returnFunction);
    JSG_METHOD(returnFunctionWithInfo);
    JSG_METHOD(returnFunctionMutable);
    JSG_METHOD(returnFunctionWithInfoMutable);
    JSG_METHOD(returnFunctionReturningVoid);
  }
};
JSG_DECLARE_ISOLATE_TYPE(WrapIsolate, WrapContext, NumberBox);

KJ_TEST("wrap functions") {
  Evaluator<WrapContext, WrapIsolate> e(v8System);

  e.expectEval("returnFunction(123)(321)", "number", "444");
  e.expectEval("returnFunctionWithInfo(123)(321, '', undefined)", "number", "447");
  e.expectEval("returnFunctionMutable(123)(321)", "number", "444");
  e.expectEval("returnFunctionWithInfoMutable(123)(321, '', undefined)", "number", "447");

  e.expectEval("var nb = new NumberBox(321);\n"
               "var ret = returnFunctionReturningVoid(123)(nb);\n"
               "ret === undefined ? nb.value : 555",
      "number", "123");
}

// ========================================================================================

struct FunctionContext: public ContextGlobalObject {
  auto test(Lock& js, Function<bool(int)> fn) {
    return fn(js, 1);
  }

  struct Foo {
    Function<bool()> fn;
    JSG_STRUCT(fn);
  };

  auto test2(Lock& js, Foo foo) {
    return foo.fn(js);
  }

  jsg::Function<double(double)> getSquare(Lock& js) {
    jsg::Function<double(double)> result = [](Lock&, double x) { return x * x; };

    // Check we can call it directly.
    KJ_ASSERT(result(js, 11) == 121);

    return result;
  }

  struct VisitDetector {
    bool visited = false;
    void visitForGc(GcVisitor& visitor) {
      visited = true;
    }
  };

  jsg::Function<int(int)> getGcLambda() {
    return JSG_VISITABLE_LAMBDA((v1 = VisitDetector(), v2 = VisitDetector(), v3 = VisitDetector()),
        (v1, v3), (Lock&, int i) {
          KJ_ASSERT(i == 123);

          // Should return 5, since v1 and v3 are visited but v2 is not. Note that a discovery
          // visitation pass happens immediately upon constructing wrappers -- we don't need to wait
          // for an actual GC pass, which is nice for this test.
          return v1.visited + v2.visited * 2 + v3.visited * 4;
        });
  }

  jsg::Function<int(int, int)> getTwoArgs() {
    return JSG_VISITABLE_LAMBDA((), (), (Lock&, int i, int j) {
      // Also test an unparenthesized comma...
      return ++i, i * j;
    });
  }

  kj::String testTryCatch(Lock& js, jsg::Function<int()> thrower) {
    return js.tryCatch([&]() { return kj::str(thrower(js)); }, [&](Value exception) {
      auto handle = exception.getHandle(js);
      return kj::str("caught: ", handle);
    });
  }

  kj::String testTryCatch2(Lock& js, jsg::Function<int()> thrower) {
    // Here we prove that the macro is if-else friendly.
    if (true) JSG_TRY(js) {
        return kj::str(thrower(js));
      }
    JSG_CATCH(exception) {
      auto handle = exception.getHandle(js);
      return kj::str("caught: ", handle);
    }
    else {
      KJ_UNREACHABLE;
    }
  }

  kj::String testTryCatchWithOptions(Lock& js, jsg::Function<void()> thrower) {
    // Test that JSG_CATCH can accept ExceptionToJsOptions.
    JSG_TRY(js) {
      thrower(js);
      return kj::str("no exception");
    }
    JSG_CATCH(exception, {.ignoreDetail = true}) {
      auto handle = exception.getHandle(js);
      return kj::str("caught with options: ", handle);
    }
  }

  kj::String testNestedTryCatchInnerCatches(Lock& js, jsg::Function<void()> thrower) {
    // Test nested JSG_TRY/JSG_CATCH where inner catches, outer doesn't see exception.
    JSG_TRY(js) {
      kj::String innerResult;
      JSG_TRY(js) {
        thrower(js);
        innerResult = kj::str("inner: no exception");
      }
      JSG_CATCH(innerException) {
        innerResult = kj::str("inner caught: ", innerException.getHandle(js));
      }
      return kj::str("outer: no exception, ", innerResult);
    }
    JSG_CATCH(outerException) {
      return kj::str("outer caught: ", outerException.getHandle(js));
    }
  }

  kj::String testNestedTryCatchOuterCatches(Lock& js, jsg::Function<void()> thrower) {
    // Test nested JSG_TRY/JSG_CATCH where inner rethrows, outer catches.
    JSG_TRY(js) {
      JSG_TRY(js) {
        thrower(js);
        return kj::str("inner: no exception");
      }
      JSG_CATCH(innerException) {
        // Rethrow so outer can catch
        js.throwException(kj::mv(innerException));
      }
      return kj::str("outer: no exception");
    }
    JSG_CATCH(outerException) {
      return kj::str("outer caught: ", outerException.getHandle(js));
    }
  }

  JSG_RESOURCE_TYPE(FunctionContext) {
    JSG_METHOD(test);
    JSG_METHOD(test2);
    JSG_METHOD(testTryCatch);
    JSG_METHOD(testTryCatch2);
    JSG_METHOD(testTryCatchWithOptions);
    JSG_METHOD(testNestedTryCatchInnerCatches);
    JSG_METHOD(testNestedTryCatchOuterCatches);

    JSG_READONLY_PROTOTYPE_PROPERTY(square, getSquare);
    JSG_READONLY_PROTOTYPE_PROPERTY(gcLambda, getGcLambda);
    JSG_READONLY_PROTOTYPE_PROPERTY(twoArgs, getTwoArgs);
  }
};
JSG_DECLARE_ISOLATE_TYPE(FunctionIsolate, FunctionContext, FunctionContext::Foo);

KJ_TEST("jsg::Function<T>") {
  Evaluator<FunctionContext, FunctionIsolate> e(v8System);

  e.expectEval("test((val) => val === 1)", "boolean", "true");

  // This variation checks that a jsg::Function pulled off a struct properly
  // preserves "this" as a reference to the object it was pulled off of.
  e.expectEval("const m = { fn() { return this === m; } }; test2(m);", "boolean", "true");

  e.expectEval("square(5)", "number", "25");

  e.expectEval("gcLambda(123)", "number", "5");

  e.expectEval("twoArgs(2, 5)", "number", "15");

  e.expectEval("testTryCatch(() => { return 123; })", "string", "123");
  e.expectEval("testTryCatch(() => { throw new Error('foo'); })", "string", "caught: Error: foo");

  e.expectEval("testTryCatch2(() => { return 123; })", "string", "123");
  e.expectEval("testTryCatch2(() => { throw new Error('foo'); })", "string", "caught: Error: foo");

  e.expectEval("testTryCatchWithOptions(() => {})", "string", "no exception");
  e.expectEval("testTryCatchWithOptions(() => { throw new Error('bar'); })", "string",
      "caught with options: Error: bar");

  // Nested JSG_TRY/JSG_CATCH tests
  e.expectEval("testNestedTryCatchInnerCatches(() => {})", "string",
      "outer: no exception, inner: no exception");
  e.expectEval("testNestedTryCatchInnerCatches(() => { throw new Error('inner'); })", "string",
      "outer: no exception, inner caught: Error: inner");

  e.expectEval("testNestedTryCatchOuterCatches(() => {})", "string", "inner: no exception");
  e.expectEval("testNestedTryCatchOuterCatches(() => { throw new Error('rethrown'); })", "string",
      "outer caught: Error: rethrown");
}

KJ_TEST("JSG_TRY/JSG_CATCH with TerminateExecution") {
  Evaluator<FunctionContext, FunctionIsolate> e(v8System);

  // TerminateExecution should propagate through JSG_CATCH without being caught.
  // The Evaluator's run() method will detect the termination and throw.
  KJ_EXPECT_THROW_MESSAGE("TerminateExecution() was called", e.run([](auto& js) {
    // Test single-level JSG_TRY/JSG_CATCH with TerminateExecution
    JSG_TRY(js) {
      js.terminateExecutionNow();
    }
    JSG_CATCH(exception) {
      (void)exception;
      KJ_FAIL_ASSERT("TerminateExecution was caught by JSG_CATCH");
    }
  }));

  KJ_EXPECT_THROW_MESSAGE("TerminateExecution() was called", e.run([](auto& js) {
    // Test nested JSG_TRY/JSG_CATCH with TerminateExecution - should propagate through both
    JSG_TRY(js) {
      JSG_TRY(js) {
        js.terminateExecutionNow();
      }
      JSG_CATCH(innerException) {
        (void)innerException;
        KJ_FAIL_ASSERT("TerminateExecution was caught by inner JSG_CATCH");
      }
    }
    JSG_CATCH(outerException) {
      (void)outerException;
      KJ_FAIL_ASSERT("TerminateExecution was caught by outer JSG_CATCH");
    }
  }));
}

// ========================================================================================

struct MicrotaskCanary {
  bool& destroyed;
  ~MicrotaskCanary() {
    destroyed = true;
  }
};

KJ_TEST("Lock::enqueueMicrotask()") {
  Evaluator<FunctionContext, FunctionIsolate> e(v8System);

  // GC keeps the callback alive and visits it while it is pending, and the callback and its
  // captures are destroyed as soon as it returns, without waiting for GC.
  e.run([](Lock& js) {
    bool destroyed = false;
    bool visitedBeforeRun = false;
    uint runs = 0;
    js.enqueueMicrotask(JSG_VISITABLE_LAMBDA(
        (canary = kj::heap<MicrotaskCanary>(destroyed), detector = FunctionContext::VisitDetector(),
            &runs, &visitedBeforeRun),
        (detector), (Lock&) {
          ++runs;
          visitedBeforeRun = detector.visited;
        }));
    js.v8Isolate->RequestGarbageCollectionForTesting(v8::Isolate::kFullGarbageCollection);
    KJ_EXPECT(runs == 0);
    KJ_EXPECT(!destroyed);

    js.runMicrotasks();
    KJ_EXPECT(runs == 1);
    KJ_EXPECT(visitedBeforeRun);
    KJ_EXPECT(destroyed);
  });

  // A JavaScript exception escaping the callback is reported, and doesn't stop later microtasks.
  e.run([](Lock& js) {
    kj::Vector<kj::String> reported;
    js.setErrorReporterCallback([&reported](Lock&, kj::String desc, const JsValue&,
                                    const JsMessage&) { reported.add(kj::mv(desc)); });
    bool destroyed = false;
    bool ranAfter = false;
    js.enqueueMicrotask([canary = kj::heap<MicrotaskCanary>(destroyed)](
                            Lock& js) { js.throwException(js.error("boom")); });
    js.enqueueMicrotask([&ranAfter](Lock&) { ranAfter = true; });

    js.runMicrotasks();
    KJ_EXPECT(destroyed);
    KJ_EXPECT(ranAfter);
    KJ_EXPECT(reported.size() == 1);
    KJ_EXPECT(reported[0] == "Error: boom", reported[0]);
  });
}

KJ_TEST("Lock::enqueueMicrotask() runs in the enqueuing context") {
  Evaluator<FunctionContext, FunctionIsolate> e(v8System);

  e.run([](auto& lock) {
    Lock& js = lock;
    auto enqueuingContext = js.v8Context();
    bool ranInEnqueuingContext = false;
    js.enqueueMicrotask(
        [&](Lock& js) { ranInEnqueuingContext = js.v8Context() == enqueuingContext; });

    auto otherContext = lock.template newContext<FunctionContext>();
    JSG_WITHIN_CONTEXT_SCOPE(js, otherContext.getHandle(js), [](Lock& js) { js.runMicrotasks(); });
    KJ_EXPECT(ranInEnqueuingContext);
  });
}

size_t oldSpaceUsed(v8::Isolate* isolate) {
  for (size_t i = 0; i < isolate->NumberOfHeapSpaces(); i++) {
    v8::HeapSpaceStatistics stats;
    isolate->GetHeapSpaceStatistics(&stats, i);
    if (kj::StringPtr(stats.space_name()) == "old_space") {
      return stats.space_used_size();
    }
  }
  KJ_UNREACHABLE;
}

KJ_TEST("Lock::enqueueMicrotask() allocates nothing in old space") {
  Evaluator<FunctionContext, FunctionIsolate> e(v8System);

  e.run([](Lock& js) {
    auto isolate = js.v8Isolate;
    constexpr uint count = 10000;
    auto oldSpaceGrowth = [&](auto enqueue) {
      isolate->RequestGarbageCollectionForTesting(v8::Isolate::kFullGarbageCollection);
      auto before = oldSpaceUsed(isolate);
      for (uint i = 0; i < count; i++) {
        enqueue();
      }
      js.runMicrotasks();
      isolate->RequestGarbageCollectionForTesting(v8::Isolate::kMinorGarbageCollection);
      auto after = oldSpaceUsed(isolate);
      return after > before ? after - before : 0;
    };

    // A wrapped function allocates a FunctionTemplateInfo in old space per call.
    auto wrapped = oldSpaceGrowth([&]() {
      js.v8Context()->GetMicrotaskQueue()->EnqueueMicrotask(isolate,
          js.wrapSimpleFunction(
              js.v8Context(), [](Lock&, const v8::FunctionCallbackInfo<v8::Value>&) {}));
    });
    auto enqueued = oldSpaceGrowth([&]() { js.enqueueMicrotask([](Lock&) {}); });

    KJ_EXPECT(wrapped > count * 32, wrapped);
    KJ_EXPECT(enqueued < wrapped / 10, enqueued, wrapped);
  });
}

}  // namespace
}  // namespace workerd::jsg::test
