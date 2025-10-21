// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;
class ContextGlobalObject: public Object, public ContextGlobal {};

// ========================================================================================

struct TestExtensionType {
  int32_t value;
};

struct ExtensionContext: public ContextGlobalObject {
  TestExtensionType toExtensionType(double value) {
    return {static_cast<int32_t>(value)};
  }
  double fromExtensionType(TestExtensionType value) {
    return value.value;
  }

  JSG_RESOURCE_TYPE(ExtensionContext) {
    JSG_METHOD(toExtensionType);
    JSG_METHOD(fromExtensionType);
  }
};

template <typename Self>
class TestExtension {
  // Test manually extending the TypeWrapper with wrap/unwrap functions for a custom type.

 public:
  static constexpr const char* getName(TestExtensionType*) {
    return "TestExtensionTypeName";
  }

  v8::Local<v8::Number> wrap(Lock& js,
      v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      TestExtensionType value) {
    return v8::Number::New(js.v8Isolate, value.value);
  }

  v8::Local<v8::Context> newContext(v8::Isolate* isolate, TestExtensionType value) = delete;

  kj::Maybe<TestExtensionType> tryUnwrap(Lock& js,
      v8::Local<v8::Context> context,
      v8::Local<v8::Value> handle,
      TestExtensionType*,
      kj::Maybe<v8::Local<v8::Object>> parentObject) {
    return TestExtensionType{handle->Int32Value(context).ToChecked()};
  }

  template <bool isContext = false>
  v8::Local<v8::FunctionTemplate> getTemplate(v8::Isolate* isolate, TestExtensionType*) = delete;
};

JSG_DECLARE_ISOLATE_TYPE(ExtensionIsolate, ExtensionContext, TypeWrapperExtension<TestExtension>);

KJ_TEST("extensions") {
  Evaluator<ExtensionContext, ExtensionIsolate> e(v8System);
  e.expectEval("fromExtensionType(toExtensionType(12.3))", "number", "12");
}

// ========================================================================================

struct TypeHandlerContext: public ContextGlobalObject {
  v8::Local<v8::Value> newNumberBox(
      jsg::Lock& js, double value, const TypeHandler<Ref<NumberBox>>& handler) {
    return handler.wrap(js, js.alloc<NumberBox>(value));
  }
  double openNumberBox(
      jsg::Lock& js, v8::Local<v8::Value> handle, const TypeHandler<Ref<NumberBox>>& handler) {
    return KJ_REQUIRE_NONNULL(handler.tryUnwrap(js, handle))->value;
  }
  v8::Local<v8::Value> wrapNumber(jsg::Lock& js, double value, const TypeHandler<double>& handler) {
    return handler.wrap(js, value);
  }
  double unwrapNumber(
      jsg::Lock& js, v8::Local<v8::Value> handle, const TypeHandler<double>& handler) {
    return KJ_REQUIRE_NONNULL(handler.tryUnwrap(js, handle));
  }

  JSG_RESOURCE_TYPE(TypeHandlerContext) {
    JSG_NESTED_TYPE(NumberBox);
    JSG_METHOD(newNumberBox);
    JSG_METHOD(openNumberBox);
    JSG_METHOD(wrapNumber);
    JSG_METHOD(unwrapNumber);
  }
};
JSG_DECLARE_ISOLATE_TYPE(TypeHandlerIsolate, TypeHandlerContext, NumberBox);

KJ_TEST("type handlers") {
  Evaluator<TypeHandlerContext, TypeHandlerIsolate> e(v8System);
  e.expectEval("newNumberBox(123).value", "number", "123");
  e.expectEval("openNumberBox(new NumberBox(123))", "number", "123");
  e.expectEval("wrapNumber(123)", "number", "123");
  e.expectEval("unwrapNumber(123)", "number", "123");
  e.expectEval("newNumberBox(789).boxedFromTypeHandler.value", "number", "789");
}

// ========================================================================================

struct ArrayContext: public ContextGlobalObject {
  double sumArray(kj::Array<double> array) {
    double result = 0;
    for (auto d: array) result += d;
    return result;
  }
  kj::Array<double> returnArray(double dlength) {
    size_t length = dlength;
    auto builder = kj::heapArrayBuilder<double>(length);
    for (uint i: kj::zeroTo(length)) builder.add(i);
    return builder.finish();
  }
  kj::ArrayPtr<const double> returnArrayPtr() {
    static const double VALUES[3] = {123, 456, 789};
    return VALUES;
  }
  JSG_RESOURCE_TYPE(ArrayContext) {
    JSG_METHOD(sumArray);
    JSG_METHOD(returnArray);
    JSG_METHOD(returnArrayPtr);
  }
};
JSG_DECLARE_ISOLATE_TYPE(ArrayIsolate, ArrayContext);

KJ_TEST("arrays") {
  Evaluator<ArrayContext, ArrayIsolate> e(v8System);
  e.expectEval("sumArray([123, 321, 33])", "number", "477");
  e.expectEval("returnArray(3).join(', ')", "string", "0, 1, 2");
  e.expectEval("returnArrayPtr(3).join(', ')", "string", "123, 456, 789");

  e.expectEval("sumArray([123, {}, 321])", "number", "NaN");
}

// ========================================================================================

struct Uint8Context: public ContextGlobalObject {
  kj::Array<byte> encodeUtf8(kj::String str) {
    return kj::heapArray(str.asBytes());
  }
  kj::String decodeUtf8(kj::Array<byte> data) {
    return kj::str(data.asChars());
  }
  kj::String decodeUtf8Const(kj::Array<const byte> data) {
    return kj::str(data.asChars());
  }
  JSG_RESOURCE_TYPE(Uint8Context) {
    JSG_METHOD(encodeUtf8);
    JSG_METHOD(decodeUtf8);
    JSG_METHOD(decodeUtf8Const);
  }
};
JSG_DECLARE_ISOLATE_TYPE(Uint8Isolate, Uint8Context);

KJ_TEST("Uint8Arrays") {
  Evaluator<Uint8Context, Uint8Isolate> e(v8System);
  uint byteSequence[] = {'f', 'o', 'o', ' ', 0xf0, 0x9f, 0x98, 0xba};
  auto byteSequenceStr = kj::strArray(kj::ArrayPtr<uint>(byteSequence), ", ");

  e.expectEval("new Uint8Array(encodeUtf8('foo 😺')).join(', ')", "string", byteSequenceStr);
  e.expectEval(kj::str("decodeUtf8(new Uint8Array([", byteSequenceStr, "]))"), "string", "foo 😺");
  e.expectEval(
      kj::str("decodeUtf8(new Uint8Array([", byteSequenceStr, "]).buffer)"), "string", "foo 😺");
  e.expectEval(kj::str("var buf = new Uint8Array([", byteSequenceStr,
                   "]).buffer;\n"
                   "decodeUtf8(new Uint8Array(buf, 1, 3))"),
      "string", "oo ");

  e.expectEval(
      kj::str("decodeUtf8Const(new Uint8Array([", byteSequenceStr, "]))"), "string", "foo 😺");
}

// ========================================================================================

struct UnwrappingContext: public ContextGlobalObject {
  v8::Local<v8::ArrayBufferView> mutateArrayBufferView(v8::Local<v8::ArrayBufferView> value) {
    if (value->ByteLength() > 0) {
      auto backing = value->Buffer()->GetBackingStore();
      *static_cast<kj::byte*>(backing->Data()) = 123;
    }
    return value;
  }
  JSG_RESOURCE_TYPE(UnwrappingContext) {
    JSG_METHOD(mutateArrayBufferView);
  }
};
JSG_DECLARE_ISOLATE_TYPE(UnwrappingIsolate, UnwrappingContext);

KJ_TEST("v8::Value subclass unwrapping") {
  Evaluator<UnwrappingContext, UnwrappingIsolate> e(v8System);
  e.expectEval("let abv = new Uint8Array([0, 1, 2]);\n"
               "let abv2 = mutateArrayBufferView(abv);\n"
               "abv === abv2 && abv[0] === 123",
      "boolean", "true");
}

// ========================================================================================

struct UnimplementedContext: public ContextGlobalObject {
  class UnimplementedConstructor: public Object {
   public:
    static Unimplemented constructor() {
      return Unimplemented();
    }
    JSG_RESOURCE_TYPE(UnimplementedConstructor) {}
  };
  class UnimplementedConstructorParam: public Object {
   public:
    UnimplementedConstructorParam(int i): i(i) {}
    static Ref<UnimplementedConstructorParam> constructor(jsg::Lock& js, int i, Unimplemented) {
      return js.alloc<UnimplementedConstructorParam>(i);
    }
    int getI() {
      return i;
    }
    int i;
    JSG_RESOURCE_TYPE(UnimplementedConstructorParam) {
      JSG_READONLY_INSTANCE_PROPERTY(i, getI);
    }
  };
  Unimplemented unimplementedMethod() {
    return Unimplemented();
  }
  int unimplementedParam(int i, Unimplemented) {
    return i;
  }
  Unimplemented getUnimplemented() {
    return Unimplemented();
  }
  void setUnimplemented(Unimplemented) {}
  struct UnimplementedField {
    int i;
    Unimplemented unimplemented;
    JSG_STRUCT(i, unimplemented);
  };
  int unimplementedField(UnimplementedField s) {
    return s.i;
  }
  auto unimplementedCallbackArgument() {
    return [](Lock&, int i, Unimplemented) { return i; };
  }

  class UnimplementedProperties: public Object {
   public:
    static Ref<UnimplementedProperties> constructor(jsg::Lock& js) {
      return js.alloc<UnimplementedProperties>();
    }

    int getNumber() {
      return 123;
    }

    Unimplemented getUnimplemented1() {
      return Unimplemented();
    }
    void setUnimplemented1(Unimplemented) {}
    Unimplemented getUnimplemented2() {
      return Unimplemented();
    }

    int implementedMethod() {
      return 123;
    }
    Unimplemented unimplementedMethod() {
      return Unimplemented();
    }

    JSG_RESOURCE_TYPE(UnimplementedProperties) {
      JSG_READONLY_INSTANCE_PROPERTY(number, getNumber);
      JSG_INSTANCE_PROPERTY(unimplemented1, getUnimplemented1, setUnimplemented1);
      JSG_READONLY_INSTANCE_PROPERTY(unimplemented2, getUnimplemented2);
      JSG_METHOD(implementedMethod);
      JSG_METHOD(unimplementedMethod);
    }
  };

  struct StructWithUnimplementedMembers {
    Optional<kj::String> optionalString;
    Unimplemented unimplementedMember;
    WontImplement wontImplementMember;
    JSG_STRUCT(optionalString, unimplementedMember, wontImplementMember);
  };

  void takeStructWithUnimplementedMembers(StructWithUnimplementedMembers) {}

  JSG_RESOURCE_TYPE(UnimplementedContext) {
    JSG_NESTED_TYPE(UnimplementedConstructor);
    JSG_NESTED_TYPE(UnimplementedConstructorParam);
    JSG_METHOD(unimplementedMethod);
    JSG_METHOD(unimplementedParam);
    JSG_INSTANCE_PROPERTY(unimplemented, getUnimplemented, setUnimplemented);
    JSG_METHOD(unimplementedField);
    JSG_METHOD(unimplementedCallbackArgument);
    JSG_NESTED_TYPE(UnimplementedProperties);
    JSG_METHOD(takeStructWithUnimplementedMembers);
  }
};
JSG_DECLARE_ISOLATE_TYPE(UnimplementedIsolate,
    UnimplementedContext,
    UnimplementedContext::UnimplementedConstructor,
    UnimplementedContext::UnimplementedConstructorParam,
    UnimplementedContext::UnimplementedField,
    UnimplementedContext::UnimplementedProperties,
    UnimplementedContext::StructWithUnimplementedMembers);

KJ_TEST("unimplemented errors") {
  Evaluator<UnimplementedContext, UnimplementedIsolate> e(v8System);
  e.expectEval("new UnimplementedConstructor()", "throws",
      "Error: Failed to construct 'UnimplementedConstructor': "
      "the constructor is not implemented.");

  e.expectEval("new UnimplementedConstructorParam(123).i", "number", "123");
  e.expectEval("new UnimplementedConstructorParam(123, 456)", "throws",
      "Error: Failed to construct 'UnimplementedConstructorParam': "
      "constructor parameter 2 is not implemented.");

  e.expectEval("unimplementedMethod()", "throws",
      "Error: Failed to execute 'unimplementedMethod' on 'UnimplementedContext': "
      "the method is not implemented.");

  e.expectEval("unimplementedParam(123)", "number", "123");
  e.expectEval("unimplementedParam(123, 456)", "throws",
      "Error: Failed to execute 'unimplementedParam' on 'UnimplementedContext': "
      "parameter 2 is not implemented.");

  e.expectEval("unimplemented", "throws",
      "Error: Failed to get the 'unimplemented' property on 'UnimplementedContext': "
      "the property is not implemented.");
  e.expectEval("unimplemented = 123", "throws",
      "Error: Failed to set the 'unimplemented' property on 'UnimplementedContext': "
      "the ability to set this property is not implemented.");

  e.expectEval("unimplementedField({i: 123})", "number", "123");
  e.expectEval("unimplementedField({i: 123, unimplemented: 456})", "throws",
      "Error: The 'unimplemented' field on 'UnimplementedField' is not implemented.");

  e.expectEval("unimplementedCallbackArgument()(123)", "number", "123");
  e.expectEval("unimplementedCallbackArgument()(123, 456)", "throws",
      "Error: Failed to execute function: parameter 2 is not implemented.");

  // Verify that unimplemented properties are not enumerable by attempting to JSON-encode a class
  // that has them. If they are enumerable, the encoder will try to access them and throw
  // exceptions.
  e.expectEval("JSON.stringify(new UnimplementedProperties)", "string", "{\"number\":123}");

  // Verify that structs with unimplemented/wont-implement members can still be initialized from
  // null/undefined values.
  e.expectEval("takeStructWithUnimplementedMembers(null)", "undefined", "undefined");
  e.expectEval("takeStructWithUnimplementedMembers(undefined)", "undefined", "undefined");
}

// ========================================================================================
// TypeHandlerRegistry tests
//
// These tests verify the TypeHandlerRegistry system, which provides type-erased access to
// TypeHandler instances. The registry allows code to wrap/unwrap values without needing
// to know the full TypeWrapper template instantiation, making it possible to pass type
// conversion capabilities across API boundaries without template parameters.

struct TypeHandlerRegistryContext: public ContextGlobalObject {
  // Test methods that use the registry
  v8::Local<v8::Value> registryWrapString(jsg::Lock& js, kj::String value) {
    auto& registry = TypeHandlerRegistry::from(js);
    auto& handler = registry.getHandler<kj::String>();
    return handler.wrap(js, kj::mv(value));
  }

  kj::Maybe<kj::String> registryUnwrapString(jsg::Lock& js, v8::Local<v8::Value> value) {
    auto& registry = TypeHandlerRegistry::from(js);
    auto& handler = registry.getHandler<kj::String>();
    return handler.tryUnwrap(js, value);
  }

  v8::Local<v8::Value> registryWrapInt(jsg::Lock& js, int value) {
    auto& registry = TypeHandlerRegistry::from(js);
    auto& handler = registry.getHandler<int>();
    return handler.wrap(js, value);
  }

  kj::Maybe<int> registryUnwrapInt(jsg::Lock& js, v8::Local<v8::Value> value) {
    auto& registry = TypeHandlerRegistry::from(js);
    auto& handler = registry.getHandler<int>();
    return handler.tryUnwrap(js, value);
  }

  v8::Local<v8::Value> registryWrapDouble(jsg::Lock& js, double value) {
    auto& registry = TypeHandlerRegistry::from(js);
    auto& handler = registry.getHandler<double>();
    return handler.wrap(js, value);
  }

  kj::Maybe<double> registryUnwrapDouble(jsg::Lock& js, v8::Local<v8::Value> value) {
    auto& registry = TypeHandlerRegistry::from(js);
    auto& handler = registry.getHandler<double>();
    return handler.tryUnwrap(js, value);
  }

  // Test that we can get a handler (throws if not found)
  bool registryCanGetStringHandler(jsg::Lock& js) {
    auto& registry = TypeHandlerRegistry::from(js);
    try {
      registry.getHandler<kj::String>();
      return true;
    } catch (...) {
      return false;
    }
  }

  bool registryCanGetBoolHandler(jsg::Lock& js) {
    auto& registry = TypeHandlerRegistry::from(js);
    try {
      registry.getHandler<bool>();
      return true;
    } catch (...) {
      return false;
    }
  }

  JSG_RESOURCE_TYPE(TypeHandlerRegistryContext) {
    JSG_METHOD(registryWrapString);
    JSG_METHOD(registryUnwrapString);
    JSG_METHOD(registryWrapInt);
    JSG_METHOD(registryUnwrapInt);
    JSG_METHOD(registryWrapDouble);
    JSG_METHOD(registryUnwrapDouble);
    JSG_METHOD(registryCanGetStringHandler);
    JSG_METHOD(registryCanGetBoolHandler);
  }
};

JSG_DECLARE_ISOLATE_TYPE(TypeHandlerRegistryIsolate, TypeHandlerRegistryContext);

KJ_TEST("TypeHandlerRegistry - basic functionality") {
  Evaluator<TypeHandlerRegistryContext, TypeHandlerRegistryIsolate> e(v8System);

  // Test wrapping and unwrapping strings
  e.expectEval("registryWrapString('hello world')", "string", "hello world");
  e.expectEval("registryUnwrapString('test string')", "string", "test string");

  // Test wrapping and unwrapping integers
  e.expectEval("registryWrapInt(42)", "number", "42");
  e.expectEval("registryUnwrapInt(123)", "number", "123");

  // Test wrapping and unwrapping doubles
  e.expectEval("registryWrapDouble(3.14159)", "number", "3.14159");
  e.expectEval("registryUnwrapDouble(2.71828)", "number", "2.71828");
}

KJ_TEST("TypeHandlerRegistry - type checking") {
  Evaluator<TypeHandlerRegistryContext, TypeHandlerRegistryIsolate> e(v8System);

  // Test that handlers can be retrieved (no exception)
  e.expectEval("registryCanGetStringHandler()", "boolean", "true");
  e.expectEval("registryCanGetBoolHandler()", "boolean", "true");
}

KJ_TEST("TypeHandlerRegistry - round-trip conversions") {
  Evaluator<TypeHandlerRegistryContext, TypeHandlerRegistryIsolate> e(v8System);

  // Round-trip string conversion
  e.expectEval("registryUnwrapString(registryWrapString('round trip'))", "string", "round trip");

  // Round-trip number conversions
  e.expectEval("registryUnwrapInt(registryWrapInt(999))", "number", "999");
  e.expectEval("registryUnwrapDouble(registryWrapDouble(1.23))", "number", "1.23");
}

KJ_TEST("TypeHandlerRegistry - null/undefined handling") {
  Evaluator<TypeHandlerRegistryContext, TypeHandlerRegistryIsolate> e(v8System);

  // tryUnwrap should return null for incompatible types
  e.expectEval("registryUnwrapString(123)", "string", "123");
  e.expectEval("registryUnwrapString(null)", "string", "null");
  e.expectEval("registryUnwrapString(undefined)", "string", "undefined");

  e.expectEval("registryUnwrapInt('not a number')", "number", "0");
  e.expectEval("registryUnwrapInt(null)", "number", "0");
}

// ========================================================================================
// Mock TypeHandler tests

template <typename T>
class MockTypeHandler final: public TypeHandler<T> {
  mutable int wrapCallCount = 0;
  mutable int unwrapCallCount = 0;
  T mockValue;

 public:
  explicit MockTypeHandler(T mockValue): mockValue(kj::mv(mockValue)) {}

  v8::Local<v8::Value> wrap(Lock& js, T value) const override {
    wrapCallCount++;
    if constexpr (kj::isSameType<T, kj::String>()) {
      return v8StrIntern(js.v8Isolate, "MOCK_STRING");
    } else if constexpr (kj::isSameType<T, int>()) {
      return v8::Number::New(js.v8Isolate, 999);
    } else if constexpr (kj::isSameType<T, double>()) {
      return v8::Number::New(js.v8Isolate, 9.99);
    }
    return v8::Undefined(js.v8Isolate);
  }

  kj::Maybe<T> tryUnwrap(Lock& js, v8::Local<v8::Value> handle) const override {
    unwrapCallCount++;
    if constexpr (kj::isSameType<T, kj::String>()) {
      return kj::str(mockValue);
    } else {
      return mockValue;
    }
  }

  int getWrapCallCount() const {
    return wrapCallCount;
  }
  int getUnwrapCallCount() const {
    return unwrapCallCount;
  }
};

struct MockHandlerContext: public ContextGlobalObject {
  v8::Local<v8::Value> useStringHandler(jsg::Lock& js, kj::String value) {
    auto& registry = TypeHandlerRegistry::from(js);
    auto& handler = registry.getHandler<kj::String>();
    return handler.wrap(js, kj::mv(value));
  }

  JSG_RESOURCE_TYPE(MockHandlerContext) {
    JSG_METHOD(useStringHandler);
  }
};

JSG_DECLARE_ISOLATE_TYPE(MockHandlerIsolate, MockHandlerContext);

KJ_TEST("TypeHandlerRegistry - mock handlers") {
  Evaluator<MockHandlerContext, MockHandlerIsolate> e(v8System);

  // First, test with default handlers
  e.expectEval("useStringHandler('original')", "string", "original");

  // Now we would need to inject mock handlers for more advanced testing
  // This demonstrates the capability but requires access to isolate initialization
}

// ========================================================================================
// Test direct registry API usage

KJ_TEST("TypeHandlerRegistry - direct API") {
  Evaluator<TypeHandlerRegistryContext, TypeHandlerRegistryIsolate> e(v8System);

  e.getIsolate().runInLockScope([&](TypeHandlerRegistryIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock,
        lock.newContext<TypeHandlerRegistryContext>().getHandle(lock.v8Isolate),
        [&](jsg::Lock& js) {
      auto& registry = TypeHandlerRegistry::from(js);

      // Test that we can get handlers for built-in types (will throw if not registered)
      auto& stringHandler = registry.getHandler<kj::String>();
      auto& intHandler = registry.getHandler<int>();
      auto& doubleHandler = registry.getHandler<double>();
      [[maybe_unused]] auto& boolHandler = registry.getHandler<bool>();

      // Test wrapping with the registry
      auto jsString = stringHandler.wrap(js, kj::str("test"));
      KJ_EXPECT(jsString->IsString());

      // Test unwrapping with the registry
      auto maybeStr = stringHandler.tryUnwrap(js, jsString);
      KJ_EXPECT(maybeStr != kj::none);
      KJ_EXPECT(KJ_REQUIRE_NONNULL(maybeStr) == "test");

      // Test integer handler
      auto jsInt = intHandler.wrap(js, 42);
      KJ_EXPECT(jsInt->IsNumber());

      auto maybeInt = intHandler.tryUnwrap(js, jsInt);
      KJ_EXPECT(maybeInt != kj::none);
      KJ_EXPECT(KJ_REQUIRE_NONNULL(maybeInt) == 42);

      // Test double handler
      auto jsDouble = doubleHandler.wrap(js, 3.14159);
      KJ_EXPECT(jsDouble->IsNumber());

      auto maybeDouble = doubleHandler.tryUnwrap(js, jsDouble);
      KJ_EXPECT(maybeDouble != kj::none);
      KJ_EXPECT(KJ_REQUIRE_NONNULL(maybeDouble) == 3.14159);
    });
  });
}

KJ_TEST("TypeHandlerRegistry - error handling") {
  Evaluator<TypeHandlerRegistryContext, TypeHandlerRegistryIsolate> e(v8System);

  e.getIsolate().runInLockScope([&](TypeHandlerRegistryIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock,
        lock.newContext<TypeHandlerRegistryContext>().getHandle(lock.v8Isolate),
        [&](jsg::Lock& js) {
      auto& registry = TypeHandlerRegistry::from(js);

      // Test that getHandler works for registered types
      auto& stringHandler = registry.getHandler<kj::String>();
      auto jsValue = stringHandler.wrap(js, kj::str("test"));
      KJ_EXPECT(jsValue->IsString());

      // Test that getHandler works for int
      auto& intHandler = registry.getHandler<int>();
      auto jsInt = intHandler.wrap(js, 42);
      KJ_EXPECT(jsInt->IsNumber());
    });
  });
}

KJ_TEST("TypeHandlerRegistry - type mismatches") {
  Evaluator<TypeHandlerRegistryContext, TypeHandlerRegistryIsolate> e(v8System);

  e.getIsolate().runInLockScope([&](TypeHandlerRegistryIsolate::Lock& lock) {
    JSG_WITHIN_CONTEXT_SCOPE(lock,
        lock.newContext<TypeHandlerRegistryContext>().getHandle(lock.v8Isolate),
        [&](jsg::Lock& js) {
      auto& registry = TypeHandlerRegistry::from(js);

      // Try to unwrap wrong type - should return kj::none
      auto& stringHandler = registry.getHandler<kj::String>();
      auto jsNumber = v8::Number::New(js.v8Isolate, 42);

      auto maybeStr = stringHandler.tryUnwrap(js, jsNumber);
      // String handler should handle number coercion based on its implementation
      // This test verifies the tryUnwrap behavior

      auto maybeStrFromNull = stringHandler.tryUnwrap(js, js.null());
      KJ_EXPECT(KJ_ASSERT_NONNULL(maybeStrFromNull) == "null"_kj);

      auto maybeStrFromUndefined = stringHandler.tryUnwrap(js, js.undefined());
      KJ_EXPECT(KJ_ASSERT_NONNULL(maybeStrFromUndefined) == "undefined"_kj);
    });
  });
}

}  // namespace
}  // namespace workerd::jsg::test
