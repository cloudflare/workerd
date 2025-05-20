// Copyright (c) 2017-2022 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

#include "jsg-test.h"
#include "ser.h"

namespace workerd::jsg::test {
namespace {

V8System v8System;
class ContextGlobalObject: public jsg::Object, public ContextGlobal {};

kj::Array<kj::byte> lastSerializedData;

struct SerTestContext: public ContextGlobalObject {
  enum class SerializationTag {
    FOO,
    BAR,
    BAZ,
    QUX,
  };

  struct Foo: public jsg::Object {
    uint i;
    Foo(uint i): i(i) {}

    static jsg::Ref<Foo> constructor(jsg::Lock& js, uint i) {
      return js.alloc<Foo>(i);
    }

    int getI() {
      return i;
    }

    JSG_RESOURCE_TYPE(Foo) {
      JSG_READONLY_PROTOTYPE_PROPERTY(i, getI);
    }

    void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
      serializer.writeRawUint32(i);
    }
    static jsg::Ref<Foo> deserialize(Lock& js, SerializationTag tag, Deserializer& deserializer) {
      KJ_ASSERT(tag == SerializationTag::FOO);

      // Intentionally deserialize differently so we can detect it.
      return js.alloc<Foo>(deserializer.readRawUint32() + 2);
    }
    JSG_SERIALIZABLE(SerializationTag::FOO);
  };

  struct Bar: public jsg::Object {
    kj::String text;
    Bar(kj::String text): text(kj::mv(text)) {}

    static jsg::Ref<Bar> constructor(jsg::Lock& js, kj::String text) {
      return js.alloc<Bar>(kj::mv(text));
    }

    kj::String getText() {
      return kj::str(text);
    }

    JSG_RESOURCE_TYPE(Bar) {
      JSG_READONLY_PROTOTYPE_PROPERTY(text, getText);
    }

    void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
      serializer.writeRawUint64(text.size());
      serializer.writeRawBytes(text.asBytes());
    }
    static jsg::Ref<Bar> deserialize(Lock& js, SerializationTag tag, Deserializer& deserializer) {
      KJ_ASSERT(tag == SerializationTag::BAR);

      size_t size = deserializer.readRawUint64();
      auto bytes = deserializer.readRawBytes(size);
      // Intentionally deserialize differently so we can detect it.
      return js.alloc<Bar>(kj::str(bytes.asChars(), '!'));
    }
    JSG_SERIALIZABLE(SerializationTag::BAR);
  };

  struct Baz: public jsg::Object {
    bool serializeThrows;
    Baz(bool serializeThrows): serializeThrows(serializeThrows) {}
    static jsg::Ref<Baz> constructor(jsg::Lock& js, bool serializeThrows) {
      return js.alloc<Baz>(serializeThrows);
    }

    JSG_RESOURCE_TYPE(Baz) {}

    void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
      JSG_REQUIRE(!serializeThrows, Error, "throw from serialize()");
    }
    static jsg::Ref<Bar> deserialize(Lock& js, SerializationTag tag, Deserializer& deserializer) {
      JSG_FAIL_REQUIRE(Error, "throw from deserialize()");
    }
    JSG_SERIALIZABLE(SerializationTag::BAZ);
  };

  // Qux is like Bar but serializes its string by converting it to a JS value first.
  struct Qux: public jsg::Object {
    kj::String text;
    Qux(kj::String text): text(kj::mv(text)) {}

    static jsg::Ref<Qux> constructor(jsg::Lock& js, kj::String text) {
      return js.alloc<Qux>(kj::mv(text));
    }

    kj::String getText() {
      return kj::str(text);
    }

    JSG_RESOURCE_TYPE(Qux) {
      JSG_READONLY_PROTOTYPE_PROPERTY(text, getText);
    }

    void serialize(
        jsg::Lock& js, jsg::Serializer& serializer, const TypeHandler<kj::String>& stringHandler) {
      // V2 prefers to serialize the string as a JS value.
      serializer.write(js, JsValue(stringHandler.wrap(js, kj::str(text, '?'))));
    }
    static jsg::Ref<Qux> deserialize(Lock& js,
        SerializationTag tag,
        Deserializer& deserializer,
        const TypeHandler<kj::String>& stringHandler) {
      KJ_ASSERT(tag == SerializationTag::QUX);

      return js.alloc<Qux>(
          KJ_ASSERT_NONNULL(stringHandler.tryUnwrap(js, deserializer.readValue(js))));
    }
    JSG_SERIALIZABLE(SerializationTag::QUX);
  };

  JsValue roundTrip(Lock& js, JsValue in) {
    auto content = ({
      Serializer ser(js);
      ser.write(js, in);
      ser.release();
    });

    auto result = ({
      Deserializer deser(js, content);
      deser.readValue(js);
    });

    // Save the last serialization off to the side.
    lastSerializedData = kj::mv(content.data);

    return result;
  }

  JSG_RESOURCE_TYPE(SerTestContext) {
    JSG_NESTED_TYPE(Foo);
    JSG_NESTED_TYPE(Bar);
    JSG_NESTED_TYPE(Baz);
    JSG_NESTED_TYPE(Qux);
    JSG_METHOD(roundTrip);
  }
};
JSG_DECLARE_ISOLATE_TYPE(SerTestIsolate,
    SerTestContext,
    SerTestContext::Foo,
    SerTestContext::Bar,
    SerTestContext::Baz,
    SerTestContext::Qux);

// Define a whole second JSG isolate type that contains "updated" code where Bar no longer wraps
// a string, it wraps an arbitrary value.
struct SerTestContextV2: public ContextGlobalObject {
  enum class SerializationTag { FOO, BAR_OLD, BAZ, QUX, BAR_V2 };

  struct Bar: public jsg::Object {
    JsRef<JsValue> val;
    Bar(JsRef<JsValue> val): val(kj::mv(val)) {}

    static jsg::Ref<Bar> constructor(jsg::Lock& js, JsRef<JsValue> val) {
      return js.alloc<Bar>(kj::mv(val));
    }

    JsRef<JsValue> getVal(Lock& js) {
      return val.addRef(js);
    }

    JSG_RESOURCE_TYPE(Bar) {
      JSG_READONLY_PROTOTYPE_PROPERTY(val, getVal);
    }

    void serialize(jsg::Lock& js, jsg::Serializer& serializer) {
      // V2 just writes a value!
      serializer.write(js, JsValue(val.getHandle(js)));
    }
    static jsg::Ref<Bar> deserialize(Lock& js, SerializationTag tag, Deserializer& deserializer) {
      if (tag == SerializationTag::BAR_OLD) {
        // Oh, it's an old value.
        size_t size = deserializer.readRawUint64();
        auto bytes = deserializer.readRawBytes(size);

        return js.alloc<Bar>(JsRef<JsValue>(js, js.str(kj::str("old:", bytes.asChars()))));
      } else {
        KJ_ASSERT(tag == SerializationTag::BAR_V2);

        return js.alloc<Bar>(JsRef<JsValue>(js, deserializer.readValue(js)));
      }
    }
    JSG_SERIALIZABLE(SerializationTag::BAR_V2, SerializationTag::BAR_OLD);
  };

  JsValue roundTrip(Lock& js, JsValue in) {
    auto content = ({
      Serializer ser(js);
      ser.write(js, in);
      ser.release();
    });

    auto result = ({
      Deserializer deser(js, content);
      deser.readValue(js);
    });

    // Save the last serialization off to the side.
    lastSerializedData = kj::mv(content.data);

    return result;
  }

  JsValue deserializeLast(Lock& js) {
    Deserializer deser(js, lastSerializedData);
    return deser.readValue(js);
  }

  JSG_RESOURCE_TYPE(SerTestContextV2) {
    JSG_NESTED_TYPE(Bar);
    JSG_METHOD(roundTrip);
    JSG_METHOD(deserializeLast);
  }
};
JSG_DECLARE_ISOLATE_TYPE(SerTestIsolateV2, SerTestContextV2, SerTestContextV2::Bar);

KJ_TEST("serialization") {
  Evaluator<SerTestContext, SerTestIsolate> e(v8System);

  // Test serializing built-in values.
  e.expectEval("roundTrip(123)", "number", "123");
  e.expectEval("JSON.stringify(roundTrip({foo: 123}))", "string", "{\"foo\":123}");

  // Test serializing host objects.
  e.expectEval("roundTrip(new Foo(123)).i", "number", "125");
  e.expectEval("roundTrip(new Qux(\"hello\")).text", "string", "hello?");
  e.expectEval("roundTrip(new Bar(\"hello\")).text", "string", "hello!");

  // Test throwing from serialize/deserialize
  e.expectEval("roundTrip(new Baz(true)).text", "throws", "Error: throw from serialize()");
  e.expectEval("roundTrip(new Baz(false)).text", "throws", "Error: throw from deserialize()");

  // Let's set up the "new version" of the code.
  Evaluator<SerTestContextV2, SerTestIsolateV2> e2(v8System);

  // This will deserialize the last-serialized bytes from above, where we serialized Bar("hello").
  // However, it is using a "new version" of the code where Bar's serialization has changed, but
  // the old version is still accepted.
  e2.expectEval("deserializeLast().val", "string", "old:hello");

  // Also try round-tripping the new version. It now accepts arbitrary values, not just strings.
  e2.expectEval("roundTrip(new Bar(123)).val", "number", "123");

  // Note that cycles through host objects are correctly serialized!
  //
  // V8 BUG ALERT: The below works if we use `obj` as the root of serialization, but NOT if we
  //   use `bar` as the root. The reason is a flaw in the design of V8's callbacks for parsing
  //   host objects. V8 makes a single callback to the embedder which fully reads the object and
  //   returns a handle. However, this means that V8 cannot put the object into the backreference
  //   table until this callback returns. If, while parsing the object, we encounter a
  //   backreference to the object itself (a cycle), the deserializer will find the backreference
  //   is not in the table and therefore raises an error. This is not a problem for native objects
  //   because V8 allocates the object first, then immediately adds it to the backreference table,
  //   and only then parses its content -- and this is why everything works fine if we start with
  //   a native object as the root, as in this test. The API for host objects needs to be extended
  //   somehow to allow the object to be inserted into the table before parsing its content.
  e2.expectEval("let obj = {i: 321};\n"
                "let bar = new Bar(obj);\n"
                "obj.bar = bar;\n"
                "roundTrip(obj).bar.val.bar.val.bar.val.i",
      "number", "321");
}

}  // namespace
}  // namespace workerd::jsg::test
