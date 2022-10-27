// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include <capnp/serialize-text.h>
#include <workerd/jsg/jsg-test.h>
#include <workerd/jsg/rtti.h>

struct MockConfig {};

namespace workerd::jsg::rtti {
namespace {

template<typename T>
kj::String tType() {
  // returns textual encoding of rtti.
  Builder<MockConfig> builder((MockConfig()));
  auto type = builder.type<T>();
  capnp::TextCodec codec;
  return codec.encode(type);
}

template<typename T>
kj::String tStructure() {
  // returns textual encoding of structure.
  Builder<MockConfig> builder((MockConfig()));
  auto type = builder.structure<T>();
  capnp::TextCodec codec;
  return codec.encode(type);
}

KJ_TEST("primitive types") {
  KJ_EXPECT(tType<void>() == "(voidt = void)");
  KJ_EXPECT(tType<bool>() == "(boolt = void)");
  KJ_EXPECT(tType<v8::Value>() == "(unknown = void)");
}

KJ_TEST("number types") {
  KJ_EXPECT(tType<char>() == "(number = (name = \"char\"))");
  KJ_EXPECT(tType<signed char>() == "(number = (name = \"signed char\"))");
  KJ_EXPECT(tType<unsigned char>() == "(number = (name = \"unsigned char\"))");
  KJ_EXPECT(tType<short>() == "(number = (name = \"short\"))");
  KJ_EXPECT(tType<unsigned short>() == "(number = (name = \"unsigned short\"))");
  KJ_EXPECT(tType<int>() == "(number = (name = \"int\"))");
  KJ_EXPECT(tType<unsigned int>() == "(number = (name = \"unsigned int\"))");
  KJ_EXPECT(tType<long>() == "(number = (name = \"long\"))");
  KJ_EXPECT(tType<unsigned long>() == "(number = (name = \"unsigned long\"))");

  KJ_EXPECT(tType<double>() == "(number = (name = \"double\"))");
}

KJ_TEST("string types") {
  KJ_EXPECT(tType<kj::String>() == "(string = (name = \"kj::String\"))");
  KJ_EXPECT(tType<kj::StringPtr>() == "(string = (name = \"kj::StringPtr\"))");
  KJ_EXPECT(tType<v8::String>() == "(string = (name = \"v8::String\"))");
  KJ_EXPECT(tType<ByteString>() == "(string = (name = \"ByteString\"))");
  KJ_EXPECT(tType<UsvString>() == "(string = (name = \"UsvString\"))");
  KJ_EXPECT(tType<UsvStringPtr>() == "(string = (name = \"UsvStringPtr\"))");
}

KJ_TEST("object types") {
  KJ_EXPECT(tType<v8::Object>() == "(object = void)");
  KJ_EXPECT(tType<jsg::Object>() == "(object = void)");
}

KJ_TEST("promises") {
  KJ_EXPECT(tType<kj::Promise<void>>() ==
      "(promise = (value = (voidt = void)))");
  KJ_EXPECT(tType<kj::Promise<int>>() ==
      "(promise = (value = (number = (name = \"int\"))))");
  KJ_EXPECT(tType<jsg::Promise<int>>() ==
      "(promise = (value = (number = (name = \"int\"))))");
  KJ_EXPECT(tType<v8::Promise>() ==
      "(promise = (value = (unknown = void)))");
}

KJ_TEST("generic types") {
  KJ_EXPECT(tType<Ref<v8::Object>>() == "(object = void)");
  KJ_EXPECT(tType<V8Ref<v8::Object>>() == "(object = void)");
  KJ_EXPECT(tType<HashableV8Ref<v8::Object>>() == "(object = void)");
  KJ_EXPECT(tType<v8::Local<v8::Object>>() == "(object = void)");
  KJ_EXPECT(tType<jsg::Identified<v8::Object>>() == "(object = void)");
  KJ_EXPECT(tType<jsg::MemoizedIdentity<v8::Object>>() == "(object = void)");
  KJ_EXPECT(tType<jsg::NonCoercible<kj::String>>() == "(string = (name = \"kj::String\"))");

  KJ_EXPECT(tType<kj::Array<int>>() == "(array = (element = (number = (name = \"int\")), name = \"kj::Array\"))");
  KJ_EXPECT(tType<kj::ArrayPtr<int>>() == "(array = (element = (number = (name = \"int\")), name = \"kj::ArrayPtr\"))");
  KJ_EXPECT(tType<jsg::Sequence<int>>() == "(array = (element = (number = (name = \"int\")), name = \"jsg::Sequence\"))");

  KJ_EXPECT(tType<kj::Maybe<int>>() == "(maybe = (value = (number = (name = \"int\")), name = \"kj::Maybe\"))");
  KJ_EXPECT(tType<jsg::Optional<int>>() == "(maybe = (value = (number = (name = \"int\")), name = \"jsg::Optional\"))");
  KJ_EXPECT(tType<jsg::LenientOptional<int>>() == "(maybe = (value = (number = (name = \"int\")), name = \"jsg::LenientOptional\"))");

  KJ_EXPECT(tType<jsg::Dict<int>>() == "(dict = (key = (string = (name = \"kj::String\")), value = (number = (name = \"int\"))))");
  KJ_EXPECT((tType<jsg::Dict<int, double>>()) == "(dict = (key = (number = (name = \"double\")), value = (number = (name = \"int\"))))");

  KJ_EXPECT((tType<kj::OneOf<int, double>>()) == "(oneOf = (variants = ["
      "(number = (name = \"int\")), "
      "(number = (name = \"double\"))]))");
  KJ_EXPECT((tType<kj::OneOf<int, double, kj::String>>()) == "(oneOf = (variants = ["
      "(number = (name = \"int\")), "
      "(number = (name = \"double\")), "
      "(string = (name = \"kj::String\"))]))");
}

KJ_TEST("builtins") {
  KJ_EXPECT(tType<jsg::BufferSource>() == "(builtin = (type = jsgBufferSource))");
  KJ_EXPECT(tType<v8::Uint8Array>() == "(builtin = (type = v8Uint8Array))");
  KJ_EXPECT(tType<v8::ArrayBufferView>() == "(builtin = (type = v8ArrayBufferView))");
  KJ_EXPECT(tType<v8::Function>() == "(builtin = (type = v8Function))");
  KJ_EXPECT(tType<kj::Date>() == "(builtin = (type = kjDate))");
}

KJ_TEST("jsgImpl") {
  KJ_EXPECT(tType<jsg::Lock>() == "(jsgImpl = (type = jsgLock))");
  KJ_EXPECT(tType<jsg::SelfRef>() == "(jsgImpl = (type = jsgSelfRef))");
  KJ_EXPECT(tType<jsg::Unimplemented>() == "(jsgImpl = (type = jsgUnimplemented))");
  KJ_EXPECT(tType<jsg::Varargs>() == "(jsgImpl = (type = jsgVarargs))");
  KJ_EXPECT(tType<v8::Isolate*>() == "(jsgImpl = (type = v8Isolate))");
  KJ_EXPECT(tType<MockConfig>() == "(jsgImpl = (type = configuration))");
  KJ_EXPECT(tType<jsg::TypeHandler<kj::Date>>() == "(jsgImpl = (type = jsgTypeHandler))");
  KJ_EXPECT(tType<v8::FunctionCallbackInfo<v8::Value>>() == "(jsgImpl = (type = v8FunctionCallbackInfo))");
  KJ_EXPECT(tType<v8::PropertyCallbackInfo<v8::Value>>() == "(jsgImpl = (type = v8PropertyCallbackInfo))");
}

KJ_TEST("functions") {
  KJ_EXPECT(tType<jsg::Function<int()>>() ==
      "(function = (returnType = (number = (name = \"int\")), args = []))");
  KJ_EXPECT(tType<jsg::Function<void(int a, double b)>>() ==
      "(function = (returnType = (voidt = void), args = [(number = (name = \"int\")), (number = (name = \"double\"))]))");
}

KJ_TEST("c++ modifiers") {
  KJ_EXPECT(tType<const int>() == "(number = (name = \"int\"))");
  KJ_EXPECT(tType<int&>() == "(number = (name = \"int\"))");
  KJ_EXPECT(tType<int&&>() == "(number = (name = \"int\"))");
  KJ_EXPECT(tType<const int&>() == "(number = (name = \"int\"))");
}

struct Base: public Object {
  JSG_RESOURCE_TYPE(Base) {
    JSG_INHERIT_INTRINSIC(v8::kIteratorPrototype);
  }
};

struct TestResource: public Base {
  void instanceMethod(int i, double f) { }
  static int staticMethod() { return 42; }

  int getSize() { return 1; }
  void setSize(int size) { }

  static jsg::Ref<TestResource> constructor(jsg::Optional<kj::String> label);

  JSG_RESOURCE_TYPE(TestResource) {
    JSG_INHERIT(Base);

    JSG_METHOD(instanceMethod);
    JSG_STATIC_METHOD(staticMethod);
    JSG_INSTANCE_PROPERTY(size, getSize, setSize);
    JSG_READONLY_INSTANCE_PROPERTY(readonlySize, getSize);
    JSG_LAZY_INSTANCE_PROPERTY(lazySize, getSize);
    JSG_LAZY_READONLY_INSTANCE_PROPERTY(lazyReadonlySize, getSize);
    JSG_PROTOTYPE_PROPERTY(protoSize, getSize, setSize);
    JSG_READONLY_PROTOTYPE_PROPERTY(protoReadonlySize, getSize);
  }
};

KJ_TEST("resource reference") {
  KJ_EXPECT(tType<TestResource>() == "(structure = (name = \"TestResource\", fullyQualifiedName = \"workerd::jsg::rtti::(anonymous namespace)::TestResource\"))");
}

KJ_TEST("resource structure") {
  KJ_EXPECT(tStructure<Base>() == "(name = \"Base\", members = [], "
      "extends = (intrinsic = (name = \"v8::kIteratorPrototype\")), "
      "iterable = false, asyncIterable = false, "
      "fullyQualifiedName = \"workerd::jsg::rtti::(anonymous namespace)::Base\")");

  KJ_EXPECT(tStructure<TestResource>() == "(name = \"TestResource\", members = ["
      "(method = (name = \"instanceMethod\", returnType = (voidt = void), args = [(number = (name = \"int\")), (number = (name = \"double\"))], static = false)), "
      "(method = (name = \"staticMethod\", returnType = (number = (name = \"int\")), args = [], static = true)), "
      "(property = (name = \"size\", type = (number = (name = \"int\")), readonly = false, lazy = false, prototype = false)), "
      "(property = (name = \"readonlySize\", type = (number = (name = \"int\")), readonly = true, lazy = false, prototype = false)), "
      "(property = (name = \"lazySize\", type = (number = (name = \"int\")), readonly = false, lazy = true, prototype = false)), "
      "(property = (name = \"lazyReadonlySize\", type = (number = (name = \"int\")), readonly = true, lazy = true, prototype = false)), "
      "(property = (name = \"protoSize\", type = (number = (name = \"int\")), readonly = false, lazy = false, prototype = true)), "
      "(property = (name = \"protoReadonlySize\", type = (number = (name = \"int\")), readonly = true, lazy = false, prototype = true)), "
      "(constructor = (args = [(maybe = (value = (string = (name = \"kj::String\")), name = \"jsg::Optional\"))]))], "
      "extends = (structure = (name = \"Base\", fullyQualifiedName = \"workerd::jsg::rtti::(anonymous namespace)::Base\")), "
      "iterable = false, asyncIterable = false, "
      "fullyQualifiedName = \"workerd::jsg::rtti::(anonymous namespace)::TestResource\")");
}

struct TestNested : jsg::Object {
  JSG_RESOURCE_TYPE(TestNested) { JSG_NESTED_TYPE(Base); };
};

KJ_TEST("nested structure") {
  KJ_EXPECT(tStructure<TestNested>() == "(name = \"TestNested\", members = [("
    "nested = ("
    "structure = ("
    "name = \"Base\", members = [], "
    "extends = (intrinsic = (name = \"v8::kIteratorPrototype\")), "
    "iterable = false, asyncIterable = false, "
    "fullyQualifiedName = \"workerd::jsg::rtti::(anonymous namespace)::Base\""
    "), "
    "name = \"Base\"))"
    "], "
    "iterable = false, asyncIterable = false, "
    "fullyQualifiedName = \"workerd::jsg::rtti::(anonymous namespace)::TestNested\")");
}

struct TestConstant : jsg::Object {
  static constexpr int ENABLED [[maybe_unused]] = 1;

  enum Type {
    CIRCLE = 2,
  };

  JSG_RESOURCE_TYPE(TestConstant) {
    JSG_STATIC_CONSTANT(ENABLED);
    JSG_STATIC_CONSTANT(CIRCLE);
  };
};

KJ_TEST("constant members") {
  KJ_EXPECT(tStructure<TestConstant>() == "(name = \"TestConstant\", members = ["
    "(constant = (name = \"ENABLED\", value = 1)), "
    "(constant = (name = \"CIRCLE\", value = 2))], "
    "iterable = false, asyncIterable = false, "
    "fullyQualifiedName = \"workerd::jsg::rtti::(anonymous namespace)::TestConstant\")");
}

struct TestStruct {
  int a;
  bool b;
  JSG_STRUCT(a, b);
};

KJ_TEST("struct reference") {
  KJ_EXPECT(tType<TestStruct>() == "(structure = (name = \"TestStruct\", fullyQualifiedName = \"workerd::jsg::rtti::(anonymous namespace)::TestStruct\"))");
}

KJ_TEST("struct structure") {
  KJ_EXPECT(tStructure<TestStruct>() == "(name = \"TestStruct\", members = ["
      "(property = (name = \"a\", type = (number = (name = \"int\")), readonly = false, lazy = false, prototype = false)), "
      "(property = (name = \"b\", type = (boolt = void), readonly = false, lazy = false, prototype = false))], "
      "iterable = false, asyncIterable = false, "
      "fullyQualifiedName = \"workerd::jsg::rtti::(anonymous namespace)::TestStruct\")");
}

struct TestSymbolTable: public jsg::Object {
  void acceptResource(const TestResource& resource) {};
  void recursiveTypeFunction(const TestSymbolTable& table) { }

  JSG_RESOURCE_TYPE(TestSymbolTable) {
    JSG_METHOD(acceptResource);
    JSG_METHOD(recursiveTypeFunction);
  };
};

KJ_TEST("symbol table") {
  Builder<MockConfig> builder((MockConfig()));
  auto type = builder.structure<TestSymbolTable>();
  capnp::TextCodec codec;

  KJ_EXPECT(codec.encode(type) == "(name = \"TestSymbolTable\", members = ["
      "(method = (name = \"acceptResource\", returnType = (voidt = void), args = [(structure = (name = \"TestResource\", fullyQualifiedName = \"workerd::jsg::rtti::(anonymous namespace)::TestResource\"))], static = false)), "
      "(method = (name = \"recursiveTypeFunction\", returnType = (voidt = void), args = [(structure = (name = \"TestSymbolTable\", fullyQualifiedName = \"workerd::jsg::rtti::(anonymous namespace)::TestSymbolTable\"))], static = false))], "
      "iterable = false, asyncIterable = false, "
      "fullyQualifiedName = \"workerd::jsg::rtti::(anonymous namespace)::TestSymbolTable\")");

  KJ_EXPECT(builder.structure("workerd::jsg::rtti::(anonymous namespace)::TestSymbolTable"_kj) != nullptr);
  KJ_EXPECT(builder.structure("workerd::jsg::rtti::(anonymous namespace)::TestResource"_kj) != nullptr);
  KJ_EXPECT(KJ_REQUIRE_NONNULL(builder.structure("workerd::jsg::rtti::(anonymous namespace)::TestResource"_kj)).getMembers().size() > 0);
}

} // namespace
} // namespace workerd::jsg::rtti
