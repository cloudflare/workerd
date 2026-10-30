#include "jsg-test.h"

#include <workerd/util/autogate.h>

#include <signal.h>
#ifdef __linux__
#include <sys/mman.h>
#endif

namespace workerd::jsg::test {
namespace {

using jsg::CallCounter;

volatile sig_atomic_t* fastCallbackEntered = nullptr;

void recordFastCallback(int) {
  *fastCallbackEntered = jsg::callCounter.fast > 0;
}

struct WrappedInt {
  int32_t i;

  JSG_STRUCT(i);
};

class StaticMethodContainer: public jsg::Object {
 public:
  StaticMethodContainer() = default;

  static int32_t staticMethod() {
    return 42;
  }

  uint32_t getValue() const {
    return value;
  }

  void setValue(uint32_t value_) {
    value = value_;
  }

  // On a non-context receiver, so these take the reduced fast callback.
  int32_t addPrimitive(int32_t a, int32_t b) {
    return a + b;
  }

  int32_t throwPrimitive(int32_t code) {
    if (code > 0) {
      JSG_FAIL_REQUIRE(TypeError, "Primitive error with code ", code);
    }
    return 0;
  }

  int32_t measure(kj::String s) {
    return s.size();
  }

  int32_t measureTwo(kj::String a, kj::String b) {
    return a.size() + b.size();
  }

  int32_t measureWithLock(jsg::Lock& js, kj::String s) {
    return js.str(s).length(js);
  }

  static int32_t staticAdd(int32_t a, int32_t b) {
    return a + b;
  }

  static int32_t staticMeasure(kj::String s) {
    return s.size();
  }

  static int32_t staticMeasureWithLock(jsg::Lock& js, kj::String s) {
    return js.str(s).length(js);
  }

  uint32_t getLabel() const {
    return label.size();
  }

  void setLabel(kj::String s) {
    label = kj::mv(s);
  }

  uint32_t getLockedLabel(jsg::Lock&) const {
    return lockedLabelLength;
  }

  void setLockedLabel(jsg::Lock& js, kj::String s) {
    lockedLabelLength = js.str(s).length(js);
  }

  JSG_RESOURCE_TYPE(StaticMethodContainer) {
    JSG_STATIC_METHOD(staticMethod);
    JSG_PROTOTYPE_PROPERTY(value, getValue, setValue);
    JSG_METHOD(addPrimitive);
    JSG_METHOD(throwPrimitive);
    JSG_METHOD(measure);
    JSG_METHOD(measureTwo);
    JSG_METHOD(measureWithLock);
    JSG_STATIC_METHOD(staticAdd);
    JSG_STATIC_METHOD(staticMeasure);
    JSG_STATIC_METHOD(staticMeasureWithLock);
    JSG_PROTOTYPE_PROPERTY(label, getLabel, setLabel);
    JSG_PROTOTYPE_PROPERTY(lockedLabel, getLockedLabel, setLockedLabel);
  }

 private:
  uint32_t value = 42;
  kj::String label = kj::str();
  uint32_t lockedLabelLength = 0;
};

class FastMethodContext: public jsg::Object, public jsg::ContextGlobal {
 public:
  int32_t add(int32_t a, int32_t b) {
    return a + b;
  }

  int32_t processValue(v8::Local<v8::Value> value) {
    KJ_ASSERT(value->IsNumber(), "Value must be a number");
    return value.As<v8::Int32>()->Value();
  }

  // When arbitrary type unwrapping is supported, keep this test as it is.
  int32_t processObject(v8::Local<v8::Object> obj) {
    auto& js = jsg::Lock::current();
    auto context = js.v8Context();
    auto testKey = v8::String::NewFromUtf8(js.v8Isolate, "test").ToLocalChecked();
    v8::Local<v8::Value> value;
    if (obj->Get(context, testKey).ToLocal(&value)) {
      v8::String::Utf8Value type(js.v8Isolate, value->TypeOf(js.v8Isolate));
      KJ_ASSERT(value->IsInt32(), "Received ", *type);
      return value.As<v8::Int32>()->Value();
    }
    return 0;
  }

  void voidMethod(uint32_t a, uint32_t b) {
    // Do nothing. This is testing void return type.
  }

  int32_t throwError(int32_t code) {
    if (code > 0) {
      JSG_FAIL_REQUIRE(TypeError, "Test error with code ", code);
    }
    return 0;
  }

  int32_t addWithLock(jsg::Lock& js, int32_t a, v8::Local<v8::Value> b) {
    KJ_ASSERT(b->IsNumber(), "Second parameter must be a number");
    int32_t bValue = b.As<v8::Int32>()->Value();
    return a + bValue;
  }

  int32_t constAdd(int32_t a, int32_t b) {
    return a + b;
  }

  int32_t constAddWithLock(jsg::Lock& js, int32_t a, int32_t b) {
    return a + b;
  }

  int32_t unwrapStruct(jsg::Lock& js, WrappedInt w) {
    return w.i;
  }

  int32_t unwrapUint(jsg::Lock& js, kj::uint u) {
    return u;
  }

  int32_t unwrapString(jsg::Lock& js, kj::String str) {
    return str.size();
  }

  int32_t unwrapMaybe(jsg::Lock& js, kj::Maybe<kj::String> str) {
    KJ_IF_SOME(s, str) {
      return s.size();
    } else {
      return -1;
    }
  }

  int32_t unwrapOptional(jsg::Lock& js, jsg::Optional<kj::String> str) {
    KJ_IF_SOME(s, str) {
      return s.size();
    } else {
      return -1;
    }
  }

  int32_t unwrapLenientOptional(jsg::Lock& js, jsg::LenientOptional<kj::String> str) {
    KJ_IF_SOME(s, str) {
      return s.size();
    } else {
      return -1;
    }
  }

  jsg::Ref<StaticMethodContainer> newContainer(jsg::Lock& js) {
    return js.alloc<StaticMethodContainer>();
  }

  void substituteCppgcShim(
      jsg::Lock& js, v8::Local<v8::Object> target, v8::Local<v8::Object> source) {
    substituteCppgcShimForTest(js.v8Isolate, target, source);
  }

  void resetCallCounter() {
    jsg::callCounter.reset();
  }

  JSG_RESOURCE_TYPE(FastMethodContext) {
    JSG_NESTED_TYPE(StaticMethodContainer);

    JSG_METHOD(add);
    JSG_METHOD(processValue);
    JSG_METHOD(processObject);
    JSG_METHOD(voidMethod);
    JSG_METHOD(throwError);
    JSG_METHOD(addWithLock);
    JSG_METHOD(constAdd);
    JSG_METHOD(constAddWithLock);
    JSG_METHOD(unwrapStruct);
    JSG_METHOD(unwrapUint);
    JSG_METHOD(unwrapString);
    JSG_METHOD(unwrapMaybe);
    JSG_METHOD(unwrapOptional);
    JSG_METHOD(unwrapLenientOptional);

    JSG_METHOD(newContainer);
    JSG_METHOD(substituteCppgcShim);
    JSG_METHOD(resetCallCounter);
  }
};

JSG_DECLARE_DEBUG_ISOLATE_TYPE(
    FastMethodIsolate, FastMethodContext, WrappedInt, StaticMethodContainer);
using FastMethodEvaluator = Evaluator<FastMethodContext, FastMethodIsolate, JsgConfig>;

// Turbolev cannot yet emit Fast API calls, so use TurboFan to exercise the API under test.
jsg::V8System v8System({"--allow-natives-syntax", "--no-turbolev"});

struct Test {
  kj::LiteralStringConst expr;
  kj::LiteralStringConst expectedReturnType;
  kj::LiteralStringConst expectedReturnValue;
  kj::LiteralStringConst target = ""_kjc;
  kj::uint expectedSlowCount = 1;
};

CallCounter runTest(Test test) {
  jsg::callCounter.reset();
  JsgConfig config = {
    .fastApiEnabled = true,
  };
  jsg::test::Evaluator<FastMethodContext, FastMethodIsolate, JsgConfig> e(v8System, config);

  auto target = test.target == ""_kjc ? kj::str(test.expr) : kj::str(test.target, ".", test.expr);
  e.expectEval(target, test.expectedReturnType, test.expectedReturnValue);
  KJ_ASSERT(jsg::callCounter == CallCounter(test.expectedSlowCount, 0));

  e.expectEval(kj::str("const fastCall = () => { return ", target,
                   "; }; "
                   "%PrepareFunctionForOptimization(fastCall); "
                   "fastCall(); "
                   "%OptimizeFunctionOnNextCall(fastCall); "
                   "fastCall()"),
      test.expectedReturnType, test.expectedReturnValue);
  return jsg::callCounter;
}

void runSubstitutedFastDispatch() {
  JsgConfig config = {
    .fastApiEnabled = true,
  };
  FastMethodEvaluator e(v8System, config);
  e.expectEval(R"(
    const target = newContainer();
    const source = newContainer();
    const fastCall = () => target.value;
    %PrepareFunctionForOptimization(fastCall);
    fastCall();
    %OptimizeFunctionOnNextCall(fastCall);
    fastCall();
    substituteCppgcShim(target, source);
    resetCallCounter();
    fastCall();
  )",
      "number", "42");
}

KJ_TEST("v8::Local<v8::Value> and v8::Local<v8::Object> as fast method parameters") {
  util::Autogate::initAutogateForTest({util::AutogateKey::V8_FAST_API});
  KJ_ASSERT(runTest({"processValue(42)"_kjc, "number"_kjc, "42"_kjc}) == CallCounter(2, 1));
  KJ_ASSERT(
      runTest({"processObject({test: 123})"_kjc, "number"_kjc, "123"_kjc}) == CallCounter(2, 1));
}

#ifdef __linux__
// Needs mmap.
KJ_TEST("fast dispatch rejects substituted wrapper identity") {
  util::Autogate::initAutogateForTest({util::AutogateKey::V8_FAST_API});
  auto* entered = static_cast<volatile sig_atomic_t*>(mmap(
      nullptr, sizeof(sig_atomic_t), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0));
  KJ_REQUIRE(entered != MAP_FAILED);
  *entered = 0;
  KJ_DEFER(KJ_SYSCALL(munmap(const_cast<sig_atomic_t*>(entered), sizeof(*entered))));
  fastCallbackEntered = entered;
  KJ_DEFER(fastCallbackEntered = nullptr);

  KJ_EXPECT_SIGNAL(SIGABRT, {
    signal(SIGABRT, recordFastCallback);
    runSubstitutedFastDispatch();
  });
  KJ_EXPECT(*entered == 1, "substituted invocation did not enter the Fast API callback");
}
#endif  // __linux__

KJ_TEST("Lock& as the first parameter in fast method calls") {
  KJ_ASSERT(runTest({"addWithLock(3, 4)"_kjc, "number"_kjc, "7"_kjc}) == CallCounter(2, 1));
}

KJ_TEST("Const methods in fast method calls") {
  KJ_ASSERT(runTest({"constAdd(3, 4)"_kjc, "number"_kjc, "7"_kjc}) == CallCounter(2, 1));
}

KJ_TEST("Const methods with Lock& in fast method calls") {
  KJ_ASSERT(runTest({"constAddWithLock(3, 4)"_kjc, "number"_kjc, "7"_kjc}) == CallCounter(2, 1));
}

KJ_TEST("type unwrapping arguments") {
  KJ_ASSERT(runTest({"unwrapUint(4)"_kjc, "number"_kjc, "4"_kjc}) == CallCounter(2, 1));
  KJ_ASSERT(runTest({"unwrapStruct({i: 3})"_kjc, "number"_kjc, "3"_kjc}) == CallCounter(2, 1));
  KJ_ASSERT(runTest({"unwrapString('0123')"_kjc, "number"_kjc, "4"_kjc}) == CallCounter(2, 1));
  KJ_ASSERT(runTest({"unwrapMaybe(undefined)"_kjc, "number"_kjc, "-1"_kjc}) == CallCounter(2, 1));
  KJ_ASSERT(runTest({"unwrapMaybe('foo')"_kjc, "number"_kjc, "3"_kjc}) == CallCounter(2, 1));
  KJ_ASSERT(
      runTest({"unwrapOptional(undefined)"_kjc, "number"_kjc, "-1"_kjc}) == CallCounter(2, 1));
  KJ_ASSERT(runTest({"unwrapOptional('foo')"_kjc, "number"_kjc, "3"_kjc}) == CallCounter(2, 1));
  KJ_ASSERT(runTest({"unwrapLenientOptional(undefined)"_kjc, "number"_kjc, "-1"_kjc}) ==
      CallCounter(2, 1));
  KJ_ASSERT(
      runTest({"unwrapLenientOptional('foo')"_kjc, "number"_kjc, "3"_kjc}) == CallCounter(2, 1));

  KJ_ASSERT(runTest({"StaticMethodContainer.staticMethod()"_kjc, "number"_kjc, "42"_kjc}) ==
      CallCounter(2, 1));
}

KJ_TEST("Fast methods should work with getters/setters") {
  KJ_ASSERT(
      runTest({"value"_kjc, "number"_kjc, "42"_kjc, "newContainer()"_kjc, 2}) == CallCounter(5, 1));
  KJ_ASSERT(runTest({"value = 12"_kjc, "number"_kjc, "12"_kjc, "newContainer()"_kjc, 2}) ==
      CallCounter(5, 1));
}

// Runs `body` hot in an optimized function and expects it to take the fast path.
void expectFastEval(kj::StringPtr params,
    kj::StringPtr body,
    kj::StringPtr warmup,
    kj::StringPtr args,
    kj::StringPtr expectedType,
    kj::StringPtr expectedValue) {
  jsg::callCounter.reset();
  JsgConfig config = {
    .fastApiEnabled = true,
  };
  FastMethodEvaluator e(v8System, config);
  e.expectEval(kj::str("const t = newContainer();"
                       "function hot(",
                   params, ") { ", body,
                   " }"
                   "%PrepareFunctionForOptimization(hot);"
                   "hot(",
                   warmup,
                   ");"
                   "%OptimizeFunctionOnNextCall(hot);"
                   "hot(",
                   args, ")"),
      expectedType, expectedValue);
  KJ_EXPECT(jsg::callCounter.fast > 0, body);
}

KJ_TEST("all-primitive fast path on a non-context receiver") {
  expectFastEval("a, b", "return t.addPrimitive(a, b);", "1, 2", "3, 4", "number", "7");
}

KJ_TEST("all-primitive fast path reports errors") {
  expectFastEval("code", "return t.throwPrimitive(code);", "0", "7", "throws",
      "TypeError: Primitive error with code 7");
}

KJ_TEST("two string arguments through the no-scope fast path") {
  // The first transcode must not invalidate the second argument's pointer (#4625).
  expectFastEval(
      "a, b", "return t.measureTwo(a, b);", "'x', 'y'", "'hello', 'worldwide'", "number", "14");
}

KJ_TEST("Lock& method with a string argument through the no-scope fast path") {
  expectFastEval("s",
      "let n = 0;"
      "for (let i = 0; i < 1000; i++) n += t.measureWithLock(s);"
      "return n;",
      "'x'", "'hello'", "number", "5000");
}

KJ_TEST("Fast methods properly catch JSG_FAIL_REQUIRE errors") {
  jsg::callCounter.reset();
  JsgConfig config = {
    .fastApiEnabled = true,
  };
  jsg::test::Evaluator<FastMethodContext, FastMethodIsolate, JsgConfig> e(v8System, config);

  // Test that directly calling the method results in an error
  e.expectEval("throwError(42)", "throws", "TypeError: Test error with code 42");
  KJ_ASSERT(jsg::callCounter == CallCounter(1, 0));

  // First run a non-throwing call to allow optimization
  e.expectEval("throwError(0)", "number", "0");
  KJ_ASSERT(jsg::callCounter == CallCounter(2, 0));

  e.expectEval("function fastThrow(code) { return throwError(code); }; "
               "%PrepareFunctionForOptimization(fastThrow); "
               "fastThrow(0); "
               "%OptimizeFunctionOnNextCall(fastThrow); "
               "fastThrow(42)",
      "throws", "TypeError: Test error with code 42");

  // The counts should now include both the slow path and fast path calls
  // 3 slow path calls (original direct call + non-throwing call + the initial optimization call that didn't throw)
  // 1 fast path call (the optimized call that threw an error)
  KJ_ASSERT(jsg::callCounter == CallCounter(3, 1));
}

// Every shape that is not a sequential latin1 string must fall back instead of being reinterpreted.
KJ_TEST("all string shapes agree between fast and slow paths") {
  struct Case {
    kj::LiteralStringConst expr;
    kj::LiteralStringConst utf8Length;
    kj::LiteralStringConst description;
    bool expectFastPath;
  };

  static constexpr Case cases[] = {
    {"'abc'"_kjc, "3"_kjc, "ascii"_kjc, true},
    {"'ab\\u00e9'"_kjc, "4"_kjc, "latin1 needing transcode"_kjc, true},
    {"'a\\u0000b'"_kjc, "3"_kjc, "embedded NUL must not truncate"_kjc, true},
    {"('ab' + 'cd')"_kjc, "4"_kjc, "cons string, flattened by V8"_kjc, true},
    {"'\\u65e5\\u672c\\u8a9e'"_kjc, "9"_kjc, "two-byte CJK"_kjc, false},
    {"'\\ud83d\\ude00'"_kjc, "4"_kjc, "surrogate pair"_kjc, false},
    {"'a\\u65e5b'"_kjc, "5"_kjc, "mixed one and two byte"_kjc, false},
    {"123"_kjc, "3"_kjc, "number coerced"_kjc, false},
    {"({toString() { return 'xyz'; }})"_kjc, "3"_kjc, "object with toString"_kjc, false},
    {"null"_kjc, "4"_kjc, "null coerced"_kjc, false},
  };

  for (auto& c: cases) {
    JsgConfig fastConfig = {
      .fastApiEnabled = true,
    };
    JsgConfig slowConfig = {
      .fastApiEnabled = false,
    };

    auto source = kj::str("const t = newContainer();"
                          "function hot(s) { return t.measure(s); }"
                          "%PrepareFunctionForOptimization(hot);"
                          "hot('seed');"
                          "%OptimizeFunctionOnNextCall(hot);"
                          "hot(",
        c.expr, ")");

    {
      FastMethodEvaluator e(v8System, slowConfig);
      e.expectEval(source, "number", c.utf8Length);
    }
    {
      jsg::callCounter.reset();
      FastMethodEvaluator e(v8System, fastConfig);
      e.expectEval(source, "number", c.utf8Length);
      if (c.expectFastPath) {
        KJ_EXPECT(jsg::callCounter.fast > 0, c.description, "should have used the fast callback");
      } else {
        KJ_EXPECT(jsg::callCounter.fast == 0, c.description,
            "must fall back to the slow callback rather than reinterpreting the argument");
      }
    }
  }
}

KJ_TEST("static methods through the no-scope fast path") {
  expectFastEval(
      "a, b", "return StaticMethodContainer.staticAdd(a, b);", "1, 2", "3, 4", "number", "7");
  expectFastEval(
      "s", "return StaticMethodContainer.staticMeasure(s);", "'x'", "'h\\u00e9llo'", "number", "6");
  expectFastEval("s",
      "let n = 0;"
      "for (let i = 0; i < 1000; i++) n += StaticMethodContainer.staticMeasureWithLock(s);"
      "return n;",
      "'x'", "'hello'", "number", "5000");
}

KJ_TEST("property getters and setters through the no-scope fast path") {
  expectFastEval("", "return t.label;", "", "", "number", "0");
  expectFastEval("", "return t.lockedLabel;", "", "", "number", "0");
  expectFastEval("s", "t.label = s; return t.label;", "'x'", "'h\\u00e9llo'", "number", "6");
  expectFastEval("s",
      "let n = 0;"
      "for (let i = 0; i < 1000; i++) { t.lockedLabel = s; n += t.lockedLabel; }"
      "return n;",
      "'x'", "'hello'", "number", "5000");
}

KJ_TEST("isFastApiNoScope Detection") {
  static_assert(isFastApiNoScope<decltype(&StaticMethodContainer::addPrimitive)>);
  static_assert(isFastApiNoScope<decltype(&StaticMethodContainer::measure)>);
  static_assert(isFastApiNoScope<decltype(&StaticMethodContainer::measureTwo)>);
  static_assert(isFastApiNoScope<decltype(&StaticMethodContainer::measureWithLock)>);

  static_assert(isFastApiNoScope<void (FastMethodContext::*)(jsg::Lock&, kj::String, kj::String)>);

  // Static methods and properties arrive as plain function pointers.
  static_assert(isFastApiNoScope<uint32_t (*)(uint32_t)>);
  static_assert(isFastApiNoScope<void (*)(jsg::Lock&, kj::String)>);
  static_assert(!isFastApiNoScope<kj::String (*)(int32_t)>);

  // Compatible but not no-scope: the argument still needs unwrapping.
  static_assert(isFastApiCompatible<int32_t (FastMethodContext::*)(v8::Local<v8::Value>)>);
  static_assert(!isFastApiNoScope<int32_t (FastMethodContext::*)(v8::Local<v8::Value>)>);

  static_assert(!isFastApiNoScope<kj::String (FastMethodContext::*)(int32_t)>);
}

KJ_TEST("isFastApiCompatible Detection") {
  static_assert(isFastApiCompatible<void (FastMethodContext::*)(jsg::Lock&, bool)>,
      "lock is accepted only as first argument");

  // Method type declarations
  // -----------------------

  // Compatible basic types
  using VoidMethod = void (FastMethodContext::*)();
  using IntMethod = int32_t (FastMethodContext::*)(int32_t);
  using BoolMethod = bool (FastMethodContext::*)(double, bool);
  using FloatMethod = float (FastMethodContext::*)(int32_t, float);

  // V8 Local types as parameters - should be compatible
  using V8ValueParamMethod = int32_t (FastMethodContext::*)(v8::Local<v8::Value>);
  using V8ObjectParamMethod = int32_t (FastMethodContext::*)(v8::Local<v8::Object>);

  // Const method variants - should be compatible
  using ConstIntMethod = int32_t (FastMethodContext::*)(int32_t) const;
  using ConstFloatMethod = float (FastMethodContext::*)(float) const;

  // Methods with Lock& as first parameter - should be compatible
  using LockFirstMethod = int32_t (FastMethodContext::*)(jsg::Lock&, int32_t);
  using LockFirstVoidMethod = void (FastMethodContext::*)(jsg::Lock&, bool);
  using LockFirstWithV8Local = int32_t (FastMethodContext::*)(jsg::Lock&, v8::Local<v8::Value>);
  using ConstLockFirstMethod = int32_t (FastMethodContext::*)(jsg::Lock&, int32_t) const;

  // ---- Non-compatible method types ----

  // Pointer types - not compatible
  using PointerParamMethod = void* (FastMethodContext::*)(void*);
  using PointerReturnMethod = void* (FastMethodContext::*)();

  // V8 Local as return type - not compatible
  using V8ReturnMethod = v8::Local<v8::Value> (FastMethodContext::*)(int32_t);

  // Non-primitive types - not compatible
  using StringMethod = kj::String (FastMethodContext::*)(kj::String);
  using ComplexMethod = Ref<FastMethodContext> (FastMethodContext::*)(jsg::Lock&);
  using KjArrayMethod = kj::Array<int> (FastMethodContext::*)(int32_t);
  using PromiseMethod = jsg::Promise<int> (FastMethodContext::*)(int32_t);
  using MaybeVoidMethod = kj::Maybe<void> (FastMethodContext::*)();
  using StaticMethodContainerMethod = void(StaticMethodContainer);
  using KjPromiseMethod = void (FastMethodContext::*)(kj::Promise<void>);
  using JsgPromiseMethod = void (FastMethodContext::*)(jsg::Promise<void>);

  // Static assertions for compatible method types
  // --------------------------------------------

  // Basic primitives
  static_assert(isFastApiCompatible<VoidMethod>, "Void methods should be fast-method compatible");
  static_assert(isFastApiCompatible<IntMethod>, "Integer methods should be fast-method compatible");
  static_assert(
      isFastApiCompatible<BoolMethod>, "Boolean methods should be fast-method compatible");
  static_assert(isFastApiCompatible<FloatMethod>, "Float methods should be fast-method compatible");

  // V8 Local parameters
  static_assert(isFastApiCompatible<V8ValueParamMethod>,
      "Methods with v8::Local<v8::Value> parameters should be fast-method compatible");
  static_assert(isFastApiCompatible<V8ObjectParamMethod>,
      "Methods with v8::Local<v8::Object> parameters should be fast-method compatible");

  // Const methods
  static_assert(
      isFastApiCompatible<ConstIntMethod>, "Const methods should be fast-method compatible");
  static_assert(isFastApiCompatible<ConstFloatMethod>,
      "Const methods with float should be fast-method compatible");

  // Lock& as first parameter
  static_assert(isFastApiCompatible<LockFirstMethod>,
      "Methods with Lock& as first parameter should be fast-method compatible");
  static_assert(isFastApiCompatible<LockFirstVoidMethod>,
      "Void methods with Lock& as first parameter should be fast-method compatible");
  static_assert(isFastApiCompatible<LockFirstWithV8Local>,
      "Methods with Lock& and v8::Local params should be fast-method compatible");
  static_assert(isFastApiCompatible<ConstLockFirstMethod>,
      "Const methods with Lock& as first parameter should be fast-method compatible");

  // Static assertions for incompatible method types
  // ----------------------------------------------

  // Pointer types
  static_assert(!isFastApiCompatible<PointerParamMethod>,
      "Methods with pointer parameters should not be fast-method compatible");
  static_assert(!isFastApiCompatible<PointerReturnMethod>,
      "Methods returning pointers should not be fast-method compatible");

  // V8 Local return type
  static_assert(!isFastApiCompatible<V8ReturnMethod>,
      "Methods returning v8::Local<v8::Value> should not be fast-method compatible");

  // Complex types
  static_assert(!isFastApiCompatible<StringMethod>,
      "Methods with kj::String parameters should not be fast-method compatible");
  static_assert(!isFastApiCompatible<ComplexMethod>,
      "Methods with Ref return types should not be fast-method compatible");
  static_assert(!isFastApiCompatible<KjArrayMethod>,
      "Methods returning kj::Array should not be fast-method compatible");
  static_assert(!isFastApiCompatible<PromiseMethod>,
      "Methods returning Promise should not be fast-method compatible");
  static_assert(!isFastApiCompatible<MaybeVoidMethod>,
      "Methods returning Maybe<void> should not be fast-method compatible");
  static_assert(isFastApiCompatible<StaticMethodContainerMethod>, "This should be compatible");
  static_assert(!isFastApiCompatible<KjPromiseMethod>, "kj::Promise is not compatible");
  static_assert(!isFastApiCompatible<JsgPromiseMethod>, "jsg::Promise is not compatible");
}

}  // namespace
}  // namespace workerd::jsg::test
