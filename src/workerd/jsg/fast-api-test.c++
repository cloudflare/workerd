#include "jsg-test.h"

#include <workerd/util/autogate.h>

namespace workerd::jsg::test {
namespace {

using jsg::CallCounter;

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

  JSG_RESOURCE_TYPE(StaticMethodContainer) {
    JSG_STATIC_METHOD(staticMethod);
    JSG_PROTOTYPE_PROPERTY(value, getValue, setValue);
  }

 private:
  uint32_t value = 42;
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

  int32_t unwrapBufferSource(jsg::Lock& js, jsg::BufferSource source) {
    return source.size();
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
    JSG_METHOD(unwrapBufferSource);
    JSG_METHOD(unwrapMaybe);
    JSG_METHOD(unwrapOptional);
    JSG_METHOD(unwrapLenientOptional);

    JSG_METHOD(newContainer);
  }
};

JSG_DECLARE_DEBUG_ISOLATE_TYPE(
    FastMethodIsolate, FastMethodContext, WrappedInt, StaticMethodContainer);

jsg::V8System v8System({"--allow-natives-syntax"});

struct Test {
  kj::LiteralStringConst expr;
  kj::LiteralStringConst expectedReturnType;
  kj::LiteralStringConst expectedReturnValue;
  kj::LiteralStringConst target = ""_kjc;
  kj::uint expectedSlowCount = 1;
};

CallCounter runTest(Test test) {
  jsg::callCounter.reset();
  jsg::test::Evaluator<FastMethodContext, FastMethodIsolate> e(v8System);

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

KJ_TEST("v8::Local<v8::Value> and v8::Local<v8::Object> as fast method parameters") {
  util::Autogate::initAutogateNamesForTest({"v8-fast-api"_kj});
  KJ_ASSERT(runTest({"processValue(42)"_kjc, "number"_kjc, "42"_kjc}) == CallCounter(2, 1));
  KJ_ASSERT(
      runTest({"processObject({test: 123})"_kjc, "number"_kjc, "123"_kjc}) == CallCounter(2, 1));
}

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
  KJ_ASSERT(runTest({"unwrapBufferSource(new Uint8Array(256))"_kjc, "number"_kjc, "256"_kjc}) ==
      CallCounter(2, 1));
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

KJ_TEST("Fast methods properly catch JSG_FAIL_REQUIRE errors") {
  jsg::callCounter.reset();
  jsg::test::Evaluator<FastMethodContext, FastMethodIsolate> e(v8System);

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
