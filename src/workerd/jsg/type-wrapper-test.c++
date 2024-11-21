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

  v8::Local<v8::Number> wrap(v8::Local<v8::Context> context,
      kj::Maybe<v8::Local<v8::Object>> creator,
      TestExtensionType value) {
    return v8::Number::New(context->GetIsolate(), value.value);
  }

  v8::Local<v8::Context> newContext(v8::Isolate* isolate, TestExtensionType value) = delete;

  kj::Maybe<TestExtensionType> tryUnwrap(v8::Local<v8::Context> context,
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
    return handler.wrap(js, alloc<NumberBox>(value));
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
    static Ref<UnimplementedConstructorParam> constructor(int i, Unimplemented) {
      return jsg::alloc<UnimplementedConstructorParam>(i);
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
    static Ref<UnimplementedProperties> constructor() {
      return jsg::alloc<UnimplementedProperties>();
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

}  // namespace
}  // namespace workerd::jsg::test
